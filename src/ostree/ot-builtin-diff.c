/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

#include "ot-builtins.h"
#include "ostree.h"

#include <glib/gi18n.h>

static GOptionEntry options[] = {
  { NULL }
};

static gboolean
parse_file_or_commit (OstreeRepo  *repo,
                      const char  *arg,
                      GFile      **out_file,
                      GCancellable *cancellable,
                      GError     **error)
{
  gboolean ret = FALSE;
  ot_lfree GFile *ret_file = NULL;

  if (g_str_has_prefix (arg, "/")
      || g_str_has_prefix (arg, "./")
      )
    {
      ret_file = ot_gfile_new_for_path (arg);
    }
  else
    {
      if (!ostree_repo_read_commit (repo, arg, &ret_file, cancellable, error))
        goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_file, &ret_file);
 out:
  return ret;
}


static gboolean
get_file_checksum (GFile  *f,
                   GFileInfo *f_info,
                   char  **out_checksum,
                   GCancellable *cancellable,
                   GError   **error)
{
  gboolean ret = FALSE;
  ot_lfree char *ret_checksum = NULL;
  ot_lfree guchar *csum = NULL;

  if (OSTREE_IS_REPO_FILE (f))
    {
      ret_checksum = g_strdup (ostree_repo_file_get_checksum ((OstreeRepoFile*)f));
    }
  else
    {
      if (!ostree_checksum_file (f, OSTREE_OBJECT_TYPE_RAW_FILE,
                                 &csum, cancellable, error))
        goto out;
      ret_checksum = ostree_checksum_from_bytes (csum);
    }

  ret = TRUE;
  ot_transfer_out_value(out_checksum, &ret_checksum);
 out:
  return ret;
}

typedef struct {
  volatile gint refcount;

  GFile *src;
  GFile *target;

  GFileInfo *src_info;
  GFileInfo *target_info;

  char *src_checksum;
  char *target_checksum;
} DiffItem;

DiffItem *diff_item_ref (DiffItem *diffitem);
void diff_item_unref (DiffItem *diffitem);


DiffItem *
diff_item_ref (DiffItem *diffitem)
{
  g_atomic_int_inc (&diffitem->refcount);
  return diffitem;
}

void
diff_item_unref (DiffItem *diffitem)
{
  if (!g_atomic_int_dec_and_test (&diffitem->refcount))
    return;

  g_clear_object (&diffitem->src);
  g_clear_object (&diffitem->target);
  g_clear_object (&diffitem->src_info);
  g_clear_object (&diffitem->target_info);
  g_free (diffitem->src_checksum);
  g_free (diffitem->target_checksum);
  g_free (diffitem);
}

static DiffItem *
diff_item_new (GFile          *a,
               GFileInfo      *a_info,
               GFile          *b,
               GFileInfo      *b_info,
               char           *checksum_a,
               char           *checksum_b)
{
  DiffItem *ret = g_new0 (DiffItem, 1);
  ret->refcount = 1;
  ret->src = a ? g_object_ref (a) : NULL;
  ret->src_info = a_info ? g_object_ref (a_info) : NULL;
  ret->target = b ? g_object_ref (b) : NULL;
  ret->target_info = b_info ? g_object_ref (b_info) : b_info;
  ret->src_checksum = g_strdup (checksum_a);
  ret->target_checksum = g_strdup (checksum_b);
  return ret;
}
               

static gboolean
diff_files (GFile          *a,
            GFileInfo      *a_info,
            GFile          *b,
            GFileInfo      *b_info,
            DiffItem **out_item,
            GCancellable   *cancellable,
            GError        **error)
{
  gboolean ret = FALSE;
  ot_lfree char *checksum_a = NULL;
  ot_lfree char *checksum_b = NULL;
  DiffItem *ret_item = NULL;

  if (!get_file_checksum (a, a_info, &checksum_a, cancellable, error))
    goto out;
  if (!get_file_checksum (b, b_info, &checksum_b, cancellable, error))
    goto out;

  if (strcmp (checksum_a, checksum_b) != 0)
    {
      ret_item = diff_item_new (a, a_info, b, b_info,
                                checksum_a, checksum_b);
    }

  ret = TRUE;
  ot_transfer_out_value(out_item, &ret_item);
 out:
  if (ret_item)
    diff_item_unref (ret_item);
  return ret;
}

static gboolean
diff_add_dir_recurse (GFile          *d,
                      GPtrArray      *added,
                      GCancellable   *cancellable,
                      GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFile *child = NULL;
  ot_lobj GFileInfo *child_info = NULL;

  dir_enum = g_file_enumerate_children (d, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;

      name = g_file_info_get_name (child_info);

      g_clear_object (&child);
      child = g_file_get_child (d, name);

      g_ptr_array_add (added, g_object_ref (child));

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!diff_add_dir_recurse (child, added, cancellable, error))
            goto out;
        }
      
      g_clear_object (&child_info);
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

