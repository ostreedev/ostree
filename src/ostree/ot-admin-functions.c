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
#include "otutil.h"
#include "ostree-core.h"

gboolean
ot_admin_ensure_initialized (GFile         *ostree_dir,
                             GCancellable  *cancellable,
                             GError       **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *dir = NULL;

  g_clear_object (&dir);
  dir = g_file_get_child (ostree_dir, "repo");
  if (!gs_file_ensure_directory (dir, TRUE, cancellable, error))
    goto out;

  g_clear_object (&dir);
  dir = g_file_get_child (ostree_dir, "deploy");
  if (!gs_file_ensure_directory (dir, TRUE, cancellable, error))
    goto out;

  g_clear_object (&dir);
  dir = ot_gfile_get_child_build_path (ostree_dir, "repo", "objects", NULL);
  if (!g_file_query_exists (dir, NULL))
    {
      ot_lfree char *opt_repo_arg = g_strdup_printf ("--repo=%s/repo",
                                                      gs_file_get_path_cached (ostree_dir));

      if (!gs_subprocess_simple_run_sync (NULL, GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
                                          cancellable, error,
                                          "ostree", opt_repo_arg, "init", NULL))
        {
          g_prefix_error (error, "Failed to initialize repository: ");
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
query_file_info_allow_noent (GFile         *path,
                             GFileInfo    **out_info,
                             GCancellable  *cancellable,
                             GError       **error)
{
  gboolean ret = FALSE;
  ot_lobj GFileInfo *ret_file_info = NULL;
  GError *temp_error = NULL;

  ret_file_info = g_file_query_info (path, OSTREE_GIO_FAST_QUERYINFO,
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                     cancellable, &temp_error);
  if (!ret_file_info)
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_info, &ret_file_info);
 out:
  return ret;
}

static gboolean
query_symlink_target_allow_noent (GFile         *path,
                                  GFile        **out_target,
                                  GCancellable  *cancellable,
                                  GError       **error)
{
  gboolean ret = FALSE;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GFile *ret_target = NULL;
  ot_lobj GFile *path_parent = NULL;

  if (!query_file_info_allow_noent (path, &file_info,
                                    cancellable, error))
    goto out;

  path_parent = g_file_get_parent (path);

  if (file_info != NULL)
    {
      const char *target;
      
      if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_SYMBOLIC_LINK)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Not a symbolic link");
          goto out;
        }
      target = g_file_info_get_symlink_target (file_info);
      g_assert (target);
      ret_target = g_file_resolve_relative_path (path_parent, target);
    }

  ret = TRUE;
  ot_transfer_out_value (out_target, &ret_target);
 out:
  return ret;
}

/**
 * ot_admin_get_current_deployment:
 * 
 * Returns in @out_deployment the full file path of the current
 * deployment that the /ostree/current symbolic link points to, or
 * %NULL if none.
 */
gboolean
ot_admin_get_current_deployment (GFile           *ostree_dir,
                                 const char      *osname,
                                 GFile          **out_deployment,
                                 GCancellable    *cancellable,
                                 GError         **error)
{
  ot_lobj GFile *current_path = NULL;

  current_path = ot_gfile_get_child_build_path (ostree_dir, "deploy", osname,
                                                "current", NULL);

  return query_symlink_target_allow_noent (current_path, out_deployment,
                                           cancellable, error);
}

/**
 * ot_admin_get_previous_deployment:
 * 
 * Returns in @out_deployment the full file path of the current
 * deployment that the /ostree/previous symbolic link points to, or
 * %NULL if none.
 */
gboolean
ot_admin_get_previous_deployment (GFile           *ostree_dir,
                                  const char      *osname,
                                  GFile          **out_deployment,
                                  GCancellable    *cancellable,
                                  GError         **error)
{
  ot_lobj GFile *previous_path = NULL;

  previous_path = ot_gfile_get_child_build_path (ostree_dir, "deploy", osname,
                                                 "previous", NULL);

  return query_symlink_target_allow_noent (previous_path, out_deployment,
                                           cancellable, error);
}

