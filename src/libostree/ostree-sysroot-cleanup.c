/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
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
#include "ostree-linuxfsutil.h"

#include "ostree-sysroot-private.h"

gboolean
_ostree_sysroot_list_deployment_dirs_for_os (GFile               *osdir,
                                             GPtrArray           *inout_deployments,
                                             GCancellable        *cancellable,
                                             GError             **error)
{
  gboolean ret = FALSE;
  const char *osname = glnx_basename (gs_file_get_path_cached (osdir));
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFile) osdeploy_dir = NULL;
  GError *temp_error = NULL;

  osdeploy_dir = g_file_get_child (osdir, "deploy");

  dir_enum = g_file_enumerate_children (osdeploy_dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, &temp_error);
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
      const char *name;
      GFileInfo *file_info = NULL;
      GFile *child = NULL;
      glnx_unref_object OstreeDeployment *deployment = NULL;
      g_autofree char *csum = NULL;
      gint deployserial;

      if (!g_file_enumerator_iterate (dir_enum, &file_info, &child,
                                      cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      name = g_file_info_get_name (file_info);

      if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
        continue;

      if (!_ostree_sysroot_parse_deploy_path_name (name, &csum, &deployserial, error))
        goto out;
      
      deployment = ostree_deployment_new (-1, osname, csum, deployserial, NULL, -1);
      g_ptr_array_add (inout_deployments, g_object_ref (deployment));
    }

 done:
  ret = TRUE;
 out:
  return ret;
}

