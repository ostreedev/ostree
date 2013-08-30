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

#include "ostree.h"
#include "otutil.h"
#include "libgsystem.h"

/**
 * ostree_repo_traverse_new_reachable:
 *
 * This hash table is a set of #GVariant which can be accessed via
 * ostree_object_name_deserialize().
 *
 * Returns: (transfer full) (element-type GVariant GVariant): A new hash table
 */
GHashTable *
ostree_repo_traverse_new_reachable (void)
{
  return g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                (GDestroyNotify)g_variant_unref, NULL);
}

static gboolean
traverse_dirtree_internal (OstreeRepo      *repo,
                           const char      *dirtree_checksum,
                           int              recursion_depth,
                           GHashTable      *inout_reachable,
                           GCancellable    *cancellable,
                           GError         **error)
{
  gboolean ret = FALSE;
  int n, i;
  gs_unref_variant GVariant *key = NULL;
  gs_unref_variant GVariant *tree = NULL;
  gs_unref_variant GVariant *files_variant = NULL;
  gs_unref_variant GVariant *dirs_variant = NULL;
  gs_unref_variant GVariant *csum_v = NULL;
  gs_unref_variant GVariant *content_csum_v = NULL;
  gs_unref_variant GVariant *metadata_csum_v = NULL;
  gs_free char *tmp_checksum = NULL;

  if (recursion_depth > OSTREE_MAX_RECURSION)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Maximum recursion limit reached during traversal");
      goto out;
    }

  if (!ostree_repo_load_variant_if_exists (repo, OSTREE_OBJECT_TYPE_DIR_TREE, dirtree_checksum, &tree, error))
    goto out;

  if (!tree)
    return TRUE;

  key = ostree_object_name_serialize (dirtree_checksum, OSTREE_OBJECT_TYPE_DIR_TREE);
  if (!g_hash_table_lookup (inout_reachable, key))
    { 
      g_hash_table_insert (inout_reachable, key, key);
      key = NULL;

      /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
      files_variant = g_variant_get_child_value (tree, 0);
      n = g_variant_n_children (files_variant);
      for (i = 0; i < n; i++)
        {
          const char *filename;
      
          g_clear_pointer (&csum_v, (GDestroyNotify) g_variant_unref);
          g_variant_get_child (files_variant, i, "(&s@ay)", &filename, &csum_v);
          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes_v (csum_v);
          key = ostree_object_name_serialize (tmp_checksum, OSTREE_OBJECT_TYPE_FILE);
          g_hash_table_replace (inout_reachable, key, key);
          key = NULL;
        }

      dirs_variant = g_variant_get_child_value (tree, 1);
      n = g_variant_n_children (dirs_variant);
      for (i = 0; i < n; i++)
        {
          const char *dirname;
      
          g_clear_pointer (&content_csum_v, (GDestroyNotify) g_variant_unref);
          g_clear_pointer (&metadata_csum_v, (GDestroyNotify) g_variant_unref);
          g_variant_get_child (dirs_variant, i, "(&s@ay@ay)",
                               &dirname, &content_csum_v, &metadata_csum_v);
      
          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes_v (content_csum_v);
          if (!traverse_dirtree_internal (repo, tmp_checksum, recursion_depth + 1,
                                          inout_reachable, cancellable, error))
            goto out;

          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes_v (metadata_csum_v);
          key = ostree_object_name_serialize (tmp_checksum, OSTREE_OBJECT_TYPE_DIR_META);
          g_hash_table_replace (inout_reachable, key, key);
          key = NULL;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_traverse_dirtree (OstreeRepo      *repo,
                              const char      *dirtree_checksum,
                              GHashTable      *inout_reachable,
                              GCancellable    *cancellable,
                              GError         **error)
{
  return traverse_dirtree_internal (repo, dirtree_checksum, 0,
                                    inout_reachable, cancellable, error);
}

/**
 * ostree_traverse_commit:
 *
 * Add to @inout_reachable all objects reachable from
 * @commit_checksum, traversing @maxdepth parent commits.
 */
gboolean
ostree_repo_traverse_commit (OstreeRepo      *repo,
                             const char      *commit_checksum,
                             int              maxdepth,
                             GHashTable      *inout_reachable,
                             GCancellable    *cancellable,
                             GError         **error)
{
  gboolean ret = FALSE;
  gs_free char*tmp_checksum = NULL;

  while (TRUE)
    {
      gboolean recurse = FALSE;
      gs_unref_variant GVariant *meta_csum_bytes = NULL;
      gs_unref_variant GVariant *content_csum_bytes = NULL;
      gs_unref_variant GVariant *key = NULL;
      gs_unref_variant GVariant *commit = NULL;

      key = ostree_object_name_serialize (commit_checksum, OSTREE_OBJECT_TYPE_COMMIT);

      if (g_hash_table_contains (inout_reachable, key))
        break;

      /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
      if (!ostree_repo_load_variant_if_exists (repo, OSTREE_OBJECT_TYPE_COMMIT, commit_checksum, &commit, error))
        goto out;

      /* Just return if the parent isn't found; we do expect most
       * people to have partial repositories.
       */
      if (!commit)
        break;
  
      g_hash_table_add (inout_reachable, key);
      key = NULL;

      g_variant_get_child (commit, 7, "@ay", &meta_csum_bytes);
      g_free (tmp_checksum);
      if (G_UNLIKELY (g_variant_n_children (meta_csum_bytes) == 0))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Corrupted commit '%s'; invalid tree metadata",
                       commit_checksum);
          goto out;
        }

      tmp_checksum = ostree_checksum_from_bytes_v (meta_csum_bytes);
      key = ostree_object_name_serialize (tmp_checksum, OSTREE_OBJECT_TYPE_DIR_META);
      g_hash_table_replace (inout_reachable, key, key);
      key = NULL;

      g_variant_get_child (commit, 6, "@ay", &content_csum_bytes);
      g_free (tmp_checksum);
      if (G_UNLIKELY (g_variant_n_children (content_csum_bytes) == 0))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Corrupted commit '%s'; invalid tree content",
                       commit_checksum);
          goto out;
        }

      tmp_checksum = ostree_checksum_from_bytes_v (content_csum_bytes);
      if (!ostree_repo_traverse_dirtree (repo, tmp_checksum, inout_reachable, cancellable, error))
        goto out;

      if (maxdepth == -1 || maxdepth > 0)
        {
          g_free (tmp_checksum);
          tmp_checksum = ostree_commit_get_parent (commit);
          if (tmp_checksum)
            {
              commit_checksum = tmp_checksum;
              if (maxdepth > 0)
                maxdepth -= 1;
              recurse = TRUE;
            }
        }
      if (!recurse)
        break;
    }

  ret = TRUE;
 out:
  return ret;
}
