/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-admin-builtins.h"
#include "ostree.h"

#include <glib/gi18n.h>
#include <sys/utsname.h>

typedef struct {
  OtAdminBuiltinOpts *admin_opts;
  GFile       *ostree_dir;
  GFile       *boot_ostree_dir;
  GFile       *deploy_path;
  GFile       *kernel_path;
  char        *release;
  char        *osname;
} OtAdminUpdateKernel;

static gboolean opt_no_bootloader;

static GOptionEntry options[] = {
  { "no-bootloader", 0, 0, G_OPTION_ARG_NONE, &opt_no_bootloader, "Don't update bootloader", NULL },
  { NULL }
};

static gboolean
get_kernel_from_boot (GFile         *path,
                      GFile        **out_kernel,
                      GCancellable  *cancellable,
                      GError       **error)
{
  gboolean ret = FALSE;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GFile *ret_kernel = NULL;

  dir_enum = g_file_enumerate_children (path, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, error);
  if (!dir_enum)
    goto out;

  while ((file_info = g_file_enumerator_next_file (dir_enum, cancellable, error)) != NULL)
    {
      const char *name;

      name = g_file_info_get_name (file_info);

      if (!g_str_has_prefix (name, "vmlinuz-"))
        continue;

      ret_kernel = g_file_get_child (path, name);
      break;
    }

  ot_transfer_out_value (out_kernel, &ret_kernel);
  ret = TRUE;
 out:
  return ret;
}

static gboolean
setup_kernel (OtAdminUpdateKernel *self,
              GCancellable        *cancellable,
              GError             **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *deploy_boot_path = NULL;
  ot_lobj GFile *src_kernel_path = NULL;
  ot_lfree char *prefix = NULL;
  const char *release = NULL;
  const char *kernel_name = NULL;

  deploy_boot_path = g_file_get_child (self->deploy_path, "boot"); 

  if (!get_kernel_from_boot (deploy_boot_path, &src_kernel_path,
                             cancellable, error))
    goto out;
  if (src_kernel_path == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No kernel found in %s", gs_file_get_path_cached (deploy_boot_path));
      goto out;
    }

  if (!gs_file_ensure_directory (self->boot_ostree_dir, TRUE, cancellable, error))
    goto out;

  kernel_name = gs_file_get_basename_cached (src_kernel_path);
  release = strchr (kernel_name, '-');
  if (release == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid kernel name %s, no - found", gs_file_get_path_cached (src_kernel_path));
      goto out;
    }

  self->release = g_strdup (release + 1);
  prefix = g_strndup (kernel_name, release - kernel_name);
  self->kernel_path = ot_gfile_get_child_strconcat (self->boot_ostree_dir, prefix, "-", self->release, NULL);

  if (!gs_file_linkcopy_sync_data (src_kernel_path, self->kernel_path, G_FILE_COPY_OVERWRITE,
                                   cancellable, error))
    goto out;

  g_print ("ostadmin: Deploying kernel %s\n", gs_file_get_path_cached (self->kernel_path));
      
  ret = TRUE;
 out:
  if (error)
    g_prefix_error (error, "Error copying kernel: ");
  return ret;
}