static gboolean
list_all_deployment_directories (OstreeSysroot       *self,
                                 GPtrArray          **out_deployments,
                                 GCancellable        *cancellable,
                                 GError             **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFile) deploydir = NULL;
  g_autoptr(GPtrArray) ret_deployments = NULL;
  GError *temp_error = NULL;

  deploydir = g_file_resolve_relative_path (self->path, "ostree/deploy");

  ret_deployments = g_ptr_array_new_with_free_func (g_object_unref);

  dir_enum = g_file_enumerate_children (deploydir, OSTREE_GIO_FAST_QUERYINFO,
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

      if (!g_file_enumerator_iterate (dir_enum, &file_info, &child,
                                      NULL, error))
        goto out;
      if (file_info == NULL)
        break;

      if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
        continue;
      
      if (!_ostree_sysroot_list_deployment_dirs_for_os (child, ret_deployments,
                                                        cancellable, error))
        goto out;
    }
  
 done:
  ret = TRUE;
  ot_transfer_out_value (out_deployments, &ret_deployments);
 out:
  return ret;
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
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFile) boot_ostree = NULL;
  g_autoptr(GPtrArray) ret_bootdirs = NULL;
  GError *temp_error = NULL;

  boot_ostree = g_file_resolve_relative_path (self->path, "boot/ostree");

  ret_bootdirs = g_ptr_array_new_with_free_func (g_object_unref);

  dir_enum = g_file_enumerate_children (boot_ostree, OSTREE_GIO_FAST_QUERYINFO,
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

static gboolean
cleanup_other_bootversions (OstreeSysroot       *self,
                            GCancellable        *cancellable,
                            GError             **error)
{
  gboolean ret = FALSE;
  int cleanup_bootversion;
  int cleanup_subbootversion;
  g_autoptr(GFile) cleanup_boot_dir = NULL;

  cleanup_bootversion = self->bootversion == 0 ? 1 : 0;
  cleanup_subbootversion = self->subbootversion == 0 ? 1 : 0;

  cleanup_boot_dir = ot_gfile_resolve_path_printf (self->path, "boot/loader.%d", cleanup_bootversion);
  if (!glnx_shutil_rm_rf_at (AT_FDCWD, gs_file_get_path_cached (cleanup_boot_dir), cancellable, error))
    goto out;
  g_clear_object (&cleanup_boot_dir);

  cleanup_boot_dir = ot_gfile_resolve_path_printf (self->path, "ostree/boot.%d", cleanup_bootversion);
  if (!glnx_shutil_rm_rf_at (AT_FDCWD, gs_file_get_path_cached (cleanup_boot_dir), cancellable, error))
    goto out;
  g_clear_object (&cleanup_boot_dir);

  cleanup_boot_dir = ot_gfile_resolve_path_printf (self->path, "ostree/boot.%d.0", cleanup_bootversion);
  if (!glnx_shutil_rm_rf_at (AT_FDCWD, gs_file_get_path_cached (cleanup_boot_dir), cancellable, error))
    goto out;
  g_clear_object (&cleanup_boot_dir);

  cleanup_boot_dir = ot_gfile_resolve_path_printf (self->path, "ostree/boot.%d.1", cleanup_bootversion);
  if (!glnx_shutil_rm_rf_at (AT_FDCWD, gs_file_get_path_cached (cleanup_boot_dir), cancellable, error))
    goto out;
  g_clear_object (&cleanup_boot_dir);

  cleanup_boot_dir = ot_gfile_resolve_path_printf (self->path, "ostree/boot.%d.%d", self->bootversion,
                                                   cleanup_subbootversion);
  if (!glnx_shutil_rm_rf_at (AT_FDCWD, gs_file_get_path_cached (cleanup_boot_dir), cancellable, error))
    goto out;
  g_clear_object (&cleanup_boot_dir);

  ret = TRUE;
 out:
  return ret;
}

static gboolean
cleanup_old_deployments (OstreeSysroot       *self,
                         GCancellable        *cancellable,
                         GError             **error)
{
  gboolean ret = FALSE;
  struct stat root_stbuf;
  guint i;
  g_autoptr(GHashTable) active_deployment_dirs = NULL;
  g_autoptr(GHashTable) active_boot_checksums = NULL;
  g_autoptr(GPtrArray) all_deployment_dirs = NULL;
  g_autoptr(GPtrArray) all_boot_dirs = NULL;

  if (stat ("/", &root_stbuf) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  active_deployment_dirs = g_hash_table_new_full (g_str_hash, (GEqualFunc)g_str_equal, g_free, NULL);
  active_boot_checksums = g_hash_table_new_full (g_str_hash, (GEqualFunc)g_str_equal, g_free, NULL);

  for (i = 0; i < self->deployments->len; i++)
    {
      OstreeDeployment *deployment = self->deployments->pdata[i];
      char *deployment_path = ostree_sysroot_get_deployment_dirpath (self, deployment);
      char *bootcsum = g_strdup (ostree_deployment_get_bootcsum (deployment));
      /* Transfer ownership */
      g_hash_table_replace (active_deployment_dirs, deployment_path, deployment_path);
      g_hash_table_replace (active_boot_checksums, bootcsum, bootcsum);
    }

  if (!list_all_deployment_directories (self, &all_deployment_dirs,
                                        cancellable, error))
    goto out;
  
  for (i = 0; i < all_deployment_dirs->len; i++)
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
            goto out;

          if (fstat (deployment_fd, &stbuf) != 0)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }

          /* This shouldn't happen, because higher levels should
           * disallow having the booted deployment not in the active
           * deployment list, but let's be extra safe. */
          if (stbuf.st_dev == root_stbuf.st_dev &&
              stbuf.st_ino == root_stbuf.st_ino)
            continue;

          if (!_ostree_linuxfs_fd_alter_immutable_flag (deployment_fd, FALSE,
                                                        cancellable, error))
            goto out;
          
          if (!glnx_shutil_rm_rf_at (self->sysroot_fd, deployment_path, cancellable, error))
            goto out;
          if (!glnx_shutil_rm_rf_at (self->sysroot_fd, origin_relpath, cancellable, error))
            goto out;
        }
    }

  if (!list_all_boot_directories (self, &all_boot_dirs,
                                  cancellable, error))
    goto out;
  
  for (i = 0; i < all_boot_dirs->len; i++)
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
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
cleanup_ref_prefix (OstreeRepo         *repo,
                    int                 bootversion,
                    int                 subbootversion,
                    GCancellable       *cancellable,
                    GError            **error)
{
  gboolean ret = FALSE;
  g_autofree char *prefix = NULL;
  g_autoptr(GHashTable) refs = NULL;
  GHashTableIter hashiter;
  gpointer hashkey, hashvalue;

  prefix = g_strdup_printf ("ostree/%d/%d", bootversion, subbootversion);

  if (!ostree_repo_list_refs_ext (repo, prefix, &refs, OSTREE_REPO_LIST_REFS_EXT_NONE, cancellable, error))
    goto out;

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  g_hash_table_iter_init (&hashiter, refs);
  while (g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue))
    {
      const char *ref = hashkey;
      ostree_repo_transaction_set_refspec (repo, ref, NULL);
    }

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  ostree_repo_abort_transaction (repo, cancellable, NULL);
  return ret;
}

