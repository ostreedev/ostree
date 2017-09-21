/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

#include "otutil.h"
#include "ostree-repo-private.h"
#include "ostree-linuxfsutil.h"

#include "ostree-sysroot-private.h"

/* @deploydir_dfd: Directory FD for ostree/deploy
 * @osname: Target osname
 * @inout_deployments: All deployments in this subdir will be appended to this array
 */
gboolean
_ostree_sysroot_list_deployment_dirs_for_os (int                  deploydir_dfd,
                                             const char          *osname,
                                             GPtrArray           *inout_deployments,
                                             GCancellable        *cancellable,
                                             GError             **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  gboolean exists;
  const char *osdeploy_path = glnx_strjoina (osname, "/deploy");
  if (!ot_dfd_iter_init_allow_noent (deploydir_dfd, osdeploy_path, &dfd_iter, &exists, error))
    return FALSE;
  if (!exists)
    return TRUE;

  while (TRUE)
    {
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (dent->d_type != DT_DIR)
        continue;

      g_autofree char *csum = NULL;
      gint deployserial;
      if (!_ostree_sysroot_parse_deploy_path_name (dent->d_name, &csum, &deployserial, error))
        return FALSE;

      g_ptr_array_add (inout_deployments, ostree_deployment_new (-1, osname, csum, deployserial, NULL, -1));
    }

  return TRUE;
}

/* Return in @out_deployments a new array of OstreeDeployment loaded from the
 * filesystem state.
 */
static gboolean
list_all_deployment_directories (OstreeSysroot       *self,
                                 GPtrArray          **out_deployments,
                                 GCancellable        *cancellable,
                                 GError             **error)
{
  g_autoptr(GPtrArray) ret_deployments =
    g_ptr_array_new_with_free_func (g_object_unref);

  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  gboolean exists;
  if (!ot_dfd_iter_init_allow_noent (self->sysroot_fd, "ostree/deploy", &dfd_iter, &exists, error))
    return FALSE;
  if (!exists)
    return TRUE;

  while (TRUE)
    {
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (dent->d_type != DT_DIR)
        continue;

      if (!_ostree_sysroot_list_deployment_dirs_for_os (dfd_iter.fd, dent->d_name,
                                                        ret_deployments,
                                                        cancellable, error))
        return FALSE;
    }

  ot_transfer_out_value (out_deployments, &ret_deployments);
  return TRUE;
}

static gboolean
parse_bootdir_name (const char *name,
                    char      **out_osname,
                    char      **out_csum)
{
  const char *lastdash;

  if (out_osname)
    *out_osname = NULL;
  if (out_csum)
    *out_csum = NULL;

  lastdash = strrchr (name, '-');

  if (!lastdash)
    return FALSE;

  if (!ostree_validate_checksum_string (lastdash + 1, NULL))
    return FALSE;

  if (out_osname)
    *out_osname = g_strndup (name, lastdash - name);
  if (out_csum)
    *out_csum = g_strdup (lastdash + 1);

  return TRUE;
}

static gboolean
list_all_boot_directories (OstreeSysroot       *self,
                           GPtrArray          **out_bootdirs,
                           GCancellable        *cancellable,
                           GError             **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFile) boot_ostree = NULL;
  g_autoptr(GPtrArray) ret_bootdirs = NULL;
  GError *temp_error = NULL;

  boot_ostree = g_file_resolve_relative_path (self->path, "boot/ostree");

  ret_bootdirs = g_ptr_array_new_with_free_func (g_object_unref);

  g_autoptr(GFileEnumerator) dir_enum =
    g_file_enumerate_children (boot_ostree, OSTREE_GIO_FAST_QUERYINFO,
                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                               cancellable, &temp_error);
  if (!dir_enum)
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
          goto done;
        } 
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  while (TRUE)
    {
      GFileInfo *file_info = NULL;
      GFile *child = NULL;
      const char *name;

      if (!g_file_enumerator_iterate (dir_enum, &file_info, &child,
                                      NULL, error))
        goto out;
      if (file_info == NULL)
        break;

      if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
        continue;

      /* Only look at directories ending in -CHECKSUM; nothing else
       * should be in here, but let's be conservative.
       */
      name = g_file_info_get_name (file_info);
      if (!parse_bootdir_name (name, NULL, NULL))
        continue;
      
      g_ptr_array_add (ret_bootdirs, g_object_ref (child));
    }
  
 done:
  ret = TRUE;
  ot_transfer_out_value (out_bootdirs, &ret_bootdirs);
 out:
  return ret;
}

