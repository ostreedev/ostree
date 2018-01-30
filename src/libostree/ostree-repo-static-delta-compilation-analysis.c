/*
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include <string.h>
#include <gio/gunixoutputstream.h>

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-lzma-compressor.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-diff.h"
#include "ostree-rollsum.h"
#include "otutil.h"
#include "ostree-varint.h"

void
_ostree_delta_content_sizenames_free (gpointer v)
{
  OstreeDeltaContentSizeNames *ce = v;
  g_free (ce->checksum);
  g_ptr_array_unref (ce->basenames);
  g_free (ce);
}

static gboolean
build_content_sizenames_recurse (OstreeRepo                     *repo,
                                 OstreeRepoCommitTraverseIter   *iter,
                                 GHashTable                     *sizenames_map,
                                 GHashTable                     *include_only_objects,
                                 GCancellable                   *cancellable,
                                 GError                        **error)
{
  gboolean ret = FALSE;

  while (TRUE)
    {
      OstreeRepoCommitIterResult iterres =
        ostree_repo_commit_traverse_iter_next (iter, cancellable, error);
          
      if (iterres == OSTREE_REPO_COMMIT_ITER_RESULT_ERROR)
        goto out;
      else if (iterres == OSTREE_REPO_COMMIT_ITER_RESULT_END)
        break;
      else if (iterres == OSTREE_REPO_COMMIT_ITER_RESULT_FILE)
        {
          char *name;
          char *checksum;
          OstreeDeltaContentSizeNames *csizenames;
            
          ostree_repo_commit_traverse_iter_get_file (iter, &name, &checksum);

          if (include_only_objects && !g_hash_table_contains (include_only_objects, checksum))
            continue;

          csizenames = g_hash_table_lookup (sizenames_map, checksum);
          if (!csizenames)
            {
              g_autoptr(GFileInfo) finfo = NULL;

              if (!ostree_repo_load_file (repo, checksum,
                                          NULL, &finfo, NULL,
                                          cancellable, error))
                goto out;

              if (g_file_info_get_file_type (finfo) != G_FILE_TYPE_REGULAR)
                continue;

              csizenames = g_new0 (OstreeDeltaContentSizeNames, 1);
              csizenames->checksum = g_strdup (checksum);
              csizenames->size = g_file_info_get_size (finfo);
              g_hash_table_replace (sizenames_map, csizenames->checksum, csizenames);
            }

          if (!csizenames->basenames)
            csizenames->basenames = g_ptr_array_new_with_free_func (g_free);
          g_ptr_array_add (csizenames->basenames, g_strdup (name));
        }
      else if (iterres == OSTREE_REPO_COMMIT_ITER_RESULT_DIR)
        {
          char *name;
          char *content_checksum;
          char *meta_checksum;
          g_autoptr(GVariant) dirtree = NULL;
          ostree_cleanup_repo_commit_traverse_iter
            OstreeRepoCommitTraverseIter subiter = { 0, };

          ostree_repo_commit_traverse_iter_get_dir (iter, &name, &content_checksum, &meta_checksum);
          
          if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_DIR_TREE,
                                         content_checksum, &dirtree,
                                         error))
            goto out;

          if (!ostree_repo_commit_traverse_iter_init_dirtree (&subiter, repo, dirtree,
                                                              OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                                              error))
            goto out;

          if (!build_content_sizenames_recurse (repo, &subiter,
                                                sizenames_map, include_only_objects,
                                                cancellable, error))
            goto out;
        }
      else
        g_assert_not_reached ();
    }
  ret = TRUE;
 out:
  return ret;
}

static int
compare_sizenames (const void  *a,
                   const void  *b)
{
  OstreeDeltaContentSizeNames *sn_a = *(OstreeDeltaContentSizeNames**)(void*)a;
  OstreeDeltaContentSizeNames *sn_b = *(OstreeDeltaContentSizeNames**)(void*)b;

  return sn_a->size - sn_b->size;
}

/*
 * Generate a sorted array of [(checksum: str, size: uint64, names: array[string]), ...]
 * for regular file content.
 */
static gboolean
build_content_sizenames_filtered (OstreeRepo              *repo,
                                  GVariant                *commit,
                                  GHashTable              *include_only_objects,
                                  GPtrArray              **out_sizenames,
                                  GCancellable            *cancellable,
                                  GError                 **error)
{
  gboolean ret = FALSE;
  g_autoptr(GPtrArray) ret_sizenames =
    g_ptr_array_new_with_free_func (_ostree_delta_content_sizenames_free);
  g_autoptr(GHashTable) sizenames_map =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, _ostree_delta_content_sizenames_free);
  ostree_cleanup_repo_commit_traverse_iter
    OstreeRepoCommitTraverseIter iter = { 0, };

  if (!ostree_repo_commit_traverse_iter_init_commit (&iter, repo, commit,
                                                     OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                                     error))
    goto out;

  if (!build_content_sizenames_recurse (repo, &iter, sizenames_map, include_only_objects,
                                        cancellable, error))
    goto out;

  { GHashTableIter hashiter;
    gpointer hkey, hvalue;

    g_hash_table_iter_init (&hashiter, sizenames_map);
    while (g_hash_table_iter_next (&hashiter, &hkey, &hvalue))
      {
        g_hash_table_iter_steal (&hashiter);
        g_ptr_array_add (ret_sizenames, hvalue);
      }
  }

  g_ptr_array_sort (ret_sizenames, compare_sizenames);

  ret = TRUE;
  if (out_sizenames)
    *out_sizenames = g_steal_pointer (&ret_sizenames);
 out:
  return ret;
}

