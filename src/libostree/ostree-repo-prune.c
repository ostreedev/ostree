/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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

#include "ostree-repo-private.h"
#include "otutil.h"

typedef struct {
  OstreeRepo *repo;
  GHashTable *reachable;
  guint n_reachable_meta;
  guint n_reachable_content;
  guint n_unreachable_meta;
  guint n_unreachable_content;
  guint64 freed_bytes;
} OtPruneData;

static gboolean
prune_commitpartial_file (OstreeRepo    *repo,
                          const char    *checksum,
                          GCancellable  *cancellable,
                          GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *objpath = ot_gfile_resolve_path_printf (repo->repodir, "state/%s.commitpartial", checksum);
  
  if (!ot_gfile_ensure_unlinked (objpath, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
maybe_prune_loose_object (OtPruneData        *data,
                          OstreeRepoPruneFlags    flags,
                          const char         *checksum,
                          OstreeObjectType    objtype,
                          GCancellable       *cancellable,
                          GError            **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *key = NULL;

  key = ostree_object_name_serialize (checksum, objtype);

  if (!g_hash_table_lookup_extended (data->reachable, key, NULL, NULL))
    {
      if (!(flags & OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE))
        {
          guint64 storage_size = 0;

          if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
            {
              if (!prune_commitpartial_file (data->repo, checksum, cancellable, error))
                goto out;
            }

          if (!ostree_repo_query_object_storage_size (data->repo, objtype, checksum,
                                                      &storage_size, cancellable, error))
            goto out;

          if (!ostree_repo_delete_object (data->repo, objtype, checksum,
                                          cancellable, error))
            goto out;

          data->freed_bytes += storage_size;
        }
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        data->n_unreachable_meta++;
      else
        data->n_unreachable_content++;
    }
  else
    {
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        data->n_reachable_meta++;
      else
        data->n_reachable_content++;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_prune:
 * @self: Repo
 * @flags: Options controlling prune process
 * @depth: Stop traversal after this many iterations (-1 for unlimited)
 * @out_objects_total: (out): Number of objects found
 * @out_objects_pruned: (out): Number of objects deleted
 * @out_pruned_object_size_total: (out): Storage size in bytes of objects deleted
 * @cancellable: Cancellable
 * @error: Error
 *
 * Delete content from the repository.  By default, this function will
 * only delete "orphaned" objects not referred to by any commit.  This
 * can happen during a local commit operation, when we have written
 * content objects but not saved the commit referencing them.
 *
 * However, if %OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY is provided, instead
 * of traversing all commits, only refs will be used.  Particularly
 * when combined with @depth, this is a convenient way to delete
 * history from the repository.
 * 
 * Use the %OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE to just determine
 * statistics on objects that would be deleted, without actually
 * deleting them.
 */
gboolean
ostree_repo_prune (OstreeRepo        *self,
                   OstreeRepoPruneFlags   flags,
                   gint               depth,
                   gint              *out_objects_total,
                   gint              *out_objects_pruned,
                   guint64           *out_pruned_object_size_total,
                   GCancellable      *cancellable,
                   GError           **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  gs_unref_hashtable GHashTable *objects = NULL;
  gs_unref_hashtable GHashTable *all_refs = NULL;
  OtPruneData data = { 0, };
  gboolean refs_only = flags & OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY;

  data.repo = self;
  data.reachable = ostree_repo_traverse_new_reachable ();

  if (refs_only)
    {
      if (!ostree_repo_list_refs (self, NULL, &all_refs,
                                  cancellable, error))
        goto out;
      
      g_hash_table_iter_init (&hash_iter, all_refs);
      
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *checksum = value;
          
          if (!ostree_repo_traverse_commit_union (self, checksum, depth, data.reachable,
                                            cancellable, error))
            goto out;
        }
    }

  if (!ostree_repo_list_objects (self, OSTREE_REPO_LIST_OBJECTS_ALL, &objects,
                                 cancellable, error))
    goto out;

  if (!refs_only)
    {
      g_hash_table_iter_init (&hash_iter, objects);
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          GVariant *serialized_key = key;
          const char *checksum;
          OstreeObjectType objtype;

          ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

          if (objtype != OSTREE_OBJECT_TYPE_COMMIT)
            continue;
          
          if (!ostree_repo_traverse_commit_union (self, checksum, depth, data.reachable,
                                                  cancellable, error))
            goto out;
        }
    }

  g_hash_table_iter_init (&hash_iter, objects);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      GVariant *objdata = value;
      const char *checksum;
      OstreeObjectType objtype;
      gboolean is_loose;
      
      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);
      g_variant_get_child (objdata, 0, "b", &is_loose);

      if (!is_loose)
        continue;

      if (!maybe_prune_loose_object (&data, flags, checksum, objtype,
                                     cancellable, error))
        goto out;
    }
  ret = TRUE;
  *out_objects_total = (data.n_reachable_meta + data.n_unreachable_meta +
                        data.n_reachable_content + data.n_unreachable_content);
  *out_objects_pruned = (data.n_unreachable_meta + data.n_unreachable_content);
  *out_pruned_object_size_total = data.freed_bytes;
 out:
  if (data.reachable)
    g_hash_table_unref (data.reachable);
  return ret;
}
