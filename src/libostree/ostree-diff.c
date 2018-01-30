/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "libglnx.h"
#include "ostree.h"
#include "ostree-repo-private.h"
#include "otutil.h"

/* See ostree-repo.c for a bit more info about these ABI checks */
#if __SIZEOF_POINTER__ == 8 && __SIZEOF_LONG__ == 8 && __SIZEOF_INT__ == 4
G_STATIC_ASSERT(sizeof(OstreeDiffDirsOptions) ==
                sizeof(int) * 2 +
                sizeof(gpointer) +
                sizeof(int) * (7+6) +
                sizeof(int) +  /* hole */
                sizeof(gpointer) * 7);
#endif

static gboolean
get_file_checksum (OstreeDiffFlags  flags,
                   GFile *f,
                   GFileInfo *f_info,
                   char  **out_checksum,
                   GCancellable *cancellable,
                   GError   **error)
{
  g_autofree char *ret_checksum = NULL;

  if (OSTREE_IS_REPO_FILE (f))
    {
      ret_checksum = g_strdup (ostree_repo_file_get_checksum ((OstreeRepoFile*)f));
    }
  else
    {
      g_autoptr(GVariant) xattrs = NULL;
      g_autoptr(GInputStream) in = NULL;

      if (!(flags & OSTREE_DIFF_FLAGS_IGNORE_XATTRS))
        {
          if (!glnx_dfd_name_get_all_xattrs (AT_FDCWD, gs_file_get_path_cached (f),
                                             &xattrs, cancellable, error))
            return FALSE;
        }

      if (g_file_info_get_file_type (f_info) == G_FILE_TYPE_REGULAR)
        {
          in = (GInputStream*)g_file_read (f, cancellable, error);
          if (!in)
            return FALSE;
        }

      g_autofree guchar *csum = NULL;
      if (!ostree_checksum_file_from_input (f_info, xattrs, in,
                                            OSTREE_OBJECT_TYPE_FILE,
                                            &csum, cancellable, error))
        return FALSE;
      ret_checksum = ostree_checksum_from_bytes (csum);
    }

  ot_transfer_out_value(out_checksum, &ret_checksum);
  return TRUE;
}

OstreeDiffItem *
ostree_diff_item_ref (OstreeDiffItem *diffitem)
{
  g_atomic_int_inc (&diffitem->refcount);
  return diffitem;
}

void
ostree_diff_item_unref (OstreeDiffItem *diffitem)
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

G_DEFINE_BOXED_TYPE(OstreeDiffItem, ostree_diff_item,
                    ostree_diff_item_ref,
                    ostree_diff_item_unref);

static OstreeDiffItem *
diff_item_new (GFile          *a,
               GFileInfo      *a_info,
               GFile          *b,
               GFileInfo      *b_info,
               char           *checksum_a,
               char           *checksum_b)
{
  OstreeDiffItem *ret = g_new0 (OstreeDiffItem, 1);
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
diff_files (OstreeDiffFlags  flags,
            GFile           *a,
            GFileInfo       *a_info,
            GFile           *b,
            GFileInfo       *b_info,
            OstreeDiffItem **out_item,
            GCancellable    *cancellable,
            GError         **error)
{
  g_autofree char *checksum_a = NULL;
  g_autofree char *checksum_b = NULL;
  if (!get_file_checksum (flags, a, a_info, &checksum_a, cancellable, error))
    return FALSE;
  if (!get_file_checksum (flags, b, b_info, &checksum_b, cancellable, error))
    return FALSE;

  g_autoptr(OstreeDiffItem) ret_item = NULL;
  if (strcmp (checksum_a, checksum_b) != 0)
    {
      ret_item = diff_item_new (a, a_info, b, b_info,
                                checksum_a, checksum_b);
    }

  ot_transfer_out_value(out_item, &ret_item);
  return TRUE;
}

static gboolean
diff_add_dir_recurse (GFile          *d,
                      GPtrArray      *added,
                      GCancellable   *cancellable,
                      GError        **error)
{
  g_autoptr(GFileEnumerator) dir_enum =
    g_file_enumerate_children (d, OSTREE_GIO_FAST_QUERYINFO,
                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                               cancellable,
                               error);
  if (!dir_enum)
    return FALSE;

  while (TRUE)
    {
      GFileInfo *child_info;
      const char *name;

      if (!g_file_enumerator_iterate (dir_enum, &child_info, NULL,
                                      cancellable, error))
        return FALSE;
      if (child_info == NULL)
        break;

      name = g_file_info_get_name (child_info);

      g_autoptr(GFile) child = g_file_get_child (d, name);
      g_ptr_array_add (added, g_object_ref (child));

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!diff_add_dir_recurse (child, added, cancellable, error))
            return FALSE;
        }
    }

  return TRUE;
}

