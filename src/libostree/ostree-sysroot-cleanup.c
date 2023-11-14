/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ostree-core-private.h"
#include "ostree-linuxfsutil.h"
#include "ostree-repo-private.h"
#include "otcore.h"
#include "otutil.h"

#include "ostree-sysroot-private.h"

/* @deploydir_dfd: Directory FD for ostree/deploy
 * @osname: Target osname
 * @inout_deployments: All deployments in this subdir will be appended to this array
 */
gboolean
_ostree_sysroot_list_deployment_dirs_for_os (int deploydir_dfd, const char *osname,
                                             GPtrArray *inout_deployments,
                                             GCancellable *cancellable, GError **error)
{
  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };
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

      g_ptr_array_add (inout_deployments,
                       ostree_deployment_new (-1, osname, csum, deployserial, NULL, -1));
    }

  return TRUE;
}

/* Return in @out_deployments a new array of OstreeDeployment loaded from the
 * filesystem state.
 */
static gboolean
list_all_deployment_directories (OstreeSysroot *self, GPtrArray **out_deployments,
                                 GCancellable *cancellable, GError **error)
{
  g_autoptr (GPtrArray) ret_deployments = g_ptr_array_new_with_free_func (g_object_unref);

  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };
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

      if (!_ostree_sysroot_list_deployment_dirs_for_os (dfd_iter.fd, dent->d_name, ret_deployments,
                                                        cancellable, error))
        return FALSE;
    }

  ot_transfer_out_value (out_deployments, &ret_deployments);
  return TRUE;
}

gboolean
_ostree_sysroot_parse_bootdir_name (const char *name, char **out_osname, char **out_csum)
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

gboolean
_ostree_sysroot_list_all_boot_directories (OstreeSysroot *self, char ***out_bootdirs,
                                           GCancellable *cancellable, GError **error)
{
  g_autoptr (GPtrArray) ret_bootdirs = g_ptr_array_new_with_free_func (g_free);

  gboolean exists = FALSE;
  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };
  if (self->boot_fd >= 0
      && !ot_dfd_iter_init_allow_noent (self->boot_fd, "ostree", &dfd_iter, &exists, error))
    return FALSE;

  while (exists)
    {
      struct dirent *dent;
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (dent->d_type != DT_DIR)
        continue;

      /* Only look at directories ending in -CHECKSUM; nothing else
       * should be in here, but let's be conservative.
       */
      if (!_ostree_sysroot_parse_bootdir_name (dent->d_name, NULL, NULL))
        continue;

      g_ptr_array_add (ret_bootdirs, g_strdup (dent->d_name));
    }

  g_ptr_array_add (ret_bootdirs, NULL);
  *out_bootdirs = (char **)g_ptr_array_free (g_steal_pointer (&ret_bootdirs), FALSE);
  return TRUE;
}

/* A sysroot has at most one active "boot version" (pair of version,subversion)
 * out of a total of 4 possible. This function deletes from the filesystem the 3
 * other versions that aren't active.
 */
static gboolean
cleanup_other_bootversions (OstreeSysroot *self, GCancellable *cancellable, GError **error)
{
  const int cleanup_bootversion = self->bootversion == 0 ? 1 : 0;
  const int cleanup_subbootversion = self->subbootversion == 0 ? 1 : 0;
  /* Reusable buffer for path */
  g_autoptr (GString) buf = g_string_new ("");

  /* These directories are for the other major version */
  g_string_truncate (buf, 0);
  g_string_append_printf (buf, "boot/loader.%d", cleanup_bootversion);
  if (!glnx_shutil_rm_rf_at (self->sysroot_fd, buf->str, cancellable, error))
    return FALSE;
  g_string_truncate (buf, 0);
  g_string_append_printf (buf, "ostree/boot.%d", cleanup_bootversion);
  if (!glnx_shutil_rm_rf_at (self->sysroot_fd, buf->str, cancellable, error))
    return FALSE;
  g_string_truncate (buf, 0);
  g_string_append_printf (buf, "ostree/boot.%d.0", cleanup_bootversion);
  if (!glnx_shutil_rm_rf_at (self->sysroot_fd, buf->str, cancellable, error))
    return FALSE;
  g_string_truncate (buf, 0);
  g_string_append_printf (buf, "ostree/boot.%d.1", cleanup_bootversion);
  if (!glnx_shutil_rm_rf_at (self->sysroot_fd, buf->str, cancellable, error))
    return FALSE;

  /* And finally the other subbootversion */
  g_string_truncate (buf, 0);
  g_string_append_printf (buf, "ostree/boot.%d.%d", self->bootversion, cleanup_subbootversion);
  if (!glnx_shutil_rm_rf_at (self->sysroot_fd, buf->str, cancellable, error))
    return FALSE;

  return TRUE;
}