static gboolean
string_array_nonempty_intersection (GPtrArray    *a,
                                    GPtrArray    *b,
                                    gboolean      fuzzy)
{
  guint i;
  for (i = 0; i < a->len; i++)
    {
      guint j;
      const char *a_str = a->pdata[i];
      const char *a_dot = strchr (a_str, '.');
      for (j = 0; j < b->len; j++)
        {
          const char *b_str = b->pdata[j];
          const char *b_dot = strchr (b_str, '.');
          /* When doing fuzzy comparison, just compare the part before the '.' if it exists.  */
          if (fuzzy && a_dot && b_dot && b_dot - b_str && b_dot - b_str == a_dot - a_str)
            {
              if (strncmp (a_str, b_str, a_dot - a_str) == 0)
                return TRUE;
            }
          else
            {
              if (strcmp (a_str, b_str) == 0)
                return TRUE;
            }
        }
    }
  return FALSE;
}

static gboolean
sizename_is_delta_candidate (OstreeDeltaContentSizeNames *sizename)
{
  /* Don't build candidates for the empty object */
  if (sizename->size == 0)
    return FALSE;

  /* Look for known non-delta-able files (currently just compression like xz) */
  for (guint i = 0; i < sizename->basenames->len; i++)
    {
      const char *name = sizename->basenames->pdata[i];
      /* We could replace this down the line with g_content_type_guess() or so,
       * but it's not clear to me that's a major win; we'd still need to
       * maintain a list of compression formats, and this doesn't require
       * allocation.
       * NB: We explicitly don't have .gz here in case someone might be
       * using --rsyncable for that.
       */
      const char *dot = strrchr (name, '.');
      if (!dot)
        continue;
      const char *extension = dot+1;
      /* Don't add .gz here, see above */
      if (g_str_equal (extension, "xz") || g_str_equal (extension, "bz2"))
        return FALSE;
    }

  /* Let's try it */
  return TRUE;
}

/*
 * Build up a map of files with matching basenames and similar size,
 * and use it to find apparently similar objects.
 *
 * @new_reachable_regfile_content is a Set<checksum> of new regular
 * file objects.
 *
 * Currently, @out_modified_regfile_content will be a Map<to checksum,from checksum>;
 * however in the future it would be easy to have this function return
 * multiple candidate matches.  The hard part would be changing
 * the delta compiler to iterate over all matches, determine
 * a cost for each one, then pick the best.
 */
gboolean
_ostree_delta_compute_similar_objects (OstreeRepo                 *repo,
                                       GVariant                   *from_commit,
                                       GVariant                   *to_commit,
                                       GHashTable                 *new_reachable_regfile_content,
                                       guint                       similarity_percent_threshold,
                                       GHashTable                **out_modified_regfile_content,
                                       GCancellable               *cancellable,
                                       GError                    **error)
{
  gboolean ret = FALSE;
  g_autoptr(GHashTable) ret_modified_regfile_content =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_autoptr(GPtrArray) from_sizes = NULL;
  g_autoptr(GPtrArray) to_sizes = NULL;
  guint i, j;
  guint lower;
  guint upper;

  if (!build_content_sizenames_filtered (repo, from_commit, NULL,
                                         &from_sizes,
                                         cancellable, error))
    goto out;

  if (!build_content_sizenames_filtered (repo, to_commit, new_reachable_regfile_content,
                                         &to_sizes,
                                         cancellable, error))
    goto out;

  /* Iterate over all newly added objects, find objects which have
   * similar basename and sizes.
   *
   * Because the arrays are sorted by size, we can maintain a `lower`
   * bound on the original (from) objects to start searching.
   */
  lower = 0;
  upper = from_sizes->len;
  for (i = 0; i < to_sizes->len; i++)
    {
      int fuzzy;
      gboolean found = FALSE;
      OstreeDeltaContentSizeNames *to_sizenames = to_sizes->pdata[i];
      const guint64 min_threshold = to_sizenames->size *
        (1.0-similarity_percent_threshold/100.0);
      const guint64 max_threshold = to_sizenames->size *
        (1.0+similarity_percent_threshold/100.0);

      if (!sizename_is_delta_candidate (to_sizenames))
        continue;

      for (fuzzy = 0; fuzzy < 2 && !found; fuzzy++)
        {
          for (j = lower; j < upper; j++)
            {
              OstreeDeltaContentSizeNames *from_sizenames = from_sizes->pdata[j];
              if (!sizename_is_delta_candidate (from_sizenames))
                continue;

              if (from_sizenames->size < min_threshold)
                {
                  lower++;
                  continue;
                }

              if (from_sizenames->size > max_threshold)
                break;

              if (!string_array_nonempty_intersection (from_sizenames->basenames,
                                                       to_sizenames->basenames,
                                                       fuzzy == 1))
                {
                  continue;
                }

              /* Only one candidate right now */
              g_hash_table_insert (ret_modified_regfile_content,
                                   g_strdup (to_sizenames->checksum),
                                   g_strdup (from_sizenames->checksum));
              found = TRUE;
              break;
            }
        }
    }

  ret = TRUE;
  if (out_modified_regfile_content)
    *out_modified_regfile_content = g_steal_pointer (&ret_modified_regfile_content);
 out:
  return ret;
}
