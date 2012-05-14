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
#include "otutil.h"

#include <glib/gi18n.h>
#include <sys/utsname.h>

static gboolean opt_checkout_only;

static GOptionEntry options[] = {
  { "checkout-only", 0, 0, G_OPTION_ARG_NONE, &opt_checkout_only, "Don't generate initramfs or update bootloader", NULL },
  { NULL }
};

static gboolean
update_initramfs (const char       *release,
                  const char       *last_deploy_target,
                  GCancellable     *cancellable,
                  GError          **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *dest_modules_parent = NULL;
  ot_lobj GFile *dest_modules_file = NULL;
  ot_lfree char *initramfs_name = NULL;
  ot_lobj GFile *initramfs_file = NULL;
  ot_lfree char *last_deploy_path = NULL;

  dest_modules_file = ot_gfile_from_build_path ("/ostree/modules", release, NULL);
  dest_modules_parent = g_file_get_parent (dest_modules_file);
  if (!ot_gfile_ensure_directory (dest_modules_parent, FALSE, error))
    goto out;
  if (!g_file_query_exists (dest_modules_file, NULL))
    {
      ot_lptrarray GPtrArray *cp_args = NULL;
      ot_lobj GFile *src_modules_file = ot_gfile_from_build_path ("/lib/modules", release, NULL);
          
      cp_args = g_ptr_array_new ();
      ot_ptrarray_add_many (cp_args, "cp", "-al", ot_gfile_get_path_cached (src_modules_file),
                            ot_gfile_get_path_cached (dest_modules_file), NULL);
      g_ptr_array_add (cp_args, NULL);

      g_print ("Copying kernel modules from %s\n", ot_gfile_get_path_cached (src_modules_file));
      if (!ot_spawn_sync_checked (NULL, (char**)cp_args->pdata, NULL,
                                  G_SPAWN_SEARCH_PATH,
                                  NULL, NULL, NULL, NULL, error))
        goto out;
    }
      
  initramfs_name = g_strconcat ("initramfs-ostree-", release, ".img", NULL);
  initramfs_file = ot_gfile_from_build_path ("/boot", initramfs_name, NULL);
  if (!g_file_query_exists (initramfs_file, NULL))
    {
      ot_lptrarray GPtrArray *mkinitramfs_args = NULL;
      ot_lfree char *tmpdir = NULL;
      ot_lfree char *initramfs_tmp_path = NULL;
      ot_lobj GFile *initramfs_tmp_file = NULL;
      ot_lobj GFileInfo *initramfs_tmp_info = NULL;
          
      if ((tmpdir = g_dir_make_tmp ("ostree-initramfs.XXXXXX", error)) == NULL)
        goto out;

      last_deploy_path = g_build_filename ("/ostree", last_deploy_target, NULL);

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
                            "--mount-bind", "/ostree/var", "/var",
                            "--mount-bind", tmpdir, "/tmp",
                            "--mount-bind", "/ostree/modules", "/lib/modules",
                            last_deploy_path,
                            "dracut", "-f", "/tmp/initramfs-ostree.img", release,
                            NULL);
      g_ptr_array_add (mkinitramfs_args, NULL);
          
      g_print ("Generating initramfs using %s...\n", last_deploy_path);
      if (!ot_spawn_sync_checked (NULL, (char**)mkinitramfs_args->pdata, NULL,
                                  G_SPAWN_SEARCH_PATH,
                                  NULL, NULL, NULL, NULL, error))
        {
          (void) unlink (initramfs_tmp_path);
          goto out;
        }
          
      initramfs_tmp_path = g_build_filename (tmpdir, "initramfs-ostree.img", NULL);
      initramfs_tmp_file = g_file_new_for_path (initramfs_tmp_path);
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

      if (!g_file_copy (initramfs_tmp_file, initramfs_file, 0, cancellable, NULL, NULL, error))
        goto out;
          
      g_print ("Created: %s\n", ot_gfile_get_path_cached (initramfs_file));

      (void) unlink (initramfs_tmp_path);
      (void) rmdir (tmpdir);
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
get_kernel_path_from_release (const char         *release,
                              GFile             **out_path,
                              GCancellable       *cancellable,
                              GError            **error)
{
  gboolean ret = FALSE;
  ot_lfree char *name = NULL;
  ot_lobj GFile *possible_path = NULL;

  /* TODO - replace this with grubby code */

  name = g_strconcat ("vmlinuz-", release, NULL);
  possible_path = ot_gfile_from_build_path ("/boot", name, NULL);
  if (!g_file_query_exists (possible_path, cancellable))
    g_clear_object (&possible_path);

  ret = TRUE;
  ot_transfer_out_value (out_path, &possible_path);
  /*  out: */
  return ret;
}

static gboolean
update_grub (const char         *release,
             GCancellable       *cancellable,
             GError            **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *grub_path = g_file_new_for_path ("/boot/grub/grub.conf");

  if (g_file_query_exists (grub_path, cancellable))
    {
      gboolean have_grub_entry;
      if (!grep_literal (grub_path, "OSTree", &have_grub_entry,
                         cancellable, error))
        goto out;

      if (!have_grub_entry)
        {
          ot_lptrarray GPtrArray *grubby_args = NULL;
          ot_lfree char *add_kernel_arg = NULL;
          ot_lfree char *initramfs_arg = NULL;
          ot_lobj GFile *kernel_path = NULL;

          if (!get_kernel_path_from_release (release, &kernel_path, cancellable, error))
            goto out;

          if (kernel_path == NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Couldn't find kernel for release %s", release);
              goto out;
            }

          grubby_args = g_ptr_array_new ();
          add_kernel_arg = g_strconcat ("--add-kernel=", ot_gfile_get_path_cached (kernel_path), NULL);
          initramfs_arg = g_strconcat ("--initrd=", "/boot/initramfs-ostree-", release, ".img", NULL);
          ot_ptrarray_add_many (grubby_args, "grubby", "--grub", add_kernel_arg, initramfs_arg,
                                "--copy-default", "--title=OSTree", NULL);
          g_ptr_array_add (grubby_args, NULL);

          g_print ("Adding OSTree grub entry...\n");
          if (!ot_spawn_sync_checked (NULL, (char**)grubby_args->pdata, NULL, G_SPAWN_SEARCH_PATH,
                                      NULL, NULL, NULL, NULL, error))
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

static gboolean
update_current (const char         *last_deploy_target,
                GCancellable       *cancellable,
                GError            **error)
{
  gboolean ret = FALSE;
  ot_lfree char *tmp_symlink = NULL;
  ot_lfree char *current_name = NULL;

  tmp_symlink = g_build_filename ("/ostree", "tmp-current", NULL);
  (void) unlink (tmp_symlink);

  if (symlink (last_deploy_target, tmp_symlink) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  current_name = g_build_filename ("/ostree", "current", NULL);
  if (rename (tmp_symlink, current_name) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  g_print ("/ostree/current set to %s\n", last_deploy_target);

  ret = TRUE;
 out:
  return ret;
}

gboolean
ot_admin_builtin_deploy (int argc, char **argv, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  int i;
  const char *last_deploy_target = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  context = g_option_context_new ("- Perform checkouts, ensure initramfs is generated");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 3)
    {
      ot_util_usage_error (context, "At least one REV must be specified", error);
      goto out;
    }

  for (i = 2; i < argc; i++)
    {
      const char *deploy_target = argv[i];
      ot_lfree char *tree_ref = NULL;
      ot_lptrarray GPtrArray *checkout_args = NULL;

      tree_ref = g_strconcat ("trees/", deploy_target, NULL);

      checkout_args = g_ptr_array_new ();
      ot_ptrarray_add_many (checkout_args, "ostree", "--repo=/ostree/repo",
                            "checkout", "--atomic-retarget", tree_ref, deploy_target, NULL);
      g_ptr_array_add (checkout_args, NULL);

      if (!ot_spawn_sync_checked ("/ostree", (char**)checkout_args->pdata, NULL, G_SPAWN_SEARCH_PATH,
                                  NULL, NULL, NULL, NULL, error))
        goto out;

      last_deploy_target = deploy_target;
    }

  if (!opt_checkout_only)
    {

      struct utsname utsname;
      const char *release;
  
      (void) uname (&utsname);
  
      if (strcmp (utsname.sysname, "Linux") != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unsupported machine %s", utsname.sysname);
          goto out;
        }
      
      release = utsname.release;

      if (!update_initramfs (release, last_deploy_target, cancellable, error))
        goto out;

      if (!update_grub (release, cancellable, error))
        goto out;
    }

  if (!update_current (last_deploy_target, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