/* Delete a deployment directory */
gboolean
_ostree_sysroot_rmrf_deployment (OstreeSysroot *self, OstreeDeployment *deployment,
                                 GCancellable *cancellable, GError **error)
{
  g_autofree char *origin_relpath = ostree_deployment_get_origin_relpath (deployment);
  g_autofree char *deployment_path = ostree_sysroot_get_deployment_dirpath (self, deployment);
  struct stat stbuf;
  glnx_autofd int deployment_fd = -1;

  if (!glnx_opendirat (self->sysroot_fd, deployment_path, TRUE, &deployment_fd, error))
    return FALSE;

  if (!glnx_fstat (deployment_fd, &stbuf, error))
    return FALSE;

  /* This shouldn't happen, because higher levels should
   * disallow having the booted deployment not in the active
   * deployment list, but let's be extra safe. */
  if (stbuf.st_dev == self->root_device && stbuf.st_ino == self->root_inode)
    return TRUE;

  /* This deployment wasn't referenced, so delete it */
  if (!_ostree_linuxfs_fd_alter_immutable_flag (deployment_fd, FALSE, cancellable, error))
    return FALSE;
  if (!glnx_shutil_rm_rf_at (self->sysroot_fd, origin_relpath, cancellable, error))
    return FALSE;
  if (!glnx_shutil_rm_rf_at (self->sysroot_fd, deployment_path, cancellable, error))
    return FALSE;

  return TRUE;
}

/* As the bootloader configuration changes, we will have leftover deployments
 * on disk.  This function deletes all deployments which aren't actively
 * referenced.
 */