static gboolean
generate_deployment_refs (OstreeSysroot       *self,
                          OstreeRepo          *repo,
                          int                  bootversion,
                          int                  subbootversion,
                          GPtrArray           *deployments,
                          GCancellable        *cancellable,
                          GError             **error)
{
  gboolean ret = FALSE;
  int cleanup_bootversion;
  int cleanup_subbootversion;
  guint i;

  cleanup_bootversion = (bootversion == 0) ? 1 : 0;
  cleanup_subbootversion = (subbootversion == 0) ? 1 : 0;

  if (!cleanup_ref_prefix (repo, cleanup_bootversion, 0,
                           cancellable, error))
    goto out;

  if (!cleanup_ref_prefix (repo, cleanup_bootversion, 1,
                           cancellable, error))
    goto out;

  if (!cleanup_ref_prefix (repo, bootversion, cleanup_subbootversion,
                           cancellable, error))
    goto out;

  for (i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      g_autofree char *refname = g_strdup_printf ("ostree/%d/%d/%u",
                                               bootversion, subbootversion,
                                               i);

      if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
        goto out;

      ostree_repo_transaction_set_refspec (repo, refname, ostree_deployment_get_csum (deployment));

      if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  ostree_repo_abort_transaction (repo, cancellable, NULL);
  return ret;
}

static gboolean
prune_repo (OstreeRepo    *repo,
            GCancellable  *cancellable,
            GError       **error)
{
  gint n_objects_total;
  gint n_objects_pruned;
  guint64 freed_space;
  gboolean ret = FALSE;

  if (!ostree_repo_prune (repo, OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY, 0,
                          &n_objects_total, &n_objects_pruned, &freed_space,
                          cancellable, error))
    goto out;

  if (freed_space > 0)
    {
      char *freed_space_str = g_format_size_full (freed_space, 0);
      g_print ("Freed objects: %s\n", freed_space_str);
    }

  ret = TRUE;

out:
  return ret;
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
  OstreeSysrootCleanupFlags flags;

  /* Do everything. */
  flags = OSTREE_SYSROOT_CLEANUP_ALL;

  return _ostree_sysroot_piecemeal_cleanup (self, flags, cancellable, error);
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
  OstreeSysrootCleanupFlags flags;

  /* Do everything EXCEPT pruning the repository. */
  flags = OSTREE_SYSROOT_CLEANUP_ALL & ~OSTREE_SYSROOT_CLEANUP_PRUNE_REPO;

  return _ostree_sysroot_piecemeal_cleanup (self, flags, cancellable, error);
}

gboolean
_ostree_sysroot_piecemeal_cleanup (OstreeSysroot              *self,
                                   OstreeSysrootCleanupFlags   flags,
                                   GCancellable               *cancellable,
                                   GError                    **error)
{
  gboolean ret = FALSE;
  glnx_unref_object OstreeRepo *repo = NULL;

  g_return_val_if_fail (OSTREE_IS_SYSROOT (self), FALSE);
  g_return_val_if_fail (self->loaded, FALSE);

  if (flags & OSTREE_SYSROOT_CLEANUP_BOOTVERSIONS)
    {
      if (!cleanup_other_bootversions (self, cancellable, error))
        goto out;
    }

  if (flags & OSTREE_SYSROOT_CLEANUP_DEPLOYMENTS)
    {
      if (!cleanup_old_deployments (self, cancellable, error))
        goto out;
    }

  if (!ostree_sysroot_get_repo (self, &repo, cancellable, error))
    goto out;
  
  if (!generate_deployment_refs (self, repo,
                                 self->bootversion,
                                 self->subbootversion,
                                 self->deployments,
                                 cancellable, error))
    goto out;
  
  if (flags & OSTREE_SYSROOT_CLEANUP_PRUNE_REPO)
    {
      if (!prune_repo (repo, cancellable, error))
            goto out;
    }

  ret = TRUE;
 out:
  return ret;
}