/* A sysroot has at most one active "boot version" (pair of version,subversion)
 * out of a total of 4 possible. This function deletes from the filesystem the 3
 * other versions that aren't active.
 */
static gboolean
cleanup_other_bootversions (OstreeSysroot       *self,
                            GCancellable        *cancellable,
                            GError             **error)
{
  const int cleanup_bootversion = self->bootversion == 0 ? 1 : 0;
  const int cleanup_subbootversion = self->subbootversion == 0 ? 1 : 0;
  /* Reusable buffer for path */
  g_autoptr(GString) buf = g_string_new ("");

  /* These directories are for the other major version */
  g_string_truncate (buf, 0); g_string_append_printf (buf, "boot/loader.%d", cleanup_bootversion);
  if (!glnx_shutil_rm_rf_at (self->sysroot_fd, buf->str, cancellable, error))
    return FALSE;
  g_string_truncate (buf, 0); g_string_append_printf (buf, "ostree/boot.%d", cleanup_bootversion);
  if (!glnx_shutil_rm_rf_at (self->sysroot_fd, buf->str, cancellable, error))
    return FALSE;
  g_string_truncate (buf, 0); g_string_append_printf (buf, "ostree/boot.%d.0", cleanup_bootversion);
  if (!glnx_shutil_rm_rf_at (self->sysroot_fd, buf->str, cancellable, error))
    return FALSE;
  g_string_truncate (buf, 0); g_string_append_printf (buf, "ostree/boot.%d.1", cleanup_bootversion);
  if (!glnx_shutil_rm_rf_at (self->sysroot_fd, buf->str, cancellable, error))
    return FALSE;

  /* And finally the other subbootversion */
  g_string_truncate (buf, 0); g_string_append_printf (buf, "ostree/boot.%d.%d", self->bootversion, cleanup_subbootversion);
  if (!glnx_shutil_rm_rf_at (self->sysroot_fd, buf->str, cancellable, error))
    return FALSE;

  return TRUE;
}

/* As the bootloader configuration changes, we will have leftover deployments
 * on disk.  This function deletes all deployments which aren't actively
 * referenced.
 */