static gboolean
cleanup_old_deployments (OstreeSysroot *self, GCancellable *cancellable, GError **error)
{
  /* Load all active deployments referenced by bootloader configuration. */
  g_autoptr (GHashTable) active_deployment_dirs
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < self->deployments->len; i++)
    {
      OstreeDeployment *deployment = self->deployments->pdata[i];
      char *deployment_path = ostree_sysroot_get_deployment_dirpath (self, deployment);
      /* Transfer ownership */
      g_hash_table_replace (active_deployment_dirs, deployment_path, deployment_path);
    }

  /* Find all deployment directories, both active and inactive */
  g_autoptr (GPtrArray) all_deployment_dirs = NULL;
  if (!list_all_deployment_directories (self, &all_deployment_dirs, cancellable, error))
    return FALSE;
  g_assert (all_deployment_dirs); /* Pacify static analysis */
  for (guint i = 0; i < all_deployment_dirs->len; i++)
    {
      OstreeDeployment *deployment = all_deployment_dirs->pdata[i];
      g_autofree char *deployment_path = ostree_sysroot_get_deployment_dirpath (self, deployment);

      if (g_hash_table_lookup (active_deployment_dirs, deployment_path))
        continue;

      if (!_ostree_sysroot_rmrf_deployment (self, deployment, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/* This function deletes any files in the bootfs unreferenced by the active
 * bootloader configuration.
 */
gboolean
_ostree_sysroot_cleanup_bootfs (OstreeSysroot *self, GCancellable *cancellable, GError **error)
{
  /* Load all active bootcsums and overlays referenced by bootloader configuration. */
  g_autoptr (GHashTable) active_boot_checksums
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr (GHashTable) active_overlay_initrds
      = g_hash_table_new (g_str_hash, g_str_equal); /* borrows from deployment's bootconfig */
  for (guint i = 0; i < self->deployments->len; i++)
    {
      OstreeDeployment *deployment = self->deployments->pdata[i];
      char *bootcsum = g_strdup (ostree_deployment_get_bootcsum (deployment));
      /* Transfer ownership */
      g_hash_table_replace (active_boot_checksums, bootcsum, bootcsum);

      OstreeBootconfigParser *bootconfig = ostree_deployment_get_bootconfig (deployment);
      char **initrds = ostree_bootconfig_parser_get_overlay_initrds (bootconfig);
      for (char **it = initrds; it && *it; it++)
        g_hash_table_add (active_overlay_initrds, (char *)glnx_basename (*it));
    }

  /* Clean up boot directories */
  g_auto (GStrv) all_boot_dirs = NULL;
  if (!_ostree_sysroot_list_all_boot_directories (self, &all_boot_dirs, cancellable, error))
    return FALSE;

  for (char **it = all_boot_dirs; it && *it; it++)
    {
      char *bootdir = *it;
      g_autofree char *bootcsum = NULL;

      if (!_ostree_sysroot_parse_bootdir_name (bootdir, NULL, &bootcsum))
        g_assert_not_reached (); /* checked in _ostree_sysroot_list_all_boot_directories() */

      if (g_hash_table_lookup (active_boot_checksums, bootcsum))
        continue;

      g_autofree char *subpath = g_build_filename ("ostree", bootdir, NULL);
      if (!glnx_shutil_rm_rf_at (self->boot_fd, subpath, cancellable, error))
        return FALSE;
    }

  /* Clean up overlay initrds */
  glnx_autofd int overlays_dfd
      = glnx_opendirat_with_errno (self->sysroot_fd, _OSTREE_SYSROOT_INITRAMFS_OVERLAYS, FALSE);
  if (overlays_dfd < 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "open(initrd_overlays)");
    }
  else
    {
      g_autoptr (GPtrArray) initrds_to_delete = g_ptr_array_new_with_free_func (g_free);
      g_auto (GLnxDirFdIterator) dfd_iter = {
        0,
      };
      if (!glnx_dirfd_iterator_init_at (overlays_dfd, ".", TRUE, &dfd_iter, error))
        return FALSE;
      while (TRUE)
        {
          struct dirent *dent;
          if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
            return FALSE;
          if (dent == NULL)
            break;

          /* there shouldn't be other file types there, but let's be conservative */
          if (dent->d_type != DT_REG)
            continue;

          if (!g_hash_table_lookup (active_overlay_initrds, dent->d_name))
            g_ptr_array_add (initrds_to_delete, g_strdup (dent->d_name));
        }
      for (guint i = 0; i < initrds_to_delete->len; i++)
        {
          if (!ot_ensure_unlinked_at (overlays_dfd, initrds_to_delete->pdata[i], error))
            return FALSE;
        }
    }

  return TRUE;
}

/* Delete the ref bindings for a non-active boot version */
static gboolean
cleanup_ref_prefix (OstreeRepo *repo, int bootversion, int subbootversion,
                    GCancellable *cancellable, GError **error)
{
  g_autofree char *prefix = g_strdup_printf ("ostree/%d/%d", bootversion, subbootversion);
  g_autoptr (GHashTable) refs = NULL;
  if (!ostree_repo_list_refs_ext (repo, prefix, &refs, OSTREE_REPO_LIST_REFS_EXT_NONE, cancellable,
                                  error))
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
generate_deployment_refs (OstreeSysroot *self, OstreeRepo *repo, int bootversion,
                          int subbootversion, GPtrArray *deployments, GCancellable *cancellable,
                          GError **error)
{
  int cleanup_bootversion = (bootversion == 0) ? 1 : 0;
  int cleanup_subbootversion = (subbootversion == 0) ? 1 : 0;

  if (!cleanup_ref_prefix (repo, cleanup_bootversion, 0, cancellable, error))
    return FALSE;

  if (!cleanup_ref_prefix (repo, cleanup_bootversion, 1, cancellable, error))
    return FALSE;

  if (!cleanup_ref_prefix (repo, bootversion, cleanup_subbootversion, cancellable, error))
    return FALSE;

  g_autoptr (OstreeRepoAutoTransaction) txn
      = _ostree_repo_auto_transaction_start (repo, cancellable, error);
  if (!txn)
    return FALSE;
  for (guint i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      g_autofree char *refname
          = g_strdup_printf ("ostree/%d/%d/%u", bootversion, subbootversion, i);

      ostree_repo_transaction_set_refspec (repo, refname, ostree_deployment_get_csum (deployment));
    }
  if (!_ostree_repo_auto_transaction_commit (txn, NULL, cancellable, error))
    return FALSE;

  return TRUE;
}

/**
 * ostree_sysroot_cleanup_prune_repo:
 * @sysroot: Sysroot
 * @options: Flags controlling pruning
 * @out_objects_total: (out): Number of objects found
 * @out_objects_pruned: (out): Number of objects deleted
 * @out_pruned_object_size_total: (out): Storage size in bytes of objects deleted
 * @cancellable: Cancellable
 * @error: Error
 *
 * Prune the system repository.  This is a thin wrapper
 * around ostree_repo_prune_from_reachable(); the primary
 * addition is that this function automatically gathers
 * all deployed commits into the reachable set.
 *
 * You generally want to at least set the `OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY`
 * flag in @options.  A commit traversal depth of `0` is assumed.
 *
 * Locking: exclusive
 * Since: 2018.6
 */
gboolean
ostree_sysroot_cleanup_prune_repo (OstreeSysroot *sysroot, OstreeRepoPruneOptions *options,
                                   gint *out_objects_total, gint *out_objects_pruned,
                                   guint64 *out_pruned_object_size_total, GCancellable *cancellable,
                                   GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Pruning system repository", error);
  OstreeRepo *repo = ostree_sysroot_repo (sysroot);
  const guint depth = 0; /* Historical default */

  if (!_ostree_sysroot_ensure_writable (sysroot, error))
    return FALSE;

  /* Hold an exclusive lock by default across gathering refs and doing
   * the prune.
   */
  g_autoptr (OstreeRepoAutoLock) lock
      = ostree_repo_auto_lock_push (repo, OSTREE_REPO_LOCK_EXCLUSIVE, cancellable, error);
  if (!lock)
    return FALSE;

  /* Ensure reachable has refs, but default to depth 0.  This is
   * what we've always done for the system repo, but perhaps down
   * the line we could add a depth flag to the repo config or something?
   */
  if (!ostree_repo_traverse_reachable_refs (repo, depth, options->reachable, cancellable, error))
    return FALSE;

  /* Since ostree was created we've been generating "deployment refs" in
   * generate_deployment_refs() that look like ostree/0/1 etc. to ensure that
   * anything doing a direct prune won't delete commits backing deployments.
   * This bit might allow us to eventually drop that behavior, although we'd
   * have to be very careful to ensure that all software is updated to use
   * `ostree_sysroot_cleanup_prune_repo()`.
   */
  for (guint i = 0; i < sysroot->deployments->len; i++)
    {
      const char *checksum = ostree_deployment_get_csum (sysroot->deployments->pdata[i]);
      if (!ostree_repo_traverse_commit_union (repo, checksum, depth, options->reachable,
                                              cancellable, error))
        return FALSE;
    }

  if (!ostree_repo_prune_from_reachable (repo, options, out_objects_total, out_objects_pruned,
                                         out_pruned_object_size_total, cancellable, error))
    return FALSE;

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
ostree_sysroot_cleanup (OstreeSysroot *self, GCancellable *cancellable, GError **error)
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
ostree_sysroot_prepare_cleanup (OstreeSysroot *self, GCancellable *cancellable, GError **error)
{
  return _ostree_sysroot_cleanup_internal (self, FALSE, cancellable, error);
}

gboolean
_ostree_sysroot_cleanup_internal (OstreeSysroot *self, gboolean do_prune_repo,
                                  GCancellable *cancellable, GError **error)
{
  g_assert (OSTREE_IS_SYSROOT (self));
  g_assert (self->loadstate == OSTREE_SYSROOT_LOAD_STATE_LOADED);

  if (!_ostree_sysroot_ensure_writable (self, error))
    return FALSE;

  if (!cleanup_other_bootversions (self, cancellable, error))
    return glnx_prefix_error (error, "Cleaning bootversions");

  if (!cleanup_old_deployments (self, cancellable, error))
    return glnx_prefix_error (error, "Cleaning deployments");

  if (!_ostree_sysroot_cleanup_bootfs (self, cancellable, error))
    return glnx_prefix_error (error, "Cleaning bootfs");

  OstreeRepo *repo = ostree_sysroot_repo (self);
  if (!generate_deployment_refs (self, repo, self->bootversion, self->subbootversion,
                                 self->deployments, cancellable, error))
    return glnx_prefix_error (error, "Generating deployment refs");

  if (do_prune_repo)
    {
      gint n_objects_total;
      gint n_objects_pruned;
      guint64 freed_space;
      g_autoptr (GHashTable) reachable = ostree_repo_traverse_new_reachable ();
      OstreeRepoPruneOptions opts = { OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY, reachable };
      if (!ostree_sysroot_cleanup_prune_repo (self, &opts, &n_objects_total, &n_objects_pruned,
                                              &freed_space, cancellable, error))
        return FALSE;

      /* TODO remove printf in library */
      if (freed_space > 0)
        {
          g_autofree char *freed_space_str = g_format_size_full (freed_space, 0);
          g_print ("Freed objects: %s\n", freed_space_str);
        }
    }

  return TRUE;
}

/**
 * ostree_sysroot_update_post_copy:
 * @self: Sysroot
 * @error: Error
 *
 * Update a sysroot as needed after having copied it into place using file-level
 * operations. This enables options like fs-verity on the required files that may
 * have been lost during the copy.
 *
 * Since: 2023.11
 */
gboolean
ostree_sysroot_update_post_copy (OstreeSysroot *self, GCancellable *cancellable, GError **error)
{
  OstreeRepo *repo = ostree_sysroot_repo (self);

  if (repo->fs_verity_wanted == _OSTREE_FEATURE_NO)
    return TRUE;

  g_autoptr (GHashTable) objects
      = ostree_repo_list_objects_set (repo, OSTREE_REPO_LIST_OBJECTS_LOOSE, cancellable, error);
  if (objects == NULL)
    return FALSE;

  GLNX_HASH_TABLE_FOREACH (objects, GVariant *, key)
    {
      const char *checksum;
      OstreeObjectType objtype;

      ostree_object_name_deserialize (key, &checksum, &objtype);

      char loose_path_buf[_OSTREE_LOOSE_PATH_MAX];
      _ostree_loose_path (loose_path_buf, checksum, objtype, repo->mode);

      gboolean supported;
      if (!_ostree_ensure_fsverity (repo, FALSE, repo->objects_dir_fd, loose_path_buf, &supported,
                                    error))
        return FALSE;

      if (!supported)
        break; /* If not supported, skip rest */
    }

  g_autoptr (GPtrArray) all_deployment_dirs = NULL;
  if (!list_all_deployment_directories (self, &all_deployment_dirs, cancellable, error))
    return FALSE;
  g_assert (all_deployment_dirs); /* Pacify static analysis */
  for (guint i = 0; i < all_deployment_dirs->len; i++)
    {
      OstreeDeployment *deployment = all_deployment_dirs->pdata[i];
      g_autofree char *deployment_path = ostree_sysroot_get_deployment_dirpath (self, deployment);

      g_autofree char *cfs_file = g_build_filename (deployment_path, OSTREE_COMPOSEFS_NAME, NULL);

      gboolean supported;
      if (!_ostree_ensure_fsverity (repo, TRUE, self->sysroot_fd, cfs_file, &supported, error))
        return FALSE;

      if (!supported)
        break; /* If not supported, skip rest */
    }

  return TRUE;
}
