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
                      GFile        **out_initramfs,
                      GCancellable  *cancellable,
                      GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_object GFile *ret_kernel = NULL;
  gs_unref_object GFile *ret_initramfs = NULL;

  dir_enum = g_file_enumerate_children (path, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, error);
  if (!dir_enum)
    goto out;

  while ((file_info = g_file_enumerator_next_file (dir_enum, cancellable, error)) != NULL)
    {
      const char *name;

      name = g_file_info_get_name (file_info);

      if (ret_kernel == NULL && g_str_has_prefix (name, "vmlinuz-"))
        ret_kernel = g_file_get_child (path, name);
      else if (ret_initramfs == NULL && g_str_has_prefix (name, "initramfs-"))
        ret_initramfs = g_file_get_child (path, name);
      
      if (ret_kernel && ret_initramfs)
        break;
    }

  ot_transfer_out_value (out_kernel, &ret_kernel);
  ot_transfer_out_value (out_initramfs, &ret_initramfs);
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
  gs_unref_object GInputStream *in = NULL;
  gs_unref_object GDataInputStream *datain = NULL;
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
  gs_unref_object GFile *grub_path = g_file_resolve_relative_path (self->admin_opts->boot_dir, "grub/grub.conf");

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
          gs_unref_object GFile *initramfs_path = NULL;

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
  gboolean ret = FALSE;
  GOptionContext *context;
  OtAdminUpdateKernel self_data;
  OtAdminUpdateKernel *self = &self_data;
  GFile *ostree_dir = admin_opts->ostree_dir;
  gs_unref_object GFile *deploy_boot_path = NULL;
  gs_unref_object GFile *src_kernel_path = NULL;
  gs_unref_object GFile *src_initramfs_path = NULL;
  gs_free char *prefix = NULL;
  gs_free char *initramfs_name = NULL;
  gs_unref_object GFile *expected_initramfs_path = NULL;
  const char *release = NULL;
  const char *kernel_name = NULL;
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
      gs_unref_object GFile *osdir = ot_gfile_get_child_build_path (admin_opts->ostree_dir, "deploy", self->osname, NULL);
      self->deploy_path = g_file_get_child (osdir, "current");
    }

  self->ostree_dir = g_object_ref (ostree_dir);
  self->boot_ostree_dir = g_file_get_child (admin_opts->boot_dir, "ostree");

  deploy_boot_path = g_file_get_child (self->deploy_path, "boot"); 

  if (!get_kernel_from_boot (deploy_boot_path, &src_kernel_path, &src_initramfs_path,
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

  if (!g_file_query_exists(self->kernel_path, NULL))
    {
      if (!gs_file_linkcopy_sync_data (src_kernel_path, self->kernel_path, G_FILE_COPY_OVERWRITE,
                                       cancellable, error))
        goto out;
      g_print ("ostadmin: Deployed kernel %s\n", gs_file_get_path_cached (self->kernel_path));
    }

  initramfs_name = g_strconcat ("initramfs-", self->release, ".img", NULL);
  expected_initramfs_path = g_file_get_child (self->boot_ostree_dir, initramfs_name);

  if (!g_file_query_exists (expected_initramfs_path, NULL))
    {
      if (!gs_file_linkcopy_sync_data (src_initramfs_path, expected_initramfs_path, G_FILE_COPY_OVERWRITE,
                                       cancellable, error))
        goto out;
      
      /* In the fuse case, we need to chown after copying */
      if (getuid () != 0)
        {
          if (!gs_file_chown (expected_initramfs_path, 0, 0, cancellable, error))
            {
              g_prefix_error (error, "Failed to chown initramfs: ");
              goto out;
            }
        }

      g_print ("Deployed initramfs: %s\n", gs_file_get_path_cached (expected_initramfs_path));
    }
      
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