/*
static gboolean
ot_admin_list_osnames (GFile               *ostree_dir,
                       GPtrArray          **out_osnames,
                       GCancellable        *cancellable,
                       GError             **error)
{
  gboolean ret = FALSE;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GFile *deploy_dir = NULL;
  ot_lptrarray GPtrArray *ret_osnames = NULL;
  GError *temp_error = NULL;

  deploy_dir = g_file_get_child (ostree_dir, "deploy");

  dir_enum = g_file_enumerate_children (deploy_dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, error);
  if (!dir_enum)
    goto out;

  while ((file_info = g_file_enumerator_next_file (dir_enum, NULL, error)) != NULL)
    {
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          char *name = g_strdup (g_file_info_get_name (file_info));
          g_ptr_array_add (ret_osnames, name);
        }
      g_clear_object (&file_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  
  ret = TRUE;
  ot_transfer_out_value (out_osnames, &ret_osnames);
 out:
  return ret;
}
*/

static gboolean
list_deployments_internal (GFile               *from_dir,
                           GPtrArray           *inout_deployments,
                           GCancellable        *cancellable,
                           GError             **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFileInfo *file_info = NULL;

  dir_enum = g_file_enumerate_children (from_dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, error);
  if (!dir_enum)
    goto out;

  while ((file_info = g_file_enumerator_next_file (dir_enum, cancellable, error)) != NULL)
    {
      const char *name;
      ot_lobj GFile *child = NULL;
      ot_lobj GFile *possible_etc = NULL;
      ot_lobj GFile *possible_usr = NULL;

      name = g_file_info_get_name (file_info);

      if (g_str_has_suffix (name, "-etc"))
        goto next;
      if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
        goto next;

      child = g_file_get_child (from_dir, name);

      possible_etc = ot_gfile_get_child_strconcat (from_dir, name, "-etc", NULL);
      /* Bit of a hack... */
      possible_usr = g_file_get_child (child, "usr");

      if (g_file_query_exists (possible_etc, cancellable))
        g_ptr_array_add (inout_deployments, g_file_get_child (from_dir, name));
      else if (g_file_query_exists (possible_usr, cancellable))
        goto next;
      else
        {
          if (!list_deployments_internal (child, inout_deployments,
                                          cancellable, error))
            goto out;
        }

    next:
      g_clear_object (&file_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ot_admin_list_deployments (GFile               *ostree_dir,
                           const char          *osname,
                           GPtrArray          **out_deployments,
                           GCancellable        *cancellable,
                           GError             **error)
{
  gboolean ret = FALSE;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GFile *osdir = NULL;
  ot_lptrarray GPtrArray *ret_deployments = NULL;

  osdir = ot_gfile_get_child_build_path (ostree_dir, "deploy", osname, NULL);
  ret_deployments = g_ptr_array_new_with_free_func (g_object_unref);

  if (!list_deployments_internal (osdir, ret_deployments, cancellable, error))
    goto out;
  
  ret = TRUE;
  ot_transfer_out_value (out_deployments, &ret_deployments);
 out:
  return ret;
}

static gboolean
ot_admin_get_booted_os (char   **out_osname,
                        char   **out_tree,
                        GCancellable *cancellable,
                        GError **error)
{
  gboolean ret = FALSE;
  gs_free char *ret_osname = NULL;
  gs_free char *ret_tree = NULL;
  gs_free char *cmdline_contents = NULL;
  const char *iter;
  gsize len;

  if (!g_file_get_contents ("/proc/cmdline", &cmdline_contents, &len,
                            error))
    goto out;

  iter = cmdline_contents;
  do
    {
      const char *next = strchr (iter, ' ');
      if (next)
	next += 1;
      if (g_str_has_prefix (iter, "ostree="))
        {
          const char *slash = strchr (iter, '/');
          if (slash)
            {
              const char *start = iter + strlen ("ostree=");
              ret_osname = g_strndup (start, slash - start);
              if (next)
                ret_tree = g_strndup (slash + 1, next - slash - 1);
              else
                ret_tree = g_strdup (slash + 1);
              break;
            }
        }
      iter = next;
    }
  while (iter != NULL);

  ret = TRUE;
 out:
  ot_transfer_out_value (out_osname, &ret_osname);
  ot_transfer_out_value (out_tree, &ret_tree);
  return ret;
}

gboolean
ot_admin_get_active_deployment (GFile           *ostree_dir,
                                char           **out_osname,
                                GFile          **out_deployment,
                                GCancellable    *cancellable,
                                GError         **error)
{
  gboolean ret = FALSE;
  ot_lptrarray GPtrArray *osnames = NULL;
  ot_lptrarray GPtrArray *deployments = NULL;
  gs_free char *ret_osname = NULL;
  gs_unref_object GFile *ret_deployment = NULL;

  if (!ot_admin_get_booted_os (&ret_osname, NULL, cancellable, error))
    goto out;

  if (ret_osname != NULL)
    {
      gs_unref_object GFile *rootfs_path = NULL;
      gs_unref_object GFileInfo *rootfs_info = NULL;
      guint32 root_dev;
      guint64 root_inode;
      guint i;

      rootfs_path = g_file_new_for_path ("/");
      rootfs_info = g_file_query_info (rootfs_path, OSTREE_GIO_FAST_QUERYINFO,
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       cancellable, error);
      if (!rootfs_info)
        goto out;

      root_dev = g_file_info_get_attribute_uint32 (rootfs_info, "unix::device");
      root_inode = g_file_info_get_attribute_uint64 (rootfs_info, "unix::inode");

      if (!ot_admin_list_deployments (ostree_dir, ret_osname, &deployments,
                                      cancellable, error))
        goto out;
      
      for (i = 0; i < deployments->len; i++)
        {
          GFile *deployment = deployments->pdata[i];
          gs_unref_object GFileInfo *deployment_info = NULL;
          guint32 deploy_dev;
          guint64 deploy_inode;
          
          deployment_info = g_file_query_info (deployment, OSTREE_GIO_FAST_QUERYINFO,
                                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                               cancellable, error);
          if (!deployment_info)
            goto out;

          deploy_dev = g_file_info_get_attribute_uint32 (deployment_info, "unix::device");
          deploy_inode = g_file_info_get_attribute_uint64 (deployment_info, "unix::inode");

          if (root_dev == deploy_dev && root_inode == deploy_inode)
            {
              ret_deployment = g_object_ref (deployment);
              break;
            }
        }
      
      g_assert (ret_deployment != NULL);
    }

  ret = TRUE;
  ot_transfer_out_value (out_osname, &ret_osname);
  ot_transfer_out_value (out_deployment, &ret_deployment);
 out:
  return ret;
}

gboolean
ot_admin_get_default_ostree_dir (GFile        **out_ostree_dir,
                                 GCancellable  *cancellable,
                                 GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *possible_ostree_dir = NULL;
  gs_unref_object GFile *ret_ostree_dir = NULL;

  if (ret_ostree_dir == NULL)
    {
      g_clear_object (&possible_ostree_dir);
      possible_ostree_dir = g_file_new_for_path ("/sysroot/ostree");
      if (g_file_query_exists (possible_ostree_dir, NULL))
        ret_ostree_dir = g_object_ref (possible_ostree_dir);
    }
  if (ret_ostree_dir == NULL)
    {
      g_clear_object (&possible_ostree_dir);
      possible_ostree_dir = g_file_new_for_path ("/ostree");
      if (g_file_query_exists (possible_ostree_dir, NULL))
        ret_ostree_dir = g_object_ref (possible_ostree_dir);
    }

  ret = TRUE;
  ot_transfer_out_value (out_ostree_dir, &ret_ostree_dir);
  return ret;
}