/**
 * ostree_diff_dirs:
 * @flags: Flags
 * @a: First directory path, or %NULL
 * @b: First directory path
 * @modified: (element-type OstreeDiffItem): Modified files
 * @removed: (element-type Gio.File): Removed files
 * @added: (element-type Gio.File): Added files
 * @cancellable: Cancellable
 * @error: Error
 *
 * Compute the difference between directory @a and @b as 3 separate
 * sets of #OstreeDiffItem in @modified, @removed, and @added.
 */
gboolean
ostree_diff_dirs (OstreeDiffFlags flags,
                  GFile          *a,
                  GFile          *b,
                  GPtrArray      *modified,
                  GPtrArray      *removed,
                  GPtrArray      *added,
                  GCancellable   *cancellable,
                  GError        **error)
{
  return ostree_diff_dirs_with_options (flags, a, b, modified,
                                        removed, added, NULL,
                                        cancellable, error);
}

/**
 * ostree_diff_dirs_with_options:
 * @flags: Flags
 * @a: First directory path, or %NULL
 * @b: First directory path
 * @modified: (element-type OstreeDiffItem): Modified files
 * @removed: (element-type Gio.File): Removed files
 * @added: (element-type Gio.File): Added files
 * @cancellable: Cancellable
 * @options: (allow-none): Options
 * @error: Error
 *
 * Compute the difference between directory @a and @b as 3 separate
 * sets of #OstreeDiffItem in @modified, @removed, and @added.
 */