static gboolean
update_initramfs (OtAdminUpdateKernel  *self,
                  GCancellable         *cancellable,
                  GError              **error)
{
  gboolean ret = FALSE;
  ot_lfree char *initramfs_name = NULL;
  ot_lobj GFile *initramfs_file = NULL;

  initramfs_name = g_strconcat ("initramfs-", self->release, ".img", NULL);

  initramfs_file = g_file_get_child (self->boot_ostree_dir, initramfs_name);
  if (!g_file_query_exists (initramfs_file, NULL))
    {
      gs_unref_ptrarray GPtrArray *mkinitramfs_args = NULL;
      gs_unref_object GSSubprocess *proc = NULL;
      ot_lobj GFile *tmpdir = NULL;
      ot_lfree char *initramfs_tmp_path = NULL;
      ot_lobj GFile *ostree_vardir = NULL;
      ot_lobj GFile *initramfs_tmp_file = NULL;
      ot_lobj GFileInfo *initramfs_tmp_info = NULL;
      ot_lobj GFile *dracut_log_path = NULL;
      ot_lobj GOutputStream *tmp_log_out = NULL;
          
      if (!ostree_create_temp_dir (NULL, "ostree-initramfs", NULL, &tmpdir,
                                   cancellable, error))
        goto out;

      ostree_vardir = ot_gfile_get_child_build_path (self->ostree_dir, "deploy",
                                                     self->osname, "var", NULL);

      dracut_log_path = ot_gfile_get_child_build_path (ostree_vardir, "log", "dracut.log", NULL);
      tmp_log_out = (GOutputStream*)g_file_replace (dracut_log_path, NULL, FALSE,
                                                    G_FILE_CREATE_REPLACE_DESTINATION,
                                                    cancellable, error);
      if (!tmp_log_out)
        goto out;
      if (!g_output_stream_close (tmp_log_out, cancellable, error))
        goto out;

      mkinitramfs_args = g_ptr_array_new ();
      /* Note: the hardcoded /tmp path below is not actually a
       * security flaw, because we've bind-mounted dracut's view
       * of /tmp to the securely-created tmpdir above.
       */
      ot_ptrarray_add_many (mkinitramfs_args,
                            "linux-user-chroot",
                            "--mount-readonly", "/",
                            "--mount-proc", "/proc",
                            "--mount-bind", "/dev", "/dev",
                            "--mount-bind", gs_file_get_path_cached (ostree_vardir), "/var",
                            "--mount-bind", gs_file_get_path_cached (tmpdir), "/tmp", NULL);
      ot_ptrarray_add_many (mkinitramfs_args, gs_file_get_path_cached (self->deploy_path),
                            "dracut", "--tmpdir=/tmp", "-f", "/tmp/initramfs-ostree.img", self->release,
                            NULL);
      g_ptr_array_add (mkinitramfs_args, NULL);
      
      g_print ("Generating initramfs using %s...\n", gs_file_get_path_cached (self->deploy_path));
      proc = gs_subprocess_new_simple_argv ((gchar**)mkinitramfs_args->pdata,
                                            GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                            GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                            cancellable, error);
      if (!proc)
        goto out;
      if (!gs_subprocess_wait_sync_check (proc, cancellable, error))
        goto out;
          
      initramfs_tmp_file = g_file_get_child (tmpdir, "initramfs-ostree.img");
      initramfs_tmp_info = g_file_query_info (initramfs_tmp_file, OSTREE_GIO_FAST_QUERYINFO,
                                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                              cancellable, error);
      if (!initramfs_tmp_info)
        goto out;

      if (g_file_info_get_size (initramfs_tmp_info) == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Initramfs generation failed, check dracut.log");
          goto out;
        }

      if (!gs_file_chmod (initramfs_tmp_file, 0644, cancellable, error))
        {
          g_prefix_error (error, "Failed to chmod initramfs: ");
          goto out;
        }

      if (!gs_file_linkcopy_sync_data (initramfs_tmp_file, initramfs_file, G_FILE_COPY_OVERWRITE,
                                       cancellable, error))
        goto out;
      
      /* In the fuse case, we need to chown after copying */
      if (getuid () != 0)
        {
          if (!gs_file_chown (initramfs_file, 0, 0, cancellable, error))
            {
              g_prefix_error (error, "Failed to chown initramfs: ");
              goto out;
            }
        }
          
      g_print ("Created: %s\n", gs_file_get_path_cached (initramfs_file));

      if (!gs_shutil_rm_rf (tmpdir, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
grep_literal (GFile              *f,
              const char         *string,
              gboolean           *out_matches,
              GCancellable       *cancellable,
              GError            **error)
{
  gboolean ret = FALSE;
  gboolean ret_matches = FALSE;
  ot_lobj GInputStream *in = NULL;
  ot_lobj GDataInputStream *datain = NULL;
  ot_lfree char *line = NULL;

  in = (GInputStream*)g_file_read (f, cancellable, error);
  if (!in)
    goto out;
  datain = (GDataInputStream*)g_data_input_stream_new (in);
  if (!in)
    goto out;

  while ((line = g_data_input_stream_read_line (datain, NULL, cancellable, error)) != NULL)
    {
      if (strstr (line, string))
        {
          ret_matches = TRUE;
          break;
        }
      
      g_free (line);
    }

  ret = TRUE;
  if (out_matches)
    *out_matches = ret_matches;
 out:
  return ret;
}

static gboolean
update_grub (OtAdminUpdateKernel  *self,
             GCancellable         *cancellable,
             GError              **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *grub_path = g_file_resolve_relative_path (self->admin_opts->boot_dir, "grub/grub.conf");

  if (g_file_query_exists (grub_path, cancellable))
    {
      gboolean have_grub_entry;
      if (!grep_literal (grub_path, "OSTree", &have_grub_entry,
                         cancellable, error))
        goto out;

      if (!have_grub_entry)
        {
          ot_lfree char *add_kernel_arg = NULL;
          ot_lfree char *initramfs_arg = NULL;
          ot_lfree char *initramfs_name = NULL;
          ot_lobj GFile *initramfs_path = NULL;

          initramfs_name = g_strconcat ("initramfs-", self->release, ".img", NULL);
          initramfs_path = g_file_get_child (self->boot_ostree_dir, initramfs_name);

          add_kernel_arg = g_strconcat ("--add-kernel=", gs_file_get_path_cached (self->kernel_path), NULL);
          initramfs_arg = g_strconcat ("--initrd=", gs_file_get_path_cached (initramfs_path), NULL);

          g_print ("Adding OSTree grub entry...\n");
          if (!gs_subprocess_simple_run_sync (NULL, GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
                                              cancellable, error,
                                              "grubby", "--grub", add_kernel_arg, initramfs_arg,
                                              "--copy-default", "--title=OSTree", NULL))
            goto out;
        } 
      else
        g_print ("Already have OSTree entry in grub config\n");
    }
  else
    {
      g_print ("/boot/grub/grub.conf not found, assuming you have GRUB 2\n");
    }
  
  ret = TRUE;
 out:
  return ret;
}

gboolean
ot_admin_builtin_update_kernel (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error)
{
  GOptionContext *context;
  OtAdminUpdateKernel self_data;
  OtAdminUpdateKernel *self = &self_data;
  GFile *ostree_dir = admin_opts->ostree_dir;
  gboolean ret = FALSE;
  GCancellable *cancellable = NULL;

  memset (self, 0, sizeof (*self));

  self->admin_opts = admin_opts;

  context = g_option_context_new ("OSNAME [DEPLOY_PATH] - Update kernel and regenerate initial ramfs");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "OSNAME must be specified", error);
      goto out;
    }

  self->osname = g_strdup (argv[1]);

  if (argc > 2)
    self->deploy_path = g_file_new_for_path (argv[2]);
  else
    {
      ot_lobj GFile *osdir = ot_gfile_get_child_build_path (admin_opts->ostree_dir, "deploy", self->osname, NULL);
      self->deploy_path = g_file_get_child (osdir, "current");
    }

  self->ostree_dir = g_object_ref (ostree_dir);
  self->boot_ostree_dir = g_file_get_child (admin_opts->boot_dir, "ostree");
  
  if (!setup_kernel (self, cancellable, error))
    goto out;

  if (!update_initramfs (self, cancellable, error))
    goto out;
      
  if (!opt_no_bootloader)
    {
      if (!update_grub (self, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  g_clear_object (&self->ostree_dir);
  g_clear_object (&self->boot_ostree_dir);
  g_clear_object (&self->kernel_path);
  g_free (self->release);
  if (context)
    g_option_context_free (context);
  return ret;
}
