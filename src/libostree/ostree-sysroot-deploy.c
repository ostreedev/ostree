/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012,2014 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include "ostree-sysroot-private.h"
#include "ostree-core-private.h"
#include "otutil.h"
#include "libgsystem.h"

#define OSTREE_DEPLOYMENT_COMPLETE_ID "dd440e3e549083b63d0efc7dc15255f1"

/**
 * copy_modified_config_file:
 *
 * Copy @file from @modified_etc to @new_etc, overwriting any existing
 * file there.  The @file may refer to a regular file, a symbolic
 * link, or a directory.  Directories will be copied recursively.
 *
 * Note this function does not (yet) handle the case where a directory
 * needed by a modified file is deleted in a newer tree.
 */
static gboolean
copy_modified_config_file (GFile              *orig_etc,
                           GFile              *modified_etc,
                           GFile              *new_etc,
                           GFile              *src,
                           GCancellable       *cancellable,
                           GError            **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileInfo *src_info = NULL;
  gs_unref_object GFileInfo *parent_info = NULL;
  gs_unref_object GFile *dest = NULL;
  gs_unref_object GFile *dest_parent = NULL;
  gs_free char *relative_path = NULL;
  
  relative_path = g_file_get_relative_path (modified_etc, src);
  g_assert (relative_path);
  dest = g_file_resolve_relative_path (new_etc, relative_path);

  src_info = g_file_query_info (src, OSTREE_GIO_FAST_QUERYINFO, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                cancellable, error);
  if (!src_info)
    goto out;

  dest_parent = g_file_get_parent (dest);
  if (!ot_gfile_query_info_allow_noent (dest_parent, OSTREE_GIO_FAST_QUERYINFO, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        &parent_info, cancellable, error))
    goto out;
  if (!parent_info)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "New tree removes parent directory '%s', cannot merge",
                   gs_file_get_path_cached (dest_parent));
      goto out;
    }

  if (!gs_shutil_rm_rf (dest, cancellable, error))
    goto out;

  if (g_file_info_get_file_type (src_info) == G_FILE_TYPE_DIRECTORY)
    {
      if (!gs_shutil_cp_a (src, dest, cancellable, error))
        goto out;
    }
  else
    {
      if (!g_file_copy (src, dest, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA,
                        cancellable, NULL, NULL, error))
        goto out;
      if (g_file_info_get_file_type (src_info) == G_FILE_TYPE_REGULAR)
        {
          if (!gs_file_sync_data (dest, cancellable, error))
            goto out;
        }
      if (!ot_util_fsync_directory (dest_parent, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * merge_etc_changes:
 *
 * Compute the difference between @orig_etc and @modified_etc,
 * and apply that to @new_etc.
 *
 * The algorithm for computing the difference is pretty simple; it's
 * approximately equivalent to "diff -unR orig_etc modified_etc",
 * except that rather than attempting a 3-way merge if a file is also
 * changed in @new_etc, the modified version always wins.
 */
static gboolean
merge_etc_changes (GFile          *orig_etc,
                   GFile          *modified_etc,
                   GFile          *new_etc,
                   GCancellable   *cancellable,
                   GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_ptrarray GPtrArray *modified = NULL;
  gs_unref_ptrarray GPtrArray *removed = NULL;
  gs_unref_ptrarray GPtrArray *added = NULL;
  guint i;

  modified = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_diff_item_unref);
  removed = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  added = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

  /* For now, ignore changes to xattrs; the problem is that
   * security.selinux will be different between the /usr/etc labels
   * and the ones in the real /etc, so they all show up as different.
   *
   * This means that if you want to change the security context of a
   * file, to have that change persist across upgrades, you must also
   * modify the content of the file.
   */
  if (!ostree_diff_dirs (OSTREE_DIFF_FLAGS_IGNORE_XATTRS,
                         orig_etc, modified_etc, modified, removed, added,
                         cancellable, error))
    {
      g_prefix_error (error, "While computing configuration diff: ");
      goto out;
    }

  if (modified->len > 0 || removed->len > 0 || added->len > 0)
    g_print ("ostadmin: Processing /etc: %u modified, %u removed, %u added\n", 
             modified->len,
             removed->len,
             added->len);
  else
    g_print ("ostadmin: No modified configuration\n");

  for (i = 0; i < removed->len; i++)
    {
      GFile *file = removed->pdata[i];
      gs_unref_object GFile *target_file = NULL;
      gs_free char *path = NULL;

      path = g_file_get_relative_path (orig_etc, file);
      g_assert (path);
      target_file = g_file_resolve_relative_path (new_etc, path);

      if (!gs_shutil_rm_rf (target_file, cancellable, error))
        goto out;
    }

  for (i = 0; i < modified->len; i++)
    {
      OstreeDiffItem *diff = modified->pdata[i];

      if (!copy_modified_config_file (orig_etc, modified_etc, new_etc, diff->target,
                                      cancellable, error))
        goto out;
    }
  for (i = 0; i < added->len; i++)
    {
      GFile *file = added->pdata[i];

      if (!copy_modified_config_file (orig_etc, modified_etc, new_etc, file,
                                      cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * checkout_deployment_tree:
 *
 * Look up @revision in the repository, and check it out in
 * /ostree/deploy/OS/deploy/${treecsum}.${deployserial}.
 */
static gboolean
checkout_deployment_tree (OstreeSysroot     *sysroot,
                          OstreeRepo        *repo,
                          OstreeDeployment      *deployment,
                          GFile            **out_deployment_path,
                          GCancellable      *cancellable,
                          GError           **error)
{
  gboolean ret = FALSE;
  const char *csum = ostree_deployment_get_csum (deployment);
  gs_unref_object GFile *root = NULL;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_free char *checkout_target_name = NULL;
  gs_unref_object GFile *osdeploy_path = NULL;
  gs_unref_object GFile *deploy_target_path = NULL;
  gs_unref_object GFile *deploy_parent = NULL;

  if (!ostree_repo_read_commit (repo, csum, &root, NULL, cancellable, error))
    goto out;

  file_info = g_file_query_info (root, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!file_info)
    goto out;

  osdeploy_path = ot_gfile_get_child_build_path (sysroot->path, "ostree", "deploy",
                                                 ostree_deployment_get_osname (deployment),
                                                 "deploy", NULL);
  checkout_target_name = g_strdup_printf ("%s.%d", csum, ostree_deployment_get_deployserial (deployment));
  deploy_target_path = g_file_get_child (osdeploy_path, checkout_target_name);

  deploy_parent = g_file_get_parent (deploy_target_path);
  if (!ot_util_ensure_directory_and_fsync (deploy_parent, cancellable, error))
    goto out;
  
  g_print ("ostadmin: Creating deployment %s\n",
           gs_file_get_path_cached (deploy_target_path));

  if (!ostree_repo_checkout_tree (repo, 0, 0, deploy_target_path, OSTREE_REPO_FILE (root),
                                  file_info, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_deployment_path, &deploy_target_path);
 out:
  return ret;
}

static char *
ptrarray_path_join (GPtrArray  *path)
{
  GString *path_buf;

  path_buf = g_string_new ("");

  if (path->len == 0)
    g_string_append_c (path_buf, '/');
  else
    {
      guint i;
      for (i = 0; i < path->len; i++)
        {
          const char *elt = path->pdata[i];

          g_string_append_c (path_buf, '/');
          g_string_append (path_buf, elt);
        }
    }

  return g_string_free (path_buf, FALSE);
}

static gboolean
relabel_one_path (OstreeSysroot  *sysroot,
                  OstreeSePolicy *sepolicy,
                  GFile         *path,
                  GFileInfo     *info,
                  GPtrArray     *path_parts,
                  GCancellable   *cancellable,
                  GError        **error)
{
  gboolean ret = FALSE;
  gs_free char *relpath = NULL;

  relpath = ptrarray_path_join (path_parts);
  if (!ostree_sepolicy_restorecon (sepolicy, relpath,
                                   info, path,
                                   OSTREE_SEPOLICY_RESTORECON_FLAGS_ALLOW_NOLABEL,
                                   NULL,
                                   cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
relabel_recursively (OstreeSysroot  *sysroot,
                     OstreeSePolicy *sepolicy,
                     GFile          *dir,
                     GFileInfo      *dir_info,
                     GPtrArray      *path_parts,
                     GCancellable   *cancellable,
                     GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *direnum = NULL;

  if (!relabel_one_path (sysroot, sepolicy, dir, dir_info, path_parts,
                         cancellable, error))
    goto out;

  direnum = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO,
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       cancellable, error);
  if (!direnum)
    goto out;
  
  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *child;
      GFileType ftype;

      if (!gs_file_enumerator_iterate (direnum, &file_info, &child,
                                       cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      g_ptr_array_add (path_parts, (char*)gs_file_get_basename_cached (child));

      ftype = g_file_info_get_file_type (file_info);
      if (ftype == G_FILE_TYPE_DIRECTORY)
        {
          if (!relabel_recursively (sysroot, sepolicy, child, file_info, path_parts,
                                    cancellable, error))
            goto out;
        }
      else
        {
          if (!relabel_one_path (sysroot, sepolicy, child, file_info, path_parts,
                                 cancellable, error))
            goto out;
        }

      g_ptr_array_remove_index (path_parts, path_parts->len - 1);
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
selinux_relabel_dir (OstreeSysroot                 *sysroot,
                     OstreeSePolicy                *sepolicy,
                     GFile                         *dir,
                     const char                    *prefix,
                     GCancellable                  *cancellable,
                     GError                       **error)
{
  gboolean ret = FALSE;
  gs_unref_ptrarray GPtrArray *path_parts = g_ptr_array_new ();
  gs_unref_object GFileInfo *root_info = NULL;

  root_info = g_file_query_info (dir, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!root_info)
    goto out;
  
  g_ptr_array_add (path_parts, (char*)prefix);
  if (!relabel_recursively (sysroot, sepolicy, dir, root_info, path_parts,
                            cancellable, error))
    {
      g_prefix_error (error, "Relabeling /%s: ", prefix);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
selinux_relabel_file (OstreeSysroot                 *sysroot,
                      OstreeSePolicy                *sepolicy,
                      GFile                         *path,
                      const char                    *prefix,
                      GCancellable                  *cancellable,
                      GError                       **error)
{
  gboolean ret = FALSE;
  gs_unref_ptrarray GPtrArray *path_parts = g_ptr_array_new ();
  gs_unref_object GFileInfo *file_info = g_file_query_info (path, OSTREE_GIO_FAST_QUERYINFO,
                                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                            cancellable, error);
  if (!file_info)
    goto out;

  g_ptr_array_add (path_parts, (char*)prefix);
  g_ptr_array_add (path_parts, (char*)gs_file_get_basename_cached (path));
  if (!relabel_one_path (sysroot, sepolicy, path, file_info, path_parts,
                         cancellable, error))
    {
      g_prefix_error (error, "Relabeling /%s/%s: ", prefix,
                      gs_file_get_basename_cached (path));
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
selinux_relabel_var_if_needed (OstreeSysroot                 *sysroot,
                               OstreeSePolicy                *sepolicy,
                               GFile                         *deployment_var_path,
                               GCancellable                  *cancellable,
                               GError                       **error)
{
  gboolean ret = FALSE;
  /* This is a bit of a hack; we should change the code at some
   * point in the distant future to only create (and label) /var
   * when doing a deployment.
   */
  gs_unref_object GFile *deployment_var_labeled = 
    g_file_get_child (deployment_var_path, ".ostree-selabeled");
  gs_unref_object GFile *deployment_var_labeled_tmp = 
    g_file_get_child (deployment_var_path, ".ostree-selabeled.tmp");
      
  if (!g_file_query_exists (deployment_var_labeled, NULL))
    {
      g_print ("ostadmin: Didn't find '%s', relabeling /var\n",
               gs_file_get_path_cached (deployment_var_labeled));

      if (!selinux_relabel_dir (sysroot, sepolicy,
                                deployment_var_path, "var",
                                cancellable, error))
        {
          g_prefix_error (error, "Relabeling /var: ");
          goto out;
        }

      if (!g_file_replace_contents (deployment_var_labeled_tmp, "", 0, NULL, FALSE,
                                    G_FILE_CREATE_REPLACE_DESTINATION, NULL,
                                    cancellable, error))
        goto out;
          
      if (!selinux_relabel_file (sysroot, sepolicy,
                                 deployment_var_labeled_tmp, "var",
                                 cancellable, error))
        goto out;

      if (!gs_file_rename (deployment_var_labeled_tmp, deployment_var_labeled,
                           cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
merge_configuration (OstreeSysroot         *sysroot,
                     OstreeDeployment      *previous_deployment,
                     OstreeDeployment      *deployment,
                     GFile                 *deployment_path,
                     OstreeSePolicy       **out_sepolicy,
                     GCancellable          *cancellable,
                     GError               **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *source_etc_path = NULL;
  gs_unref_object GFile *source_etc_pristine_path = NULL;
  gs_unref_object GFile *deployment_usretc_path = NULL;
  gs_unref_object GFile *deployment_etc_path = NULL;
  gs_unref_object OstreeSePolicy *sepolicy = NULL;
  gboolean etc_exists;
  gboolean usretc_exists;

  if (previous_deployment)
    {
      gs_unref_object GFile *previous_path = NULL;
      OstreeBootconfigParser *previous_bootconfig;

      previous_path = ostree_sysroot_get_deployment_directory (sysroot, previous_deployment);
      source_etc_path = g_file_resolve_relative_path (previous_path, "etc");
      source_etc_pristine_path = g_file_resolve_relative_path (previous_path, "usr/etc");

      previous_bootconfig = ostree_deployment_get_bootconfig (previous_deployment);
      if (previous_bootconfig)
        {
          const char *previous_options = ostree_bootconfig_parser_get (previous_bootconfig, "options");
          /* Completely overwrite the previous options here; we will extend
           * them later.
           */
          ostree_bootconfig_parser_set (ostree_deployment_get_bootconfig (deployment), "options",
                                        previous_options);
        }
    }

  deployment_etc_path = g_file_get_child (deployment_path, "etc");
  deployment_usretc_path = g_file_resolve_relative_path (deployment_path, "usr/etc");
  
  etc_exists = g_file_query_exists (deployment_etc_path, NULL);
  usretc_exists = g_file_query_exists (deployment_usretc_path, NULL);

  if (etc_exists && usretc_exists)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "Tree contains both /etc and /usr/etc");
      goto out;
    }
  else if (etc_exists)
    {
      /* Compatibility hack */
      if (!gs_file_rename (deployment_etc_path, deployment_usretc_path,
                           cancellable, error))
        goto out;
      usretc_exists = TRUE;
      etc_exists = FALSE;
    }
  
  if (usretc_exists)
    {
      /* TODO - set out labels as we copy files */
      g_assert (!etc_exists);
      if (!gs_shutil_cp_a (deployment_usretc_path, deployment_etc_path,
                           cancellable, error))
        goto out;

      /* Here, we initialize SELinux policy from the /usr/etc inside
       * the root - this is before we've finalized the configuration
       * merge into /etc. */
      sepolicy = ostree_sepolicy_new (deployment_path, cancellable, error);
      if (!sepolicy)
        goto out;

      if (ostree_sepolicy_get_name (sepolicy) != NULL)
        {
          if (!selinux_relabel_dir (sysroot, sepolicy, deployment_etc_path, "etc",
                                    cancellable, error))
            goto out;
        }
    }

  if (source_etc_path)
    {
      if (!merge_etc_changes (source_etc_pristine_path, source_etc_path, deployment_etc_path, 
                              cancellable, error))
        goto out;
    }
  else
    {
      g_print ("ostadmin: No previous configuration changes to merge\n");
    }

  ret = TRUE;
  gs_transfer_out_value (out_sepolicy, &sepolicy);
 out:
  return ret;
}

static gboolean
write_origin_file (OstreeSysroot         *sysroot,
                   OstreeDeployment      *deployment,
                   GCancellable      *cancellable,
                   GError           **error)
{
  gboolean ret = FALSE;
  GKeyFile *origin = ostree_deployment_get_origin (deployment);

  if (origin)
    {
      gs_unref_object GFile *deployment_path = ostree_sysroot_get_deployment_directory (sysroot, deployment);
      gs_unref_object GFile *origin_path = ostree_sysroot_get_deployment_origin_path (deployment_path);
      gs_free char *contents = NULL;
      gsize len;

      contents = g_key_file_to_data (origin, &len, error);
      if (!contents)
        goto out;

      if (!g_file_replace_contents (origin_path, contents, len, NULL, FALSE,
                                    G_FILE_CREATE_REPLACE_DESTINATION, NULL,
                                    cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
get_kernel_from_tree (GFile         *deployroot,
                      GFile        **out_kernel,
                      GFile        **out_initramfs,
                      GCancellable  *cancellable,
                      GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *bootdir = g_file_get_child (deployroot, "boot");
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFile *ret_kernel = NULL;
  gs_unref_object GFile *ret_initramfs = NULL;
  gs_free char *kernel_checksum = NULL;
  gs_free char *initramfs_checksum = NULL;

  dir_enum = g_file_enumerate_children (bootdir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, error);
  if (!dir_enum)
    goto out;

  while (TRUE)
    {
      GFileInfo *file_info = NULL;
      const char *name;

      if (!gs_file_enumerator_iterate (dir_enum, &file_info, NULL,
                                       cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      name = g_file_info_get_name (file_info);
      
      if (ret_kernel == NULL && g_str_has_prefix (name, "vmlinuz-"))
        {
          const char *dash = strrchr (name, '-');
          g_assert (dash);
          if (ostree_validate_structureof_checksum_string (dash + 1, NULL))
            {
              kernel_checksum = g_strdup (dash + 1);
              ret_kernel = g_file_get_child (bootdir, name);
            }
        }
      else if (ret_initramfs == NULL && g_str_has_prefix (name, "initramfs-"))
        {
          const char *dash = strrchr (name, '-');
          g_assert (dash);
          if (ostree_validate_structureof_checksum_string (dash + 1, NULL))
            {
              initramfs_checksum = g_strdup (dash + 1);
              ret_initramfs = g_file_get_child (bootdir, name);
            }
        }
      
      if (ret_kernel && ret_initramfs)
        break;
    }

  if (ret_kernel == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Failed to find boot/vmlinuz-<CHECKSUM> in %s",
                   gs_file_get_path_cached (deployroot));
      goto out;
    }

  if (ret_initramfs != NULL)
    {
      if (strcmp (kernel_checksum, initramfs_checksum) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "Mismatched kernel %s checksum vs initrd %s",
                       gs_file_get_basename_cached (ret_initramfs),
                       gs_file_get_basename_cached (ret_initramfs));
          goto out;
        }
    }

  ot_transfer_out_value (out_kernel, &ret_kernel);
  ot_transfer_out_value (out_initramfs, &ret_initramfs);
  ret = TRUE;
 out:
  return ret;
}

static gboolean
checksum_from_kernel_src (GFile        *src,
                          char        **out_checksum,
                          GError     **error)
{
  const char *last_dash = strrchr (gs_file_get_path_cached (src), '-');
  if (!last_dash)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Malformed initramfs name '%s', missing '-'", gs_file_get_basename_cached (src));
      return FALSE;
    }
  *out_checksum = g_strdup (last_dash + 1);
  return TRUE;
}

/* FIXME: We should really do individual fdatasync() on files/dirs,
 * since this causes us to block on unrelated I/O.  However, it's just
 * safer for now.
 */
static gboolean
full_system_sync (GCancellable      *cancellable,
                  GError           **error)
{
  sync ();
  return TRUE;
}

static gboolean
swap_bootlinks (OstreeSysroot *self,
                int            bootversion,
                GPtrArray    *new_deployments,
                GCancellable *cancellable,
                GError      **error)
{
  gboolean ret = FALSE;
  guint i;
  int old_subbootversion;
  int new_subbootversion;
  gs_unref_object GFile *ostree_dir = g_file_get_child (self->path, "ostree");
  gs_free char *ostree_bootdir_name = g_strdup_printf ("boot.%d", bootversion);
  gs_unref_object GFile *ostree_bootdir = g_file_resolve_relative_path (ostree_dir, ostree_bootdir_name);
  gs_free char *ostree_subbootdir_name = NULL;
  gs_unref_object GFile *ostree_subbootdir = NULL;

  if (bootversion != self->bootversion)
    {
      if (!_ostree_sysroot_read_current_subbootversion (self, bootversion, &old_subbootversion,
                                                        cancellable, error))
        goto out;
    }
  else
    old_subbootversion = self->subbootversion;

  new_subbootversion = old_subbootversion == 0 ? 1 : 0;

  ostree_subbootdir_name = g_strdup_printf ("boot.%d.%d", bootversion, new_subbootversion);
  ostree_subbootdir = g_file_resolve_relative_path (ostree_dir, ostree_subbootdir_name);

  if (!ot_util_ensure_directory_and_fsync (ostree_subbootdir, cancellable, error))
    goto out;

  for (i = 0; i < new_deployments->len; i++)
    {
      OstreeDeployment *deployment = new_deployments->pdata[i];
      gs_free char *bootlink_pathname = g_strdup_printf ("%s/%s/%d",
                                                         ostree_deployment_get_osname (deployment),
                                                         ostree_deployment_get_bootcsum (deployment),
                                                         ostree_deployment_get_bootserial (deployment));
      gs_free char *bootlink_target = g_strdup_printf ("../../../deploy/%s/deploy/%s.%d",
                                                       ostree_deployment_get_osname (deployment),
                                                       ostree_deployment_get_csum (deployment),
                                                       ostree_deployment_get_deployserial (deployment));
      gs_unref_object GFile *linkname = g_file_get_child (ostree_subbootdir, bootlink_pathname);
      gs_unref_object GFile *linkname_parent = g_file_get_parent (linkname);

      if (!ot_util_ensure_directory_and_fsync (linkname_parent, cancellable, error))
        goto out;
      
      if (!g_file_make_symbolic_link (linkname, bootlink_target, cancellable, error))
        goto out;
    }

  if (!ot_gfile_atomic_symlink_swap (ostree_bootdir, ostree_subbootdir_name,
                                     cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static char *
remove_checksum_from_kernel_name (const char *name,
                                  const char *csum)
{
  const char *p = strrchr (name, '-');
  g_assert_cmpstr (p+1, ==, csum);
  return g_strndup (name, p-name);
}

static GHashTable *
parse_os_release (const char *contents,
                  const char *split)
{
  char **lines = g_strsplit (contents, split, -1);
  char **iter;
  GHashTable *ret = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  for (iter = lines; *iter; iter++)
    {
      char *line = *iter;
      char *eq;
      const char *quotedval;
      char *val;

      if (g_str_has_prefix (line, "#"))
        continue;
      
      eq = strchr (line, '=');
      if (!eq)
        continue;
      
      *eq = '\0';
      quotedval = eq + 1;
      val = g_shell_unquote (quotedval, NULL);
      if (!val)
        continue;
      
      g_hash_table_insert (ret, line, val);
    }

  return ret;
}

/*
 * install_deployment_kernel:
 * 
 * Write out an entry in /boot/loader/entries for @deployment.
 */
static gboolean
install_deployment_kernel (OstreeSysroot   *sysroot,
                           int             new_bootversion,
                           OstreeDeployment   *deployment,
                           guint           n_deployments,
                           GCancellable   *cancellable,
                           GError        **error)

{
  gboolean ret = FALSE;
  const char *osname = ostree_deployment_get_osname (deployment);
  const char *bootcsum = ostree_deployment_get_bootcsum (deployment);
  gs_unref_object GFile *bootdir = NULL;
  gs_unref_object GFile *bootcsumdir = NULL;
  gs_unref_object GFile *bootconfpath = NULL;
  gs_unref_object GFile *bootconfpath_parent = NULL;
  gs_free char *dest_kernel_name = NULL;
  gs_unref_object GFile *dest_kernel_path = NULL;
  gs_unref_object GFile *dest_initramfs_path = NULL;
  gs_unref_object GFile *tree_kernel_path = NULL;
  gs_unref_object GFile *tree_initramfs_path = NULL;
  gs_unref_object GFile *etc_os_release = NULL;
  gs_unref_object GFile *deployment_dir = NULL;
  gs_free char *contents = NULL;
  gs_unref_hashtable GHashTable *osrelease_values = NULL;
  gs_free char *linux_relpath = NULL;
  gs_free char *linux_key = NULL;
  gs_free char *initramfs_relpath = NULL;
  gs_free char *title_key = NULL;
  gs_free char *initrd_key = NULL;
  gs_free char *version_key = NULL;
  gs_free char *ostree_kernel_arg = NULL;
  gs_free char *options_key = NULL;
  __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *kargs = NULL;
  const char *val;
  OstreeBootconfigParser *bootconfig;
  gsize len;

  bootconfig = ostree_deployment_get_bootconfig (deployment);
  deployment_dir = ostree_sysroot_get_deployment_directory (sysroot, deployment);

  if (!get_kernel_from_tree (deployment_dir, &tree_kernel_path, &tree_initramfs_path,
                             cancellable, error))
    goto out;

  bootdir = g_file_get_child (ostree_sysroot_get_path (sysroot), "boot");
  bootcsumdir = ot_gfile_resolve_path_printf (bootdir, "ostree/%s-%s",
                                              osname,
                                              bootcsum);
  bootconfpath = ot_gfile_resolve_path_printf (bootdir, "loader.%d/entries/ostree-%s-%d.conf",
                                               new_bootversion, osname, 
                                               ostree_deployment_get_index (deployment));

  if (!ot_util_ensure_directory_and_fsync (bootcsumdir, cancellable, error))
    goto out;
  bootconfpath_parent = g_file_get_parent (bootconfpath);
  if (!ot_util_ensure_directory_and_fsync (bootconfpath_parent, cancellable, error))
    goto out;

  dest_kernel_name = remove_checksum_from_kernel_name (gs_file_get_basename_cached (tree_kernel_path),
                                                       bootcsum);
  dest_kernel_path = g_file_get_child (bootcsumdir, dest_kernel_name);
  if (!g_file_query_exists (dest_kernel_path, NULL))
    {
      if (!gs_file_linkcopy_sync_data (tree_kernel_path, dest_kernel_path, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA,
                                       cancellable, error))
        goto out;
    }

  if (tree_initramfs_path)
    {
      gs_free char *dest_initramfs_name = remove_checksum_from_kernel_name (gs_file_get_basename_cached (tree_initramfs_path),
                                                                       bootcsum);
      dest_initramfs_path = g_file_get_child (bootcsumdir, dest_initramfs_name);

      if (!g_file_query_exists (dest_initramfs_path, NULL))
        {
          if (!gs_file_linkcopy_sync_data (tree_initramfs_path, dest_initramfs_path, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA,
                                           cancellable, error))
            goto out;
        }
    }

  etc_os_release = g_file_resolve_relative_path (deployment_dir, "etc/os-release");

  if (!g_file_load_contents (etc_os_release, cancellable,
                             &contents, &len, NULL, error))
    {
      g_prefix_error (error, "Reading /etc/os-release: ");
      goto out;
    }

  osrelease_values = parse_os_release (contents, "\n");

  /* title */
  val = g_hash_table_lookup (osrelease_values, "PRETTY_NAME");
  if (val == NULL)
      val = g_hash_table_lookup (osrelease_values, "ID");
  if (val == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "No PRETTY_NAME or ID in /etc/os-release");
      goto out;
    }
  
  title_key = g_strdup_printf ("ostree:%s:%d %s", ostree_deployment_get_osname (deployment),
                               ostree_deployment_get_index (deployment),
                               val);
  ostree_bootconfig_parser_set (bootconfig, "title", title_key);

  version_key = g_strdup_printf ("%d", n_deployments - ostree_deployment_get_index (deployment));
  ostree_bootconfig_parser_set (bootconfig, "version", version_key);

  linux_relpath = g_file_get_relative_path (bootdir, dest_kernel_path);
  linux_key = g_strconcat ("/", linux_relpath, NULL);
  ostree_bootconfig_parser_set (bootconfig, "linux", linux_key);

  if (dest_initramfs_path)
    {
      initramfs_relpath = g_file_get_relative_path (bootdir, dest_initramfs_path);
      initrd_key = g_strconcat ("/", initramfs_relpath, NULL);
      ostree_bootconfig_parser_set (bootconfig, "initrd", initrd_key);
    }

  val = ostree_bootconfig_parser_get (bootconfig, "options");

  ostree_kernel_arg = g_strdup_printf ("ostree=/ostree/boot.%d/%s/%s/%d",
                                       new_bootversion, osname, bootcsum,
                                       ostree_deployment_get_bootserial (deployment));
  kargs = _ostree_kernel_args_from_string (val);
  _ostree_kernel_args_replace_take (kargs, ostree_kernel_arg);
  ostree_kernel_arg = NULL;
  options_key = _ostree_kernel_args_to_string (kargs);
  ostree_bootconfig_parser_set (bootconfig, "options", options_key);
  
  if (!ostree_bootconfig_parser_write (ostree_deployment_get_bootconfig (deployment), bootconfpath,
                               cancellable, error))
      goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
swap_bootloader (OstreeSysroot  *sysroot,
                 int             current_bootversion,
                 int             new_bootversion,
                 GCancellable   *cancellable,
                 GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *boot_loader_link = NULL;
  gs_free char *new_target = NULL;

  g_assert ((current_bootversion == 0 && new_bootversion == 1) ||
            (current_bootversion == 1 && new_bootversion == 0));

  boot_loader_link = g_file_resolve_relative_path (sysroot->path, "boot/loader");
  new_target = g_strdup_printf ("loader.%d", new_bootversion);

  if (!ot_gfile_atomic_symlink_swap (boot_loader_link, new_target,
                                     cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static GHashTable *
assign_bootserials (GPtrArray   *deployments)
{
  guint i;
  GHashTable *ret = 
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);

  for (i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      const char *bootcsum = ostree_deployment_get_bootcsum (deployment);
      guint count;

      count = GPOINTER_TO_UINT (g_hash_table_lookup (ret, bootcsum));
      g_hash_table_replace (ret, (char*) bootcsum,
                            GUINT_TO_POINTER (count + 1));

      ostree_deployment_set_bootserial (deployment, count);
    }
  return ret;
}

static gboolean
deployment_bootconfigs_equal (OstreeDeployment *a,
                              OstreeDeployment *b)
{
  const char *a_bootcsum = ostree_deployment_get_bootcsum (a);
  const char *b_bootcsum = ostree_deployment_get_bootcsum (b);

  if (strcmp (a_bootcsum, b_bootcsum) != 0)
    return FALSE;

  {
    OstreeBootconfigParser *a_bootconfig = ostree_deployment_get_bootconfig (a);
    OstreeBootconfigParser *b_bootconfig = ostree_deployment_get_bootconfig (b);
    const char *a_boot_options = ostree_bootconfig_parser_get (a_bootconfig, "options");
    const char *b_boot_options = ostree_bootconfig_parser_get (b_bootconfig, "options");
    __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *a_kargs = NULL;
    __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *b_kargs = NULL;
    gs_free char *a_boot_options_without_ostree = NULL;
    gs_free char *b_boot_options_without_ostree = NULL;
      
    /* We checksum the kernel arguments *except* ostree= */
    a_kargs = _ostree_kernel_args_from_string (a_boot_options);
    _ostree_kernel_args_replace (a_kargs, "ostree");
    a_boot_options_without_ostree = _ostree_kernel_args_to_string (a_kargs);

    b_kargs = _ostree_kernel_args_from_string (b_boot_options);
    _ostree_kernel_args_replace (b_kargs, "ostree");
    b_boot_options_without_ostree = _ostree_kernel_args_to_string (b_kargs);

    if (strcmp (a_boot_options_without_ostree, b_boot_options_without_ostree) != 0)
      return FALSE;
  }

  return TRUE;
}

/* TEMPORARY HACK: Add a "current" symbolic link that's easy to
 * follow inside the gnome-ostree build scripts.  This isn't atomic,
 * but that doesn't matter because it's only used by deployments
 * done from the host.
 */
static gboolean
create_current_symlinks (OstreeSysroot         *self,
                         GCancellable          *cancellable,
                         GError               **error)
{
  gboolean ret = FALSE;
  guint i;
  gs_unref_hashtable GHashTable *created_current_for_osname =
    g_hash_table_new (g_str_hash, g_str_equal);

  for (i = 0; i < self->deployments->len; i++)
    {
      OstreeDeployment *deployment = self->deployments->pdata[i];
      const char *osname = ostree_deployment_get_osname (deployment);

      if (!g_hash_table_lookup (created_current_for_osname, osname))
        {
          gs_unref_object GFile *osdir = ot_gfile_resolve_path_printf (self->path, "ostree/deploy/%s", osname);
          gs_unref_object GFile *os_current_path = g_file_get_child (osdir, "current");
          gs_unref_object GFile *deployment_path = ostree_sysroot_get_deployment_directory (self, deployment);
          gs_free char *target = g_file_get_relative_path (osdir, deployment_path);
          
          g_assert (target != NULL);
          
          if (!ot_gfile_atomic_symlink_swap (os_current_path, target,
                                             cancellable, error))
            goto out;

          g_hash_table_insert (created_current_for_osname, (char*)osname, GUINT_TO_POINTER (1));
        }
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_sysroot_write_deployments:
 * @self: Sysroot
 * @new_deployments: (element-type OstreeDeployment): List of new deployments
 * @cancellable: Cancellable
 * @error: Error
 *
 * Assuming @new_deployments have already been deployed in place on
 * disk, atomically update bootloader configuration.
 */
gboolean
ostree_sysroot_write_deployments (OstreeSysroot     *self,
                                  GPtrArray         *new_deployments,
                                  GCancellable      *cancellable,
                                  GError           **error)
{
  gboolean ret = FALSE;
  guint i;
  gboolean requires_new_bootversion = FALSE;
  gboolean found_booted_deployment = FALSE;

  g_assert (self->loaded);

  /* Assign a bootserial to each new deployment.
   */
  assign_bootserials (new_deployments);

  /* Determine whether or not we need to touch the bootloader
   * configuration.  If we have an equal number of deployments with
   * matching bootloader configuration, then we can just swap the
   * subbootversion bootlinks.
   */
  if (new_deployments->len != self->deployments->len)
    requires_new_bootversion = TRUE;
  else
    {
      for (i = 0; i < new_deployments->len; i++)
        {
          if (!deployment_bootconfigs_equal (new_deployments->pdata[i],
                                             self->deployments->pdata[i]))
            {
              requires_new_bootversion = TRUE;
              break;
            }
        }
    }

  for (i = 0; i < new_deployments->len; i++)
    {
      OstreeDeployment *deployment = new_deployments->pdata[i];
      gs_unref_object GFile *deployment_root = NULL;
      
      if (deployment == self->booted_deployment)
        found_booted_deployment = TRUE;
      
      deployment_root = ostree_sysroot_get_deployment_directory (self, deployment);
      if (!g_file_query_exists (deployment_root, NULL))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unable to find expected deployment root: %s",
                       gs_file_get_path_cached (deployment_root));
          goto out;
        }

      ostree_deployment_set_index (deployment, i);
    }

  if (self->booted_deployment && !found_booted_deployment)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Attempting to remove booted deployment");
      goto out;
    }

  if (!requires_new_bootversion)
    {
      if (!full_system_sync (cancellable, error))
        {
          g_prefix_error (error, "Full sync: ");
          goto out;
        }

      if (!swap_bootlinks (self, self->bootversion,
                           new_deployments,
                           cancellable, error))
        {
          g_prefix_error (error, "Swapping current bootlinks: ");
          goto out;
        }
    }
  else
    {
      int new_bootversion = self->bootversion ? 0 : 1;
      gs_unref_object OstreeBootloader *bootloader = _ostree_sysroot_query_bootloader (self);
      gs_unref_object GFile *new_loader_entries_dir = NULL;

      new_loader_entries_dir = ot_gfile_resolve_path_printf (self->path, "boot/loader.%d/entries",
                                                             new_bootversion);
      if (!ot_util_ensure_directory_and_fsync (new_loader_entries_dir, cancellable, error))
        goto out;
      
      for (i = 0; i < new_deployments->len; i++)
        {
          OstreeDeployment *deployment = new_deployments->pdata[i];
          if (!install_deployment_kernel (self, new_bootversion,
                                          deployment, new_deployments->len,
                                          cancellable, error))
            {
              g_prefix_error (error, "Installing kernel: ");
              goto out;
            }
        }

      /* Swap bootlinks for *new* version */
      if (!swap_bootlinks (self, new_bootversion, new_deployments,
                           cancellable, error))
        {
          g_prefix_error (error, "Generating new bootlinks: ");
          goto out;
        }

      g_debug ("Using bootloader: %s", bootloader ?
               g_type_name (G_TYPE_FROM_INSTANCE (bootloader)) : "(none)");

      if (bootloader && !_ostree_bootloader_write_config (bootloader, new_bootversion,
                                                          cancellable, error))
        {
          g_prefix_error (error, "Bootloader write config: ");
          goto out;
        }

      if (!full_system_sync (cancellable, error))
        {
          g_prefix_error (error, "Full sync: ");
          goto out;
        }

      if (!swap_bootloader (self, self->bootversion, new_bootversion,
                            cancellable, error))
        {
          g_prefix_error (error, "Final bootloader swap: ");
          goto out;
        }
    }

  gs_log_structured_print_id_v (OSTREE_DEPLOYMENT_COMPLETE_ID,
                                "Transaction complete; bootconfig swap: %s deployment count change: %i)",
                                requires_new_bootversion ? "yes" : "no",
                                new_deployments->len - self->deployments->len);

  /* Now reload from disk */
  if (!ostree_sysroot_load (self, cancellable, error))
    {
      g_prefix_error (error, "Reloading deployments after commit: ");
      goto out;
    }

  if (!create_current_symlinks (self, cancellable, error))
    goto out;

  /* And finally, cleanup of any leftover data.
   */
  if (!ostree_sysroot_cleanup (self, cancellable, error))
    {
      g_prefix_error (error, "Performing final cleanup: ");
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
allocate_deployserial (OstreeSysroot           *self,
                       const char              *osname,
                       const char              *revision,
                       int                     *out_deployserial,
                       GCancellable            *cancellable,
                       GError                 **error)
{
  gboolean ret = FALSE;
  guint i;
  int new_deployserial = 0;
  gs_unref_object GFile *osdir = NULL;
  gs_unref_ptrarray GPtrArray *tmp_current_deployments =
    g_ptr_array_new_with_free_func (g_object_unref);

  osdir = ot_gfile_get_child_build_path (self->path, "ostree/deploy", osname, NULL);
  
  if (!_ostree_sysroot_list_deployment_dirs_for_os (osdir, tmp_current_deployments,
                                                    cancellable, error))
    goto out;

  for (i = 0; i < tmp_current_deployments->len; i++)
    {
      OstreeDeployment *deployment = tmp_current_deployments->pdata[i];
      
      if (strcmp (ostree_deployment_get_osname (deployment), osname) != 0)
        continue;
      if (strcmp (ostree_deployment_get_csum (deployment), revision) != 0)
        continue;

      new_deployserial = MAX(new_deployserial, ostree_deployment_get_deployserial (deployment)+1);
    }

  ret = TRUE;
  *out_deployserial = new_deployserial;
 out:
  return ret;
}
                            
/**
 * ostree_sysroot_deploy_tree:
 * @self: Sysroot
 * @osname: (allow-none): osname to use for merge deployment
 * @revision: Checksum to add
 * @origin: (allow-none): Origin to use for upgrades
 * @provided_merge_deployment: (allow-none): Use this deployment for merge path
 * @override_kernel_argv: (allow-none) (array zero-terminated=1) (element-type utf8): Use these as kernel arguments; if %NULL, inherit options from provided_merge_deployment
 * @out_new_deployment: (out): The new deployment path
 * @cancellable: Cancellable
 * @error: Error
 *
 * Check out deployment tree with revision @revision, performing a 3
 * way merge with @provided_merge_deployment for configuration.
 */
gboolean
ostree_sysroot_deploy_tree (OstreeSysroot     *self,
                            const char        *osname,
                            const char        *revision,
                            GKeyFile          *origin,
                            OstreeDeployment  *provided_merge_deployment,
                            char             **override_kernel_argv,
                            OstreeDeployment **out_new_deployment,
                            GCancellable      *cancellable,
                            GError           **error)
{
  gboolean ret = FALSE;
  gint new_deployserial;
  gs_unref_object OstreeDeployment *new_deployment = NULL;
  gs_unref_object OstreeDeployment *merge_deployment = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_object GFile *osdeploydir = NULL;
  gs_unref_object GFile *deployment_var = NULL;
  gs_unref_object GFile *deployment_etc = NULL;
  gs_unref_object GFile *commit_root = NULL;
  gs_unref_object GFile *tree_kernel_path = NULL;
  gs_unref_object GFile *tree_initramfs_path = NULL;
  gs_unref_object GFile *new_deployment_path = NULL;
  gs_unref_object OstreeSePolicy *sepolicy = NULL;
  gs_free char *new_bootcsum = NULL;
  gs_unref_object OstreeBootconfigParser *bootconfig = NULL;

  g_return_val_if_fail (osname != NULL || self->booted_deployment != NULL, FALSE);

  if (osname == NULL)
    osname = ostree_deployment_get_osname (self->booted_deployment);

  osdeploydir = ot_gfile_get_child_build_path (self->path, "ostree", "deploy", osname, NULL);
  if (!g_file_query_exists (osdeploydir, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No OS named \"%s\" known", osname);
      goto out;
    }

  deployment_var = g_file_get_child (osdeploydir, "var");

  if (!ostree_sysroot_get_repo (self, &repo, cancellable, error))
    goto out;

  if (!ostree_repo_read_commit (repo, revision, &commit_root, NULL, cancellable, error))
    goto out;

  if (!get_kernel_from_tree (commit_root, &tree_kernel_path, &tree_initramfs_path,
                             cancellable, error))
    goto out;
  
  if (tree_initramfs_path != NULL)
    {
      if (!checksum_from_kernel_src (tree_initramfs_path, &new_bootcsum, error))
        goto out;
    }
  else
    {
      if (!checksum_from_kernel_src (tree_kernel_path, &new_bootcsum, error))
        goto out;
    }

  if (provided_merge_deployment != NULL)
    merge_deployment = g_object_ref (provided_merge_deployment);

  if (!allocate_deployserial (self, osname, revision, &new_deployserial,
                              cancellable, error))
    goto out;

  new_deployment = ostree_deployment_new (0, osname, revision, new_deployserial,
                                          new_bootcsum, -1);
  ostree_deployment_set_origin (new_deployment, origin);

  /* Check out the userspace tree onto the filesystem */
  if (!checkout_deployment_tree (self, repo, new_deployment, &new_deployment_path,
                                 cancellable, error))
    {
      g_prefix_error (error, "Checking out tree: ");
      goto out;
    }

  if (!write_origin_file (self, new_deployment, cancellable, error))
    {
      g_prefix_error (error, "Writing out origin file: ");
      goto out;
    }

  /* Create an empty boot configuration; we will merge things into
   * it as we go.
   */
  bootconfig = ostree_bootconfig_parser_new ();
  ostree_deployment_set_bootconfig (new_deployment, bootconfig);

  if (!merge_configuration (self, merge_deployment, new_deployment,
                            new_deployment_path,
                            &sepolicy,
                            cancellable, error))
    {
      g_prefix_error (error, "During /etc merge: ");
      goto out;
    }

  deployment_etc = g_file_get_child (new_deployment_path, "etc");

  if (!selinux_relabel_var_if_needed (self, sepolicy, deployment_var,
                                      cancellable, error))
    goto out;

  /* After this, install_deployment_kernel() will set the other boot
   * options and write it out to disk.
   */
  if (override_kernel_argv)
    {
      __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *kargs = NULL;
      gs_free char *new_options = NULL;

      kargs = _ostree_kernel_args_new ();
      _ostree_kernel_args_append_argv (kargs, override_kernel_argv);
      new_options = _ostree_kernel_args_to_string (kargs);
      ostree_bootconfig_parser_set (bootconfig, "options", new_options);
    }

  ret = TRUE;
  ot_transfer_out_value (out_new_deployment, &new_deployment);
 out:
  return ret;
}

/**
 * ostree_sysroot_deployment_set_kargs:
 * @self: Sysroot
 * @deployment: A deployment
 * @new_kargs: (array zero-terminated=1) (element-type utf8): Replace deployment's kernel arguments
 * @cancellable: Cancellable
 * @error: Error
 *
 * Entirely replace the kernel arguments of @deployment with the
 * values in @new_kargs.
 */
gboolean
ostree_sysroot_deployment_set_kargs (OstreeSysroot     *self,
                                     OstreeDeployment  *deployment,
                                     char             **new_kargs,
                                     GCancellable      *cancellable,
                                     GError           **error)
{
  gboolean ret = FALSE;
  guint i;
  gs_unref_ptrarray GPtrArray *new_deployments = g_ptr_array_new_with_free_func (g_object_unref);
  gs_unref_object OstreeDeployment *new_deployment = NULL;
  __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *kargs = NULL;
  gs_free char *new_options = NULL;
  OstreeBootconfigParser *new_bootconfig;

  new_deployment = ostree_deployment_clone (deployment);
  new_bootconfig = ostree_deployment_get_bootconfig (new_deployment);

  kargs = _ostree_kernel_args_new ();
  _ostree_kernel_args_append_argv (kargs, new_kargs);
  new_options = _ostree_kernel_args_to_string (kargs);
  ostree_bootconfig_parser_set (new_bootconfig, "options", new_options);

  for (i = 0; i < self->deployments->len; i++)
    {
      OstreeDeployment *cur = self->deployments->pdata[i];
      if (cur == deployment)
        g_ptr_array_add (new_deployments, g_object_ref (new_deployment));
      else
        g_ptr_array_add (new_deployments, g_object_ref (cur));
    }

  if (!ostree_sysroot_write_deployments (self, new_deployments,
                                         cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