static gboolean
cleanup_old_deployments (OstreeSysroot       *self,
                         GCancellable        *cancellable,
                         GError             **error)
{
  /* Gather the device/inode of the rootfs, so we can double
   * check we won't delete it.
   */
  struct stat root_stbuf;
  if (!glnx_fstatat (AT_FDCWD, "/", &root_stbuf, 0, error))
    return FALSE;

  /* Load all active deployments referenced by bootloader configuration. */
  g_autoptr(GHashTable) active_deployment_dirs =
    g_hash_table_new_full (g_str_hash, (GEqualFunc)g_str_equal, g_free, NULL);
  g_autoptr(GHashTable) active_boot_checksums =
    g_hash_table_new_full (g_str_hash, (GEqualFunc)g_str_equal, g_free, NULL);
  for (guint i = 0; i < self->deployments->len; i++)
    {
      OstreeDeployment *deployment = self->deployments->pdata[i];
      char *deployment_path = ostree_sysroot_get_deployment_dirpath (self, deployment);
      char *bootcsum = g_strdup (ostree_deployment_get_bootcsum (deployment));
      /* Transfer ownership */
      g_hash_table_replace (active_deployment_dirs, deployment_path, deployment_path);
      g_hash_table_replace (active_boot_checksums, bootcsum, bootcsum);
    }

  /* Find all deployment directories, both active and inactive */
  g_autoptr(GPtrArray) all_deployment_dirs = NULL;
  if (!list_all_deployment_directories (self, &all_deployment_dirs,
                                        cancellable, error))
    return FALSE;
  for (guint i = 0; i < all_deployment_dirs->len; i++)
    {
      OstreeDeployment *deployment = all_deployment_dirs->pdata[i];
      g_autofree char *deployment_path = ostree_sysroot_get_deployment_dirpath (self, deployment);
      g_autofree char *origin_relpath = ostree_deployment_get_origin_relpath (deployment);

      if (!g_hash_table_lookup (active_deployment_dirs, deployment_path))
        {
          struct stat stbuf;
          glnx_fd_close int deployment_fd = -1;

          if (!glnx_opendirat (self->sysroot_fd, deployment_path, TRUE,
                               &deployment_fd, error))
            return FALSE;

          if (!glnx_fstat (deployment_fd, &stbuf, error))
            return FALSE;

          /* This shouldn't happen, because higher levels should
           * disallow having the booted deployment not in the active
           * deployment list, but let's be extra safe. */
          if (stbuf.st_dev == root_stbuf.st_dev &&
              stbuf.st_ino == root_stbuf.st_ino)
            continue;

          /* This deployment wasn't referenced, so delete it */
          if (!_ostree_linuxfs_fd_alter_immutable_flag (deployment_fd, FALSE,
                                                        cancellable, error))
            return FALSE;
          if (!glnx_shutil_rm_rf_at (self->sysroot_fd, origin_relpath, cancellable, error))
            return FALSE;
          if (!glnx_shutil_rm_rf_at (self->sysroot_fd, deployment_path, cancellable, error))
            return FALSE;
        }
    }

  /* Clean up boot directories */
  g_autoptr(GPtrArray) all_boot_dirs = NULL;
  if (!list_all_boot_directories (self, &all_boot_dirs,
                                  cancellable, error))
    return FALSE;

  for (guint i = 0; i < all_boot_dirs->len; i++)
    {
      GFile *bootdir = all_boot_dirs->pdata[i];
      g_autofree char *osname = NULL;
      g_autofree char *bootcsum = NULL;

      if (!parse_bootdir_name (glnx_basename (gs_file_get_path_cached (bootdir)),
                               &osname, &bootcsum))
        g_assert_not_reached ();

      if (g_hash_table_lookup (active_boot_checksums, bootcsum))
        continue;

      if (!glnx_shutil_rm_rf_at (AT_FDCWD, gs_file_get_path_cached (bootdir), cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/* Delete the ref bindings for a non-active boot version */
static gboolean
cleanup_ref_prefix (OstreeRepo         *repo,
                    int                 bootversion,
                    int                 subbootversion,
                    GCancellable       *cancellable,
                    GError            **error)
{
  g_autofree char *prefix = g_strdup_printf ("ostree/%d/%d", bootversion, subbootversion);
  g_autoptr(GHashTable) refs = NULL;
  if (!ostree_repo_list_refs_ext (repo, prefix, &refs, OSTREE_REPO_LIST_REFS_EXT_NONE, cancellable, error))
    return FALSE;

  GLNX_HASH_TABLE_FOREACH (refs, const char *, ref)
    {
      if (!ostree_repo_set_ref_immediate (repo, NULL, ref, NULL, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/* libostree holds a ref for each deployment's exact checksum to avoid it being
 * GC'd even if the origin ref changes.  This function resets those refs
 * to match active deployments.
 */
static gboolean
generate_deployment_refs (OstreeSysroot       *self,
                          OstreeRepo          *repo,
                          int                  bootversion,
                          int                  subbootversion,
                          GPtrArray           *deployments,
                          GCancellable        *cancellable,
                          GError             **error)
{
  int cleanup_bootversion = (bootversion == 0) ? 1 : 0;
  int cleanup_subbootversion = (subbootversion == 0) ? 1 : 0;

  if (!cleanup_ref_prefix (repo, cleanup_bootversion, 0,
                           cancellable, error))
    return FALSE;

  if (!cleanup_ref_prefix (repo, cleanup_bootversion, 1,
                           cancellable, error))
    return FALSE;

  if (!cleanup_ref_prefix (repo, bootversion, cleanup_subbootversion,
                           cancellable, error))
    return FALSE;

  g_autoptr(_OstreeRepoAutoTransaction) txn =
    _ostree_repo_auto_transaction_start (repo, cancellable, error);
  if (!txn)
    return FALSE;
  for (guint i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      g_autofree char *refname = g_strdup_printf ("ostree/%d/%d/%u",
                                               bootversion, subbootversion,
                                               i);

      ostree_repo_transaction_set_refspec (repo, refname, ostree_deployment_get_csum (deployment));
    }
  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
prune_repo (OstreeRepo    *repo,
            GCancellable  *cancellable,
            GError       **error)
{
  gint n_objects_total;
  gint n_objects_pruned;
  guint64 freed_space;
  if (!ostree_repo_prune (repo, OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY, 0,
                          &n_objects_total, &n_objects_pruned, &freed_space,
                          cancellable, error))
    return FALSE;

  if (freed_space > 0)
    {
      g_autofree char *freed_space_str = g_format_size_full (freed_space, 0);
      g_print ("Freed objects: %s\n", freed_space_str);
    }

  return TRUE;
}

/**
 * ostree_sysroot_cleanup:
 * @self: Sysroot
 * @cancellable: Cancellable
 * @error: Error
 *
 * Delete any state that resulted from a partially completed
 * transaction, such as incomplete deployments.
 */
gboolean
ostree_sysroot_cleanup (OstreeSysroot       *self,
                        GCancellable        *cancellable,
                        GError             **error)
{
  return _ostree_sysroot_cleanup_internal (self, TRUE, cancellable, error);
}

/**
 * ostree_sysroot_prepare_cleanup:
 * @self: Sysroot
 * @cancellable: Cancellable
 * @error: Error
 *
 * Like ostree_sysroot_cleanup() in that it cleans up incomplete deployments
 * and old boot versions, but does NOT prune the repository.
 */
gboolean
ostree_sysroot_prepare_cleanup (OstreeSysroot  *self,
                                GCancellable   *cancellable,
                                GError        **error)
{
  return _ostree_sysroot_cleanup_internal (self, FALSE, cancellable, error);
}

gboolean
_ostree_sysroot_cleanup_internal (OstreeSysroot              *self,
                                  gboolean                    do_prune_repo,
                                  GCancellable               *cancellable,
                                  GError                    **error)
{
  g_return_val_if_fail (OSTREE_IS_SYSROOT (self), FALSE);
  g_return_val_if_fail (self->loaded, FALSE);

  if (!cleanup_other_bootversions (self, cancellable, error))
    return glnx_prefix_error (error, "Cleaning bootversions");

  if (!cleanup_old_deployments (self, cancellable, error))
    return glnx_prefix_error (error, "Cleaning deployments");

  OstreeRepo *repo = ostree_sysroot_repo (self);
  if (!generate_deployment_refs (self, repo,
                                 self->bootversion,
                                 self->subbootversion,
                                 self->deployments,
                                 cancellable, error))
    return glnx_prefix_error (error, "Generating deployment refs");

  if (do_prune_repo)
    {
      if (!prune_repo (repo, cancellable, error))
        return glnx_prefix_error (error, "Pruning repo");
    }

  return TRUE;
}