static gboolean
diff_dirs (GFile          *a,
           GFile          *b,
           GPtrArray      *modified,
           GPtrArray      *removed,
           GPtrArray      *added,
           GCancellable   *cancellable,
           GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFile *child_a = NULL;
  ot_lobj GFile *child_b = NULL;
  ot_lobj GFileInfo *child_a_info = NULL;
  ot_lobj GFileInfo *child_b_info = NULL;

  dir_enum = g_file_enumerate_children (a, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_a_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;
      GFileType child_a_type;
      GFileType child_b_type;

      name = g_file_info_get_name (child_a_info);

      g_clear_object (&child_a);
      child_a = g_file_get_child (a, name);
      child_a_type = g_file_info_get_file_type (child_a_info);

      g_clear_object (&child_b);
      child_b = g_file_get_child (b, name);

      g_clear_object (&child_b_info);
      child_b_info = g_file_query_info (child_b, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        &temp_error);
      if (!child_b_info)
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&temp_error);
              g_ptr_array_add (removed, g_object_ref (child_a));
            }
          else
            {
              g_propagate_error (error, temp_error);
              goto out;
            }
        }
      else
        {
          child_b_type = g_file_info_get_file_type (child_b_info);
          if (child_a_type != child_b_type)
            {
              DiffItem *diff_item = diff_item_new (child_a, child_a_info,
                                                             child_b, child_b_info, NULL, NULL);
              
              g_ptr_array_add (modified, diff_item);
            }
          else
            {
              DiffItem *diff_item = NULL;

              if (!diff_files (child_a, child_a_info, child_b, child_b_info, &diff_item, cancellable, error))
                goto out;
              
              if (diff_item)
                g_ptr_array_add (modified, diff_item); /* Transfer ownership */

              if (child_a_type == G_FILE_TYPE_DIRECTORY)
                {
                  if (!diff_dirs (child_a, child_b, modified,
                                  removed, added, cancellable, error))
                    goto out;
                }
            }
        }
      
      g_clear_object (&child_a_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  g_clear_object (&dir_enum);
  dir_enum = g_file_enumerate_children (b, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_b_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;

      name = g_file_info_get_name (child_b_info);

      g_clear_object (&child_a);
      child_a = g_file_get_child (a, name);

      g_clear_object (&child_b);
      child_b = g_file_get_child (b, name);

      g_clear_object (&child_a_info);
      child_a_info = g_file_query_info (child_a, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        &temp_error);
      if (!child_a_info)
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&temp_error);
              g_ptr_array_add (added, g_object_ref (child_b));
              if (g_file_info_get_file_type (child_b_info) == G_FILE_TYPE_DIRECTORY)
                {
                  if (!diff_add_dir_recurse (child_b, added, cancellable, error))
                    goto out;
                }
            }
          else
            {
              g_propagate_error (error, temp_error);
              goto out;
            }
        }
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
ostree_builtin_diff (int argc, char **argv, GFile *repo_path, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  GCancellable *cancellable = NULL;
  int i;
  const char *src;
  const char *target;
  ot_lobj OstreeRepo *repo = NULL;
  ot_lfree char *src_prev = NULL;
  ot_lobj GFile *srcf = NULL;
  ot_lobj GFile *targetf = NULL;
  ot_lobj GFile *cwd = NULL;
  ot_lptrarray GPtrArray *modified = NULL;
  ot_lptrarray GPtrArray *removed = NULL;
  ot_lptrarray GPtrArray *added = NULL;

  context = g_option_context_new ("REV TARGETDIR - Compare directory TARGETDIR against revision REV");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (argc < 2)
    {
      gchar *help = g_option_context_get_help (context, TRUE, NULL);
      g_printerr ("%s\n", help);
      g_free (help);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "REV must be specified");
      goto out;
    }

  if (argc == 2)
    {
      src_prev = g_strconcat (argv[1], "^", NULL);
      src = src_prev;
      target = argv[1];
    }
  else
    {
      src = argv[1];
      target = argv[2];
    }

  cwd = ot_gfile_new_for_path (".");

  if (!parse_file_or_commit (repo, src, &srcf, cancellable, error))
    goto out;
  if (!parse_file_or_commit (repo, target, &targetf, cancellable, error))
    goto out;

  modified = g_ptr_array_new_with_free_func ((GDestroyNotify)diff_item_unref);
  removed = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  added = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  
  if (!diff_dirs (srcf, targetf, modified, removed, added, cancellable, error))
    goto out;

  for (i = 0; i < modified->len; i++)
    {
      DiffItem *diff = modified->pdata[i];
      g_print ("M    %s\n", ot_gfile_get_path_cached (diff->src));
    }
  for (i = 0; i < removed->len; i++)
    {
      g_print ("D    %s\n", ot_gfile_get_path_cached (removed->pdata[i]));
    }
  for (i = 0; i < added->len; i++)
    {
      GFile *added_f = added->pdata[i];
      if (g_file_is_native (added_f))
        {
          char *relpath = g_file_get_relative_path (cwd, added_f);
          g_assert (relpath != NULL);
          g_print ("A    /%s\n", relpath);
          g_free (relpath);
        }
      else
        g_print ("A    %s\n", ot_gfile_get_path_cached (added_f));
    }

  ret = TRUE;
 out:
  return ret;
}
