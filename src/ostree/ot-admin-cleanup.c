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

#include "ot-admin-functions.h"
#include "ot-deployment.h"
#include "ot-config-parser.h"
#include "otutil.h"
#include "ostree.h"
#include "libgsystem.h"

static gboolean
list_deployment_dirs_for_os (GFile               *osdir,
                             GPtrArray           *inout_deployments,
                             GCancellable        *cancellable,
                             GError             **error)
{
  gboolean ret = FALSE;
  const char *osname = gs_file_get_basename_cached (osdir);
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFile *osdeploy_dir = NULL;
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
      gs_unref_object OtDeployment *deployment = NULL;
      gs_free char *csum = NULL;
      gint deployserial;

      if (!gs_file_enumerator_iterate (dir_enum, &file_info, &child,
                                       cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      name = g_file_info_get_name (file_info);

      if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
        continue;

      if (!ot_admin_parse_deploy_path_name (name, &csum, &deployserial, error))
        goto out;
      
      deployment = ot_deployment_new (-1, osname, csum, deployserial, NULL, -1);
      g_ptr_array_add (inout_deployments, g_object_ref (deployment));
    }

 done:
  ret = TRUE;
 out:
  return ret;
}

static gboolean
list_all_deployment_directories (GFile               *sysroot,
                                 GPtrArray          **out_deployments,
                                 GCancellable        *cancellable,
                                 GError             **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFile *deploydir = NULL;
  gs_unref_ptrarray GPtrArray *ret_deployments = NULL;
  GError *temp_error = NULL;

  deploydir = g_file_resolve_relative_path (sysroot, "ostree/deploy");

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

      if (!gs_file_enumerator_iterate (dir_enum, &file_info, &child,
                                       NULL, error))
        goto out;
      if (file_info == NULL)
        break;

      if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
        continue;
      
      if (!list_deployment_dirs_for_os (child, ret_deployments, cancellable, error))
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
list_all_boot_directories (GFile               *sysroot,
                           GPtrArray          **out_bootdirs,
                           GCancellable        *cancellable,
                           GError             **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFile *boot_ostree = NULL;
  gs_unref_ptrarray GPtrArray *ret_bootdirs = NULL;
  GError *temp_error = NULL;

  boot_ostree = g_file_resolve_relative_path (sysroot, "boot/ostree");

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

      if (!gs_file_enumerator_iterate (dir_enum, &file_info, &child,
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
cleanup_other_bootversions (GFile               *sysroot,
                            int                  bootversion,
                            int                  subbootversion,
                            GCancellable        *cancellable,
                            GError             **error)
{
  gboolean ret = FALSE;
  int cleanup_bootversion;
  int cleanup_subbootversion;
  gs_unref_object GFile *cleanup_boot_dir = NULL;

  cleanup_bootversion = bootversion == 0 ? 1 : 0;
  cleanup_subbootversion = subbootversion == 0 ? 1 : 0;

  cleanup_boot_dir = ot_gfile_resolve_path_printf (sysroot, "boot/loader.%d", cleanup_bootversion);
  if (!gs_shutil_rm_rf (cleanup_boot_dir, cancellable, error))
    goto out;
  g_clear_object (&cleanup_boot_dir);

  cleanup_boot_dir = ot_gfile_resolve_path_printf (sysroot, "ostree/boot.%d", cleanup_bootversion);
  if (!gs_shutil_rm_rf (cleanup_boot_dir, cancellable, error))
    goto out;
  g_clear_object (&cleanup_boot_dir);

  cleanup_boot_dir = ot_gfile_resolve_path_printf (sysroot, "ostree/boot.%d.0", cleanup_bootversion);
  if (!gs_shutil_rm_rf (cleanup_boot_dir, cancellable, error))
    goto out;
  g_clear_object (&cleanup_boot_dir);

  cleanup_boot_dir = ot_gfile_resolve_path_printf (sysroot, "ostree/boot.%d.1", cleanup_bootversion);
  if (!gs_shutil_rm_rf (cleanup_boot_dir, cancellable, error))
    goto out;
  g_clear_object (&cleanup_boot_dir);

  cleanup_boot_dir = ot_gfile_resolve_path_printf (sysroot, "ostree/boot.%d.%d", bootversion,
                                                   cleanup_subbootversion);
  if (!gs_shutil_rm_rf (cleanup_boot_dir, cancellable, error))
    goto out;
  g_clear_object (&cleanup_boot_dir);

  ret = TRUE;
 out:
  return ret;
}

static gboolean
cleanup_old_deployments (GFile               *sysroot,
                         GPtrArray           *deployments,
                         GCancellable        *cancellable,
                         GError             **error)
{
  gboolean ret = FALSE;
  guint32 root_device;
  guint64 root_inode;
  guint i;
  gs_unref_object GFile *active_root = g_file_new_for_path ("/");
  gs_unref_hashtable GHashTable *active_deployment_dirs = NULL;
  gs_unref_hashtable GHashTable *active_boot_checksums = NULL;
  gs_unref_ptrarray GPtrArray *all_deployment_dirs = NULL;
  gs_unref_ptrarray GPtrArray *all_boot_dirs = NULL;

  if (!ot_admin_util_get_devino (active_root, &root_device, &root_inode,
                                 cancellable, error))
    goto out;

  active_deployment_dirs = g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal, NULL, g_object_unref);
  active_boot_checksums = g_hash_table_new_full (g_str_hash, (GEqualFunc)g_str_equal, g_free, NULL);

  for (i = 0; i < deployments->len; i++)
    {
      OtDeployment *deployment = deployments->pdata[i];
      GFile *deployment_path = ot_admin_get_deployment_directory (sysroot, deployment);
      char *bootcsum = g_strdup (ot_deployment_get_bootcsum (deployment));
      /* Transfer ownership */
      g_hash_table_insert (active_deployment_dirs, deployment_path, deployment_path);
      g_hash_table_insert (active_boot_checksums, bootcsum, bootcsum);
    }

  if (!list_all_deployment_directories (sysroot, &all_deployment_dirs,
                                        cancellable, error))
    goto out;
  
  for (i = 0; i < all_deployment_dirs->len; i++)
    {
      OtDeployment *deployment = all_deployment_dirs->pdata[i];
      gs_unref_object GFile *deployment_path = ot_admin_get_deployment_directory (sysroot, deployment);
      gs_unref_object GFile *origin_path = ot_admin_get_deployment_origin_path (deployment_path);
      if (!g_hash_table_lookup (active_deployment_dirs, deployment_path))
        {
          guint32 device;
          guint64 inode;

          if (!ot_admin_util_get_devino (deployment_path, &device, &inode,
                                         cancellable, error))
            goto out;

          /* This shouldn't happen, because higher levels should
           * disallow having the booted deployment not in the active
           * deployment list, but let's be extra safe. */
          if (device == root_device && inode == root_inode)
            continue;

          g_print ("ostadmin: Deleting deployment %s\n", gs_file_get_path_cached (deployment_path));
          if (!gs_shutil_rm_rf (deployment_path, cancellable, error))
            goto out;
          if (!gs_shutil_rm_rf (origin_path, cancellable, error))
            goto out;
        }
    }

  if (!list_all_boot_directories (sysroot, &all_boot_dirs,
                                  cancellable, error))
    goto out;
  
  for (i = 0; i < all_boot_dirs->len; i++)
    {
      GFile *bootdir = all_boot_dirs->pdata[i];
      gs_free char *osname = NULL;
      gs_free char *bootcsum = NULL;

      if (!parse_bootdir_name (gs_file_get_basename_cached (bootdir),
                               &osname, &bootcsum))
        g_assert_not_reached ();

      if (g_hash_table_lookup (active_boot_checksums, bootcsum))
        continue;

      g_print ("ostadmin: Deleting bootdir %s\n", gs_file_get_path_cached (bootdir));
      if (!gs_shutil_rm_rf (bootdir, cancellable, error))
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
  gs_free char *prefix = NULL;
  gs_unref_hashtable GHashTable *refs = NULL;
  GHashTableIter hashiter;
  gpointer hashkey, hashvalue;

  prefix = g_strdup_printf ("ostree/%d/%d", bootversion, subbootversion);

  if (!ostree_repo_list_refs (repo, prefix, &refs, cancellable, error))
    goto out;

  g_hash_table_iter_init (&hashiter, refs);
  while (g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue))
    {
      const char *suffix = hashkey;
      gs_free char *ref = g_strconcat (prefix, "/", suffix, NULL);
      if (!ostree_repo_write_refspec (repo, ref, NULL, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
generate_deployment_refs_and_prune (GFile               *sysroot,
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
  gint n_objects_total, n_objects_pruned;
  guint64 freed_space;

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
      OtDeployment *deployment = deployments->pdata[i];
      gs_free char *refname = g_strdup_printf ("ostree/%d/%d/%u",
                                               bootversion, subbootversion,
                                               i);
      if (!ostree_repo_write_refspec (repo, refname, ot_deployment_get_csum (deployment),
                                      error))
        goto out;
    }

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
  
gboolean
ot_admin_cleanup (GFile               *sysroot,
                  GCancellable        *cancellable,
                  GError             **error)
{
  gboolean ret = FALSE;
  gs_unref_ptrarray GPtrArray *deployments = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  int bootversion;
  int subbootversion;

  if (!ot_admin_list_deployments (sysroot, &bootversion, &deployments,
                                  cancellable, error))
    goto out;

  if (!ot_admin_read_current_subbootversion (sysroot, bootversion, &subbootversion,
                                             cancellable, error))
    goto out;

  if (!cleanup_other_bootversions (sysroot, bootversion, subbootversion,
                                   cancellable, error))
    goto out;

  if (!cleanup_old_deployments (sysroot, deployments,
                                cancellable, error))
    goto out;

  if (deployments->len > 0)
    {
      if (!ot_admin_get_repo (sysroot, &repo, cancellable, error))
        goto out;

      if (!generate_deployment_refs_and_prune (sysroot, repo, bootversion,
                                               subbootversion, deployments,
                                               cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}
