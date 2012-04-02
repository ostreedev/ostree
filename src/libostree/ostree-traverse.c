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

#define _GNU_SOURCE

#include "config.h"

#include "ostree.h"
#include "otutil.h"

GHashTable *
ostree_traverse_new_reachable (void)
{
  return g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                (GDestroyNotify)g_variant_unref, NULL);
}

gboolean
ostree_traverse_dirtree (OstreeRepo      *repo,
                         const char      *dirtree_checksum,
                         GHashTable      *inout_reachable,
                         GCancellable    *cancellable,
                         GError         **error)
{
  gboolean ret = FALSE;
  GVariant *tree = NULL;
  GVariant *files_variant = NULL;
  GVariant *dirs_variant = NULL;
  int n, i;
  GVariant *key;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_DIR_TREE, dirtree_checksum, &tree, error))
    goto out;

  key = ostree_object_name_serialize (dirtree_checksum, OSTREE_OBJECT_TYPE_DIR_TREE);
  if (!g_hash_table_lookup (inout_reachable, key))
    { 
      g_hash_table_insert (inout_reachable, key, key);
      key = NULL;

      /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
      files_variant = g_variant_get_child_value (tree, 2);
      n = g_variant_n_children (files_variant);
      for (i = 0; i < n; i++)
        {
          const char *filename;
          const char *checksum;
      
          g_variant_get_child (files_variant, i, "(&s&s)", &filename, &checksum);
          if (ostree_repo_get_mode (repo) == OSTREE_REPO_MODE_BARE)
            {
              key = ostree_object_name_serialize (checksum, OSTREE_OBJECT_TYPE_RAW_FILE);
              g_hash_table_replace (inout_reachable, key, key);
              key = NULL;
            }
          else
            {
              key = ostree_object_name_serialize (checksum, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META);
              g_hash_table_replace (inout_reachable, key, key);
              key = ostree_object_name_serialize (checksum, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT);
              g_hash_table_replace (inout_reachable, key, key);
              key = NULL;
            }
        }

      dirs_variant = g_variant_get_child_value (tree, 3);
      n = g_variant_n_children (dirs_variant);
      for (i = 0; i < n; i++)
        {
          const char *dirname;
          const char *tree_checksum;
          const char *meta_checksum;
      
          g_variant_get_child (dirs_variant, i, "(&s&s&s)",
                               &dirname, &tree_checksum, &meta_checksum);
      
          if (!ostree_traverse_dirtree (repo, tree_checksum, inout_reachable,
                                        cancellable, error))
            goto out;

          key = ostree_object_name_serialize (meta_checksum, OSTREE_OBJECT_TYPE_DIR_META);
          g_hash_table_replace (inout_reachable, key, key);
          key = NULL;
        }
    }

  ret = TRUE;
 out:
  ot_clear_gvariant (&key);
  ot_clear_gvariant (&tree);
  ot_clear_gvariant (&files_variant);
  ot_clear_gvariant (&dirs_variant);
  return ret;
}

gboolean
ostree_traverse_commit (OstreeRepo      *repo,
                        const char      *commit_checksum,
                        int              maxdepth,
                        GHashTable      *inout_reachable,
                        GCancellable    *cancellable,
                        GError         **error)
{
  gboolean ret = FALSE;
  GVariant *commit = NULL;
  const char *contents_checksum;
  const char *meta_checksum;
  GVariant *key;

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit_checksum, &commit, error))
    goto out;
  
  key = ostree_object_name_serialize (commit_checksum, OSTREE_OBJECT_TYPE_COMMIT);
  g_hash_table_replace (inout_reachable, key, key);

  g_variant_get_child (commit, 7, "&s", &meta_checksum);
  key = ostree_object_name_serialize (meta_checksum, OSTREE_OBJECT_TYPE_DIR_META);
  g_hash_table_replace (inout_reachable, key, key);

  g_variant_get_child (commit, 6, "&s", &contents_checksum);
  if (!ostree_traverse_dirtree (repo, contents_checksum, inout_reachable, cancellable, error))
    goto out;

  if (maxdepth == -1 || maxdepth > 0)
    {
      const char *parent_checksum;

      g_variant_get_child (commit, 2, "&s", &parent_checksum);

      if (parent_checksum[0])
        {
          if (!ostree_traverse_commit (repo, parent_checksum,
                                       maxdepth > 0 ? maxdepth - 1 : -1,
                                       inout_reachable, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  ot_clear_gvariant (&commit);
  return ret;
}