gboolean
ostree_diff_dirs_with_options (OstreeDiffFlags        flags,
                               GFile                 *a,
                               GFile                 *b,
                               GPtrArray             *modified,
                               GPtrArray             *removed,
                               GPtrArray             *added,
                               OstreeDiffDirsOptions *options,
                               GCancellable          *cancellable,
                               GError               **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFile) child_a = NULL;
  g_autoptr(GFile) child_b = NULL;
  g_autoptr(GFileInfo) child_a_info = NULL;
  g_autoptr(GFileInfo) child_b_info = NULL;
  OstreeDiffDirsOptions default_opts = OSTREE_DIFF_DIRS_OPTIONS_INIT;

  if (!options)
    options = &default_opts;

  /* If we're diffing versus a repo, and either of them have xattrs disabled,
   * then disable for both.
   */
  OstreeRepo *repo;
  if (OSTREE_IS_REPO_FILE (a))
    repo = ostree_repo_file_get_repo ((OstreeRepoFile*)a);
  else if (OSTREE_IS_REPO_FILE (b))
    repo = ostree_repo_file_get_repo ((OstreeRepoFile*)b);
  else
    repo = NULL;
  if (repo != NULL && repo->disable_xattrs)
    flags |= OSTREE_DIFF_FLAGS_IGNORE_XATTRS;

  if (a == NULL)
    {
      if (!diff_add_dir_recurse (b, added, cancellable, error))
        goto out;

      ret = TRUE;
      goto out;
    }

  child_a_info = g_file_query_info (a, OSTREE_GIO_FAST_QUERYINFO,
                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                    cancellable, error);
  if (!child_a_info)
    goto out;

  child_b_info = g_file_query_info (b, OSTREE_GIO_FAST_QUERYINFO,
                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                    cancellable, error);
  if (!child_b_info)
    goto out;

  /* Fast path test for unmodified directories */
  if (g_file_info_get_file_type (child_a_info) == G_FILE_TYPE_DIRECTORY
      && g_file_info_get_file_type (child_b_info) == G_FILE_TYPE_DIRECTORY
      && OSTREE_IS_REPO_FILE (a)
      && OSTREE_IS_REPO_FILE (b))
    {
      OstreeRepoFile *a_repof = (OstreeRepoFile*) a;
      OstreeRepoFile *b_repof = (OstreeRepoFile*) b;
      
      if (strcmp (ostree_repo_file_tree_get_contents_checksum (a_repof),
                  ostree_repo_file_tree_get_contents_checksum (b_repof)) == 0)
        {
          ret = TRUE;
          goto out;
        }
    }

  g_clear_object (&child_a_info);
  g_clear_object (&child_b_info);

  dir_enum = g_file_enumerate_children (a, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, error);
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
          if (options->owner_uid >= 0)
            g_file_info_set_attribute_uint32 (child_b_info, "unix::uid", options->owner_uid);
          if (options->owner_gid >= 0)
            g_file_info_set_attribute_uint32 (child_b_info, "unix::gid", options->owner_gid);

          child_b_type = g_file_info_get_file_type (child_b_info);
          if (child_a_type != child_b_type)
            {
              OstreeDiffItem *diff_item = diff_item_new (child_a, child_a_info,
                                                   child_b, child_b_info, NULL, NULL);
              
              g_ptr_array_add (modified, diff_item);
            }
          else
            {
              OstreeDiffItem *diff_item = NULL;

              if (!diff_files (flags, child_a, child_a_info, child_b, child_b_info, &diff_item,
                               cancellable, error))
                goto out;
              
              if (diff_item)
                g_ptr_array_add (modified, diff_item); /* Transfer ownership */

              if (child_a_type == G_FILE_TYPE_DIRECTORY)
                {
                  if (!ostree_diff_dirs_with_options (flags, child_a, child_b, modified,
                                                      removed, added, options,
                                                      cancellable, error))
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
                                        cancellable, error);
  if (!dir_enum)
    goto out;

  g_clear_object (&child_b_info);
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
      g_clear_object (&child_b_info);
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

static void
print_diff_item (char        prefix,
                 GFile      *base,
                 GFile      *file)
{
  if (g_file_is_native (file))
    {
      g_autofree char *relpath = g_file_get_relative_path (base, file);
      g_print ("%c    %s\n", prefix, relpath);
    }
  else
    {
      g_print ("%c    %s\n", prefix, gs_file_get_path_cached (file));
    }
}

/**
 * ostree_diff_print:
 * @a: First directory path
 * @b: First directory path
 * @modified: (element-type OstreeDiffItem): Modified files
 * @removed: (element-type Gio.File): Removed files
 * @added: (element-type Gio.File): Added files
 *
 * Print the contents of a diff to stdout.
 */
void
ostree_diff_print (GFile          *a,
                   GFile          *b,
                   GPtrArray      *modified,
                   GPtrArray      *removed,
                   GPtrArray      *added)
{
  guint i;

  for (i = 0; i < modified->len; i++)
    {
      OstreeDiffItem *diff = modified->pdata[i];
      print_diff_item ('M', a, diff->src);
    }
  for (i = 0; i < removed->len; i++)
    {
      GFile *removed_file = removed->pdata[i];
      print_diff_item ('D', a, removed_file);
    }
  for (i = 0; i < added->len; i++)
    {
      GFile *added_f = added->pdata[i];
      print_diff_item ('A', b, added_f);
    }
}
