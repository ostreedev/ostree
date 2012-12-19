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
