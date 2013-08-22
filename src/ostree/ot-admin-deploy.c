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
#include "ot-admin-deploy.h"
#include "ot-deployment.h"
#include "ot-config-parser.h"
#include "ot-bootloader-syslinux.h"
#include "otutil.h"
#include "ostree-core.h"
#include "libgsystem.h"


/**
 * copy_one_config_file:
 *
 * Copy @file from @modified_etc to @new_etc, overwriting any existing
 * file there.
 */
static gboolean
copy_one_config_file (GFile              *orig_etc,
                      GFile              *modified_etc,
                      GFile              *new_etc,
                      GFile              *src,
                      GCancellable       *cancellable,
                      GError            **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileInfo *src_info = NULL;
  gs_unref_object GFile *dest = NULL;
  gs_unref_object GFile *parent = NULL;
  gs_free char *relative_path = NULL;
  
  relative_path = g_file_get_relative_path (modified_etc, src);
  g_assert (relative_path);
  dest = g_file_resolve_relative_path (new_etc, relative_path);

  src_info = g_file_query_info (src, OSTREE_GIO_FAST_QUERYINFO, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                cancellable, error);
  if (!src_info)
    goto out;

  if (g_file_info_get_file_type (src_info) == G_FILE_TYPE_DIRECTORY)
    {
      gs_unref_object GFileEnumerator *src_enum = NULL;
      gs_unref_object GFileInfo *child_info = NULL;
      GError *temp_error = NULL;

      /* FIXME actually we need to copy permissions and xattrs */
      if (!gs_file_ensure_directory (dest, TRUE, cancellable, error))
        goto out;

      src_enum = g_file_enumerate_children (src, OSTREE_GIO_FAST_QUERYINFO,
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            cancellable, error);

      while ((child_info = g_file_enumerator_next_file (src_enum, cancellable, error)) != NULL)
        {
          gs_unref_object GFile *child = g_file_get_child (src, g_file_info_get_name (child_info));

          if (!copy_one_config_file (orig_etc, modified_etc, new_etc, child,
                                     cancellable, error))
            goto out;
        }
      g_clear_object (&child_info);
      if (temp_error != NULL)
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }
  else
    {
      parent = g_file_get_parent (dest);

      /* FIXME actually we need to copy permissions and xattrs */
      if (!gs_file_ensure_directory (parent, TRUE, cancellable, error))
        goto out;
      
      /* We unlink here because otherwise gio throws an error on
       * dangling symlinks.
       */
      if (!ot_gfile_ensure_unlinked (dest, cancellable, error))
        goto out;

      if (!g_file_copy (src, dest, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA,
                        cancellable, NULL, NULL, error))
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
  gs_unref_object GFile *ostree_etc = NULL;
  gs_unref_object GFile *tmp_etc = NULL;
  gs_unref_ptrarray GPtrArray *modified = NULL;
  gs_unref_ptrarray GPtrArray *removed = NULL;
  gs_unref_ptrarray GPtrArray *added = NULL;
  guint i;

  modified = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_diff_item_unref);
  removed = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  added = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

  if (!ostree_diff_dirs (orig_etc, modified_etc, modified, removed, added,
                         cancellable, error))
    {
      g_prefix_error (error, "While computing configuration diff: ");
      goto out;
    }

  if (modified->len > 0 || removed->len > 0 || added->len > 0)
    g_print ("ostadmin: Processing config: %u modified, %u removed, %u added\n", 
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

      if (!ot_gfile_ensure_unlinked (target_file, cancellable, error))
        goto out;
    }

  for (i = 0; i < modified->len; i++)
    {
      OstreeDiffItem *diff = modified->pdata[i];

      if (!copy_one_config_file (orig_etc, modified_etc, new_etc, diff->target,
                                 cancellable, error))
        goto out;
    }
  for (i = 0; i < added->len; i++)
    {
      GFile *file = added->pdata[i];

      if (!copy_one_config_file (orig_etc, modified_etc, new_etc, file,
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
checkout_deployment_tree (GFile             *sysroot,
                          OstreeRepo        *repo,
                          OtDeployment      *deployment,
                          GFile            **out_deployment_path,
                          GCancellable      *cancellable,
                          GError           **error)
{
  gboolean ret = FALSE;
  const char *csum = ot_deployment_get_csum (deployment);
  gs_unref_object OstreeRepoFile *root = NULL;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_object GFileInfo *existing_checkout_info = NULL;
  gs_free char *checkout_target_name = NULL;
  gs_free char *checkout_target_tmp_name = NULL;
  gs_unref_object GFile *osdeploy_path = NULL;
  gs_unref_object GFile *deploy_target_path = NULL;
  gs_unref_object GFile *deploy_parent = NULL;

  root = (OstreeRepoFile*)ostree_repo_file_new_root (repo, csum);
  if (!ostree_repo_file_ensure_resolved (root, error))
    goto out;

  file_info = g_file_query_info ((GFile*)root, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!file_info)
    goto out;

  osdeploy_path = ot_gfile_get_child_build_path (sysroot, "ostree", "deploy",
                                                 ot_deployment_get_osname (deployment),
                                                 "deploy", NULL);
  checkout_target_name = g_strdup_printf ("%s.%d", csum, ot_deployment_get_deployserial (deployment));
  deploy_target_path = g_file_get_child (osdeploy_path, checkout_target_name);

  deploy_parent = g_file_get_parent (deploy_target_path);
  if (!gs_file_ensure_directory (deploy_parent, TRUE, cancellable, error))
    goto out;
  
  g_print ("ostadmin: Creating deployment %s\n",
           gs_file_get_path_cached (deploy_target_path));

  if (!ostree_repo_checkout_tree (repo, 0, 0, deploy_target_path, root,
                                  file_info, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_deployment_path, &deploy_target_path);
 out:
  return ret;
}

static gboolean
merge_configuration (GFile             *sysroot,
                     OtDeployment      *previous_deployment,
                     OtDeployment      *deployment,
                     GFile             *deployment_path,
                     GCancellable      *cancellable,
                     GError           **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *source_etc_path = NULL;
  gs_unref_object GFile *source_etc_pristine_path = NULL;
  gs_unref_object GFile *deployment_usretc_path = NULL;
  gs_unref_object GFile *deployment_etc_path = NULL;
  gboolean etc_exists;
  gboolean usretc_exists;

  if (previous_deployment)
    {
      gs_unref_object GFile *previous_path = NULL;
      OtConfigParser *previous_bootconfig;

      previous_path = ot_admin_get_deployment_directory (sysroot, previous_deployment);
      source_etc_path = g_file_resolve_relative_path (previous_path, "etc");
      source_etc_pristine_path = g_file_resolve_relative_path (previous_path, "usr/etc");

      previous_bootconfig = ot_deployment_get_bootconfig (previous_deployment);
      if (previous_bootconfig)
        {
          const char *previous_options = ot_config_parser_get (previous_bootconfig, "options");
          /* Completely overwrite the previous options here; we will extend
           * them later.
           */
          ot_config_parser_set (ot_deployment_get_bootconfig (deployment), "options",
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
      g_assert (!etc_exists);
      if (!gs_shutil_cp_a (deployment_usretc_path, deployment_etc_path,
                           cancellable, error))
        goto out;
      g_print ("ostadmin: Created %s\n", gs_file_get_path_cached (deployment_etc_path));
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
 out:
  return ret;
}

static gboolean
write_origin_file (GFile             *sysroot,
                   OtDeployment      *deployment,
                   GCancellable      *cancellable,
                   GError           **error)
{
  gboolean ret = FALSE;
  GKeyFile *origin = ot_deployment_get_origin (deployment);

  if (origin)
    {
      gs_unref_object GFile *deployment_path = ot_admin_get_deployment_directory (sysroot, deployment);
      gs_unref_object GFile *origin_path = ot_admin_get_deployment_origin_path (deployment_path);
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
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_object GFile *ret_kernel = NULL;
  gs_unref_object GFile *ret_initramfs = NULL;

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
        ret_kernel = g_file_get_child (bootdir, name);
      else if (ret_initramfs == NULL && g_str_has_prefix (name, "initramfs-"))
        ret_initramfs = g_file_get_child (bootdir, name);
      
      if (ret_kernel && ret_initramfs)
        break;
    }

  if (ret_kernel == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Failed to find boot/vmlinuz-CHECKSUM in %s",
                   gs_file_get_path_cached (deployroot));
      goto out;
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

static int
sort_by_bootserial (gconstpointer ap, gconstpointer bp)
{
  OtDeployment **a_loc = (OtDeployment**)ap;
  OtDeployment *a = *a_loc;
  OtDeployment **b_loc = (OtDeployment**)bp;
  OtDeployment *b = *b_loc;

  if (ot_deployment_get_bootserial (a) == ot_deployment_get_bootserial (b))
    return 0;
  else if (ot_deployment_get_bootserial (a) < ot_deployment_get_bootserial (b))
    return -1;
  return 1;
}

static GPtrArray *
filter_deployments_by_bootcsum (GPtrArray    *deployments,
                                const char   *osname,
                                const char   *bootcsum)
{
  GPtrArray *ret = g_ptr_array_new ();
  guint i;

  for (i = 0; i < deployments->len; i++)
    {
      OtDeployment *deployment = deployments->pdata[i];
      
      if (strcmp (ot_deployment_get_osname (deployment), osname) != 0)
        continue;
      if (strcmp (ot_deployment_get_bootcsum (deployment), bootcsum) != 0)
        continue;
      
      g_ptr_array_add (ret, deployment);
    }
  g_ptr_array_sort (ret, sort_by_bootserial);

  return ret;
}

static void
compute_new_deployment_list (int           current_bootversion,
                             GPtrArray    *current_deployments,
                             const char   *osname,
                             OtDeployment *booted_deployment,
                             OtDeployment *merge_deployment,
                             gboolean      retain,
                             const char   *revision,
                             const char   *bootcsum,
                             GPtrArray   **out_new_deployments,
                             int          *out_new_bootversion)
{
  guint i;
  int new_index;
  guint new_deployserial = 0;
  int new_bootserial = 0;
  gs_unref_object OtDeployment *new_deployment = NULL;
  gs_unref_ptrarray GPtrArray *matching_deployments_by_bootserial = NULL;
  OtDeployment *deployment_to_delete = NULL;
  gs_unref_ptrarray GPtrArray *ret_new_deployments = NULL;
  gboolean requires_new_bootversion;

  if (osname == NULL)
    osname = ot_deployment_get_osname (booted_deployment);

  /* First, compute the serial for this deployment; we look
   * for other ones in this os with the same checksum.
   */
  for (i = 0; i < current_deployments->len; i++)
    {
      OtDeployment *deployment = current_deployments->pdata[i];
      
      if (strcmp (ot_deployment_get_osname (deployment), osname) != 0)
        continue;
      if (strcmp (ot_deployment_get_csum (deployment), revision) != 0)
        continue;

      new_deployserial = MAX(new_deployserial, ot_deployment_get_deployserial (deployment)+1);
    }

  /* We retain by default (well, hardcoded now) one previous
   * deployment for this OS, plus the booted deployment.  Usually, we
   * have one previous, one into which we're booted, and we're
   * deploying a new one.  So the old previous will get swapped out,
   * and booted becomes previous.
   *
   * But if the user then upgrades again, we will end up pruning the
   * front of the deployment list.  We never delete the running
   * deployment.
   */
  if (!retain)
    {
      for (i = 0; i < current_deployments->len; i++)
        {
          OtDeployment *deployment = current_deployments->pdata[i];
      
          if (strcmp (ot_deployment_get_osname (deployment), osname) != 0)
            continue;

          // Keep both the booted and merge deployments
          if (ot_deployment_equal (deployment, booted_deployment) || 
              ot_deployment_equal (deployment, merge_deployment))
            continue;

          deployment_to_delete = deployment;
        }
    }

  /* We need to update the bootloader only if the deployment we're
   * removing uses a different kernel.
   */
  requires_new_bootversion =
    (deployment_to_delete == NULL) ||
    (strcmp (ot_deployment_get_bootcsum (deployment_to_delete), bootcsum) != 0);

  ret_new_deployments = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  new_deployment = ot_deployment_new (0, osname, revision, new_deployserial,
                                      bootcsum, new_bootserial);
  g_ptr_array_add (ret_new_deployments, g_object_ref (new_deployment));
  new_index = 1;
  for (i = 0; i < current_deployments->len; i++)
    {
      OtDeployment *orig_deployment = current_deployments->pdata[i];
      gs_unref_object OtDeployment *deployment_clone = NULL;

      if (orig_deployment == deployment_to_delete)
        continue;

      deployment_clone = ot_deployment_clone (orig_deployment);
      ot_deployment_set_index (deployment_clone, new_index);
      new_index++;
      g_ptr_array_add (ret_new_deployments, g_object_ref (deployment_clone));
    }

  /* Just renumber the deployments for the OS we're adding; we don't
   * handle anything else at the moment.
   */
  matching_deployments_by_bootserial = filter_deployments_by_bootcsum (ret_new_deployments,
                                                                       osname, bootcsum);
  for (i = 0; i < matching_deployments_by_bootserial->len; i++)
    {
      OtDeployment *deployment = matching_deployments_by_bootserial->pdata[i];
      ot_deployment_set_bootserial (deployment, i);
    }

  *out_new_deployments = ret_new_deployments;
  ret_new_deployments = NULL;
  g_assert (current_bootversion == 0 || current_bootversion == 1);
  if (requires_new_bootversion)
    *out_new_bootversion = (current_bootversion == 0) ? 1 : 0;
  else
    *out_new_bootversion = current_bootversion;
}

static GHashTable *
object_array_to_set (GPtrArray   *objlist,
                     GHashFunc    hashfunc,
                     GEqualFunc   equalfunc)
{
  GHashTable *ret = g_hash_table_new_full (hashfunc, equalfunc, g_object_unref, NULL);
  guint i;

  for (i = 0; i < objlist->len; i++)
    {
      GObject *obj = g_object_ref (objlist->pdata[i]);
      g_hash_table_insert (ret, obj, obj);
    }
  
  return ret;
}

static GHashTable *
object_set_subtract (GHashTable *a, GHashTable *b)
{
  GHashTable *ret = g_hash_table_new_full (NULL, NULL, g_object_unref, NULL);
  GHashTableIter hashiter;
  gpointer hashkey, hashvalue;
  
  g_hash_table_iter_init (&hashiter, a);
  while (g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue))
    {
      if (!g_hash_table_contains (b, hashkey))
        {
          GObject *o = g_object_ref (hashkey);
          g_hash_table_insert (ret, o, o);
        }
    }

  return ret;
}

static void
print_deployment_set (gboolean    for_removal,
                      GHashTable *set)
{
  GHashTableIter hashiter;
  gpointer hashkey, hashvalue;

  if (g_hash_table_size (set) == 0)
    return;

  g_print ("%s\n", for_removal ? "removed:" : "added: ");

  g_hash_table_iter_init (&hashiter, set);
  while (g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue))
    {
      OtDeployment *deployment = hashkey;

      g_print ("  %c %s %s.%d",
               for_removal ? '-' : '+', ot_deployment_get_osname (deployment),
               ot_deployment_get_csum (deployment),
               ot_deployment_get_deployserial (deployment));

      if (!for_removal)
        g_print (" index=%d", ot_deployment_get_index (deployment));
      g_print ("\n");
    }
}

static void
print_deployment_diff (GPtrArray   *current_deployments,
                       GPtrArray   *new_deployments)
{
  gs_unref_hashtable GHashTable *curset = object_array_to_set (current_deployments, ot_deployment_hash, ot_deployment_equal);
  gs_unref_hashtable GHashTable *newset = object_array_to_set (new_deployments, ot_deployment_hash, ot_deployment_equal);
  gs_unref_hashtable GHashTable *removed = NULL;
  gs_unref_hashtable GHashTable *added = NULL;

  removed = object_set_subtract (curset, newset);
  added = object_set_subtract (newset, curset);

  print_deployment_set (TRUE, removed);
  print_deployment_set (FALSE, added);
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
swap_bootlinks (GFile        *sysroot,
                int           current_bootversion,
                GPtrArray    *new_deployments,
                GCancellable *cancellable,
                GError      **error)
{
  gboolean ret = FALSE;
  guint i;
  int old_subbootversion, new_subbootversion;
  gs_unref_object GFile *ostree_dir = g_file_get_child (sysroot, "ostree");
  gs_free char *ostree_bootdir_name = g_strdup_printf ("boot.%d", current_bootversion);
  gs_unref_object GFile *ostree_bootdir = g_file_resolve_relative_path (ostree_dir, ostree_bootdir_name);
  gs_free char *ostree_subbootdir_name = NULL;
  gs_unref_object GFile *ostree_subbootdir = NULL;
  gs_unref_ptrarray GPtrArray *deployments_to_swap = NULL;

  if (!ot_admin_read_current_subbootversion (sysroot, current_bootversion,
                                             &old_subbootversion,
                                             cancellable, error))
    goto out;

  new_subbootversion = old_subbootversion == 0 ? 1 : 0;

  ostree_subbootdir_name = g_strdup_printf ("boot.%d.%d", current_bootversion, new_subbootversion);
  ostree_subbootdir = g_file_resolve_relative_path (ostree_dir, ostree_subbootdir_name);

  if (!gs_file_ensure_directory (ostree_subbootdir, TRUE, cancellable, error))
    goto out;

  for (i = 0; i < new_deployments->len; i++)
    {
      OtDeployment *deployment = new_deployments->pdata[i];
      gs_free char *bootlink_pathname = g_strdup_printf ("%s/%s/%d",
                                                         ot_deployment_get_osname (deployment),
                                                         ot_deployment_get_bootcsum (deployment),
                                                         ot_deployment_get_bootserial (deployment));
      gs_free char *bootlink_target = g_strdup_printf ("../../../deploy/%s/deploy/%s.%d",
                                                       ot_deployment_get_osname (deployment),
                                                       ot_deployment_get_csum (deployment),
                                                       ot_deployment_get_deployserial (deployment));
      gs_unref_object GFile *linkname = g_file_get_child (ostree_subbootdir, bootlink_pathname);
      gs_unref_object GFile *linkname_parent = g_file_get_parent (linkname);

      if (!gs_file_ensure_directory (linkname_parent, TRUE, cancellable, error))
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
install_deployment_kernel (GFile          *sysroot,
                           int             new_bootversion,
                           OtDeployment   *deployment,
                           guint           n_deployments,
                           GCancellable   *cancellable,
                           GError        **error)

{
  gboolean ret = FALSE;
  const char *osname = ot_deployment_get_osname (deployment);
  const char *bootcsum = ot_deployment_get_bootcsum (deployment);
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
  __attribute__((cleanup(ot_ordered_hash_cleanup))) OtOrderedHash *ohash = NULL;
  const char *val;
  OtConfigParser *bootconfig;
  gsize len;

  bootconfig = ot_deployment_get_bootconfig (deployment);
  deployment_dir = ot_admin_get_deployment_directory (sysroot, deployment);

  if (!get_kernel_from_tree (deployment_dir, &tree_kernel_path, &tree_initramfs_path,
                             cancellable, error))
    goto out;

  bootdir = g_file_get_child (sysroot, "boot");
  bootcsumdir = ot_gfile_resolve_path_printf (bootdir, "ostree/%s-%s",
                                              osname,
                                              bootcsum);
  bootconfpath = ot_gfile_resolve_path_printf (bootdir, "loader.%d/entries/ostree-%s-%s-%d.conf",
                                               new_bootversion, osname, 
                                               ot_deployment_get_csum (deployment),
                                               ot_deployment_get_bootserial (deployment));

  if (!gs_file_ensure_directory (bootcsumdir, TRUE, cancellable, error))
    goto out;
  bootconfpath_parent = g_file_get_parent (bootconfpath);
  if (!gs_file_ensure_directory (bootconfpath_parent, TRUE, cancellable, error))
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
  
  title_key = g_strdup_printf ("ostree:%s:%d %s", ot_deployment_get_osname (deployment),
                               ot_deployment_get_index (deployment),
                               val);
  ot_config_parser_set (bootconfig, "title", title_key);

  version_key = g_strdup_printf ("%d", n_deployments - ot_deployment_get_index (deployment));
  ot_config_parser_set (bootconfig, "version", version_key);

  linux_relpath = g_file_get_relative_path (bootdir, dest_kernel_path);
  linux_key = g_strconcat ("/", linux_relpath, NULL);
  ot_config_parser_set (bootconfig, "linux", linux_key);

  if (dest_initramfs_path)
    {
      initramfs_relpath = g_file_get_relative_path (bootdir, dest_initramfs_path);
      initrd_key = g_strconcat ("/", initramfs_relpath, NULL);
      ot_config_parser_set (bootconfig, "initrd", initrd_key);
    }

  val = ot_config_parser_get (bootconfig, "options");
  ostree_kernel_arg = g_strdup_printf ("/ostree/boot.%d/%s/%s/%d",
                                       new_bootversion, osname, bootcsum,
                                       ot_deployment_get_bootserial (deployment));
  ohash = ot_admin_parse_kernel_args (val);
  ot_ordered_hash_replace_key (ohash, "ostree", ostree_kernel_arg);
  options_key = ot_admin_kernel_arg_string_serialize (ohash);
  ot_config_parser_set (bootconfig, "options", options_key);
  
  if (!ot_config_parser_write (ot_deployment_get_bootconfig (deployment), bootconfpath,
                               cancellable, error))
      goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
swap_bootloader (GFile          *sysroot,
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

  boot_loader_link = g_file_resolve_relative_path (sysroot, "boot/loader");
  new_target = g_strdup_printf ("loader.%d", new_bootversion);

  if (!ot_gfile_atomic_symlink_swap (boot_loader_link, new_target,
                                     cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
ot_admin_write_deployments (GFile             *sysroot,
                            int                current_bootversion,
                            int                new_bootversion,
                            GPtrArray         *new_deployments,
                            GCancellable      *cancellable,
                            GError           **error)
{
  gboolean ret = FALSE;
  guint i;
  gs_unref_object OtBootloader *bootloader = ot_admin_query_bootloader (sysroot);

  if (bootloader)
    g_print ("Detected bootloader: %s\n", ot_bootloader_get_name (bootloader));
  else
    g_print ("Detected bootloader: (unknown)\n");

  if (current_bootversion == new_bootversion)
    {
      if (!full_system_sync (cancellable, error))
        {
          g_prefix_error (error, "Full sync: ");
          goto out;
        }

      if (!swap_bootlinks (sysroot, current_bootversion,
                           new_deployments,
                           cancellable, error))
        {
          g_prefix_error (error, "Swapping current bootlinks: ");
          goto out;
        }
    }
  else
    {
      for (i = 0; i < new_deployments->len; i++)
        {
          OtDeployment *deployment = new_deployments->pdata[i];
          if (!install_deployment_kernel (sysroot, new_bootversion,
                                          deployment, new_deployments->len,
                                          cancellable, error))
            {
              g_prefix_error (error, "Installing kernel: ");
              goto out;
            }
        }

      /* Swap bootlinks for *new* version */
      if (!swap_bootlinks (sysroot, new_bootversion, new_deployments,
                           cancellable, error))
        {
          g_prefix_error (error, "Generating new bootlinks: ");
          goto out;
        }

      if (!full_system_sync (cancellable, error))
        {
          g_prefix_error (error, "Full sync: ");
          goto out;
        }

      if (bootloader && !ot_bootloader_write_config (bootloader, new_bootversion,
                                                     cancellable, error))
          {
            g_prefix_error (error, "Bootloader write config: ");
            goto out;
          }

      if (!swap_bootloader (sysroot, current_bootversion, new_bootversion,
                            cancellable, error))
        {
          g_prefix_error (error, "Final bootloader swap: ");
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}
                            
gboolean
ot_admin_deploy (GFile             *sysroot,
                 int                current_bootversion,
                 GPtrArray         *current_deployments,
                 const char        *osname,
                 const char        *revision,
                 GKeyFile          *origin,
                 char             **add_kernel_argv,
                 gboolean           retain,
                 OtDeployment      *booted_deployment,
                 OtDeployment      *provided_merge_deployment,
                 OtDeployment     **out_new_deployment,
                 int               *out_new_bootversion,
                 GPtrArray        **out_new_deployments,
                 GCancellable      *cancellable,
                 GError           **error)
{
  gboolean ret = FALSE;
  OtDeployment *new_deployment;
  gs_unref_object OtDeployment *merge_deployment = NULL;
  gs_unref_object GFile *rootfs = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_object GFile *commit_root = NULL;
  gs_unref_object GFile *tree_kernel_path = NULL;
  gs_unref_object GFile *tree_initramfs_path = NULL;
  gs_unref_object GFile *new_deployment_path = NULL;
  gs_unref_object GFile *deploy_path = NULL;
  gs_unref_object GFile *osdir = NULL;
  gs_free char *new_bootcsum = NULL;
  gs_unref_object GFile *source_etc_path = NULL;
  gs_unref_object GFile *source_etc_pristine_path = NULL;
  gs_unref_object OtConfigParser *bootconfig = NULL;
  gs_free char *source_etc_kernel_args = NULL;
  gs_unref_ptrarray GPtrArray *new_deployments = NULL;
  int new_bootversion;

  if (!ot_admin_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  /* Here we perform cleanup of any leftover data from previous
   * partial failures.  This avoids having to call gs_shutil_rm_rf()
   * at random points throughout the process.
   *
   * TODO: Add /ostree/transaction file, and only do this cleanup if
   * we find it.
   */
  if (!ot_admin_cleanup (sysroot, cancellable, error))
    {
      g_prefix_error (error, "Performing initial cleanup: ");
      goto out;
    }

  if (!ostree_repo_read_commit (repo, revision, &commit_root, cancellable, error))
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

  /* If we're booted into the OS into which we're deploying, then
   * merge the currently *booted* configuration, rather than the most
   * recently deployed.
   */
  if (provided_merge_deployment != NULL)
    merge_deployment = g_object_ref (provided_merge_deployment);
  else
    merge_deployment = ot_admin_get_merge_deployment (current_deployments, osname,
                                                      booted_deployment); 

  compute_new_deployment_list (current_bootversion,
                               current_deployments, osname,
                               booted_deployment, merge_deployment,
                               retain,
                               revision, new_bootcsum,
                               &new_deployments,
                               &new_bootversion);
  new_deployment = g_object_ref (new_deployments->pdata[0]);
  ot_deployment_set_origin (new_deployment, origin);

  print_deployment_diff (current_deployments, new_deployments);

  /* Check out the userspace tree onto the filesystem */
  if (!checkout_deployment_tree (sysroot, repo, new_deployment, &new_deployment_path,
                                 cancellable, error))
    {
      g_prefix_error (error, "Checking out tree: ");
      goto out;
    }

  if (!write_origin_file (sysroot, new_deployment, cancellable, error))
    {
      g_prefix_error (error, "Writing out origin file: ");
      goto out;
    }

  /* Create an empty boot configuration; we will merge things into
   * it as we go.
   */
  bootconfig = ot_config_parser_new (" ");
  ot_deployment_set_bootconfig (new_deployment, bootconfig);

  if (!merge_configuration (sysroot, merge_deployment, new_deployment,
                            new_deployment_path,
                            cancellable, error))
    {
      g_prefix_error (error, "During /etc merge: ");
      goto out;
    }

  /* We have inherited kernel arguments from the previous deployment;
   * now, override/extend that with arguments provided by the command
   * line.
   * 
   * After this, install_deployment_kernel() will set the other boot
   * options and write it out to disk.
   */
  if (add_kernel_argv)
    {
      char **strviter;
      __attribute__((cleanup(ot_ordered_hash_cleanup))) OtOrderedHash *ohash = NULL;
      gs_free char *new_options = NULL;

      ohash = ot_admin_parse_kernel_args (ot_config_parser_get (bootconfig, "options"));

      for (strviter = add_kernel_argv; *strviter; strviter++)
        {
          char *karg = g_strdup (*strviter);
          const char *val = ot_admin_util_split_keyeq (karg);
        
          ot_ordered_hash_replace_key_take (ohash, karg, val);
        }

      new_options = ot_admin_kernel_arg_string_serialize (ohash);
      ot_config_parser_set (bootconfig, "options", new_options);
    }

  if (!ot_admin_write_deployments (sysroot, current_bootversion, new_bootversion,
                                   new_deployments, cancellable, error))
    goto out;

  g_print ("Transaction complete, performing cleanup\n");

  /* TEMPORARY HACK: Add a "current" symbolic link that's easy to
   * follow inside the gnome-ostree build scripts.  This isn't atomic,
   * but that doesn't matter because it's only used by deployments
   * done from the host.
   */
  {
    gs_unref_object GFile *osdir = ot_gfile_resolve_path_printf (sysroot, "ostree/deploy/%s", ot_deployment_get_osname (new_deployment));
    gs_unref_object GFile *os_current_path = g_file_get_child (osdir, "current");
    gs_free char *target = g_file_get_relative_path (osdir, new_deployment_path);
    g_assert (target != NULL);
    if (!ot_gfile_atomic_symlink_swap (os_current_path, target,
                                       cancellable, error))
      goto out;
  }

  /* And finally, cleanup of any leftover data.
   */
  if (!ot_admin_cleanup (sysroot, cancellable, error))
    {
      g_prefix_error (error, "Performing final cleanup: ");
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_new_deployment, &new_deployment);
  *out_new_bootversion = new_bootversion;
  ot_transfer_out_value (out_new_deployments, &new_deployments)
 out:
  return ret;
}

