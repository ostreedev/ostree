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

#include "ostree-core-private.h"
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
  g_autofree char *path = _ostree_get_commitpartial_path (checksum);
  if (unlinkat (repo->repo_dir_fd, path, 0) != 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "unlinkat");
    }

  return TRUE;
}

static gboolean
maybe_prune_loose_object (OtPruneData        *data,
                          OstreeRepoPruneFlags    flags,
                          const char         *checksum,
                          OstreeObjectType    objtype,
                          GCancellable       *cancellable,
                          GError            **error)
{
  g_autoptr(GVariant) key = NULL;

  key = ostree_object_name_serialize (checksum, objtype);

  if (!g_hash_table_lookup_extended (data->reachable, key, NULL, NULL))
    {
      g_debug ("Pruning unneeded object %s.%s", checksum,
               ostree_object_type_to_string (objtype));
      if (!(flags & OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE))
        {
          guint64 storage_size = 0;

          if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
            {
              if (!prune_commitpartial_file (data->repo, checksum, cancellable, error))
                return FALSE;
            }

          if (!ostree_repo_query_object_storage_size (data->repo, objtype, checksum,
                                                      &storage_size, cancellable, error))
            return FALSE;

          if (!ostree_repo_delete_object (data->repo, objtype, checksum,
                                          cancellable, error))
            return FALSE;

          data->freed_bytes += storage_size;
        }
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        data->n_unreachable_meta++;
      else
        data->n_unreachable_content++;
    }
  else
    {
      g_debug ("Keeping needed object %s.%s", checksum,
               ostree_object_type_to_string (objtype));
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        data->n_reachable_meta++;
      else
        data->n_reachable_content++;
    }

  return TRUE;
}

static gboolean
_ostree_repo_prune_tmp (OstreeRepo *self,
                        GCancellable *cancellable,
                        GError **error)
{
  if (self->cache_dir_fd == -1)
    return TRUE;

  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  gboolean exists;
  if (!ot_dfd_iter_init_allow_noent (self->cache_dir_fd, _OSTREE_SUMMARY_CACHE_DIR,
                                     &dfd_iter, &exists, error))
    return FALSE;
  /* Note early return */
  if (!exists)
    return TRUE;

  while (TRUE)
    {
      size_t len;
      gboolean has_sig_suffix = FALSE;
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      len = strlen (dent->d_name);
      if (len > 4 && g_strcmp0 (dent->d_name + len - 4, ".sig") == 0)
        {
          has_sig_suffix = TRUE;
          dent->d_name[len - 4] = '\0';
        }

      if (!g_hash_table_contains (self->remotes, dent->d_name))
        {
          /* Restore the previous value to get the file name.  */
          if (has_sig_suffix)
            dent->d_name[len - 4] = '.';

          if (unlinkat (dfd_iter.fd, dent->d_name, 0) < 0)
            return glnx_throw_errno_prefix (error, "unlinkat");
        }
    }

  return TRUE;
}


/**
 * ostree_repo_prune_static_deltas:
 * @self: Repo
 * @commit: (allow-none): ASCII SHA256 checksum for commit, or %NULL for each
 * non existing commit
 * @cancellable: Cancellable
 * @error: Error
 *
 * Prune static deltas, if COMMIT is specified then delete static delta files only
 * targeting that commit; otherwise any static delta of non existing commits are
 * deleted.
 */
gboolean
ostree_repo_prune_static_deltas (OstreeRepo *self, const char *commit,
                                 GCancellable      *cancellable,
                                 GError           **error)
{
  g_autoptr(GPtrArray) deltas = NULL;
  if (!ostree_repo_list_static_delta_names (self, &deltas,
                                            cancellable, error))
    return FALSE;

  for (guint i = 0; i < deltas->len; i++)
    {
      const char *deltaname = deltas->pdata[i];
      const char *dash = strchr (deltaname, '-');
      const char *to = NULL;
      g_autofree char *from = NULL;

      if (!dash)
        {
          to = deltaname;
        }
      else
        {
          from = g_strndup (deltaname, dash - deltaname);
          to = dash + 1;
        }

      if (commit)
        {
          if (g_strcmp0 (to, commit))
            continue;
        }
      else
        {
          gboolean have_commit;
          if (!ostree_repo_has_object (self, OSTREE_OBJECT_TYPE_COMMIT,
                                       to, &have_commit,
                                       cancellable, error))
            return FALSE;

          if (have_commit)
            continue;
        }

      g_debug ("Trying to prune static delta %s", deltaname);
      g_autofree char *deltadir = _ostree_get_relative_static_delta_path (from, to, NULL);
      if (!glnx_shutil_rm_rf_at (self->repo_dir_fd, deltadir,
                                 cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
repo_prune_internal (OstreeRepo        *self,
                     GHashTable        *objects,
                     OstreeRepoPruneOptions *options,
                     gint              *out_objects_total,
                     gint              *out_objects_pruned,
                     guint64           *out_pruned_object_size_total,
                     GCancellable      *cancellable,
                     GError           **error)
{
  GHashTableIter hash_iter;
  gpointer key, value;
  OtPruneData data = { 0, };

  data.repo = self;
  /* We unref this when we're done */
  g_autoptr(GHashTable) reachable_owned = g_hash_table_ref (options->reachable);
  data.reachable = reachable_owned;

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

      if (!maybe_prune_loose_object (&data, options->flags, checksum, objtype,
                                     cancellable, error))
        return FALSE;
    }

  if (!ostree_repo_prune_static_deltas (self, NULL, cancellable, error))
    return FALSE;

  if (!_ostree_repo_prune_tmp (self, cancellable, error))
    return FALSE;

  *out_objects_total = (data.n_reachable_meta + data.n_unreachable_meta +
                        data.n_reachable_content + data.n_unreachable_content);
  *out_objects_pruned = (data.n_unreachable_meta + data.n_unreachable_content);
  *out_pruned_object_size_total = data.freed_bytes;
  return TRUE;
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
  GHashTableIter hash_iter;
  gpointer key, value;
  g_autoptr(GHashTable) objects = NULL;
  g_autoptr(GHashTable) all_refs = NULL;
  g_autoptr(GHashTable) reachable = NULL;
  gboolean refs_only = flags & OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY;

  reachable = ostree_repo_traverse_new_reachable ();

  /* This original prune API has fixed logic for traversing refs or all commits
   * combined with actually deleting content. The newer backend API just does
   * the deletion.
   */

  if (refs_only)
    {
      if (!ostree_repo_list_refs (self, NULL, &all_refs,
                                  cancellable, error))
        return FALSE;

      g_hash_table_iter_init (&hash_iter, all_refs);

      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *checksum = value;

          g_debug ("Finding objects to keep for commit %s", checksum);
          if (!ostree_repo_traverse_commit_union (self, checksum, depth, reachable,
                                                  cancellable, error))
            return FALSE;
        }
    }

  if (!ostree_repo_list_objects (self, OSTREE_REPO_LIST_OBJECTS_ALL | OSTREE_REPO_LIST_OBJECTS_NO_PARENTS,
                                 &objects, cancellable, error))
    return FALSE;

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

          g_debug ("Finding objects to keep for commit %s", checksum);
          if (!ostree_repo_traverse_commit_union (self, checksum, depth, reachable,
                                                  cancellable, error))
            return FALSE;
        }
    }

  { OstreeRepoPruneOptions opts = { flags, reachable };
    return repo_prune_internal (self, objects, &opts,
                                out_objects_total, out_objects_pruned,
                                out_pruned_object_size_total, cancellable, error);
  }
}

/**
 * ostree_repo_prune_from_reachable:
 * @self: Repo
 * @options: Options controlling prune process
 * @out_objects_total: (out): Number of objects found
 * @out_objects_pruned: (out): Number of objects deleted
 * @out_pruned_object_size_total: (out): Storage size in bytes of objects deleted
 * @cancellable: Cancellable
 * @error: Error
 *
 * Delete content from the repository.  This function is the "backend"
 * half of the higher level ostree_repo_prune().  To use this function,
 * you determine the root set yourself, and this function finds all other
 * unreferenced objects and deletes them.
 *
 * Use this API when you want to perform more selective pruning - for example,
 * retain all commits from a production branch, but just GC some history from
 * your dev branch.
 *
 * The %OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE flag may be specified to just determine
 * statistics on objects that would be deleted, without actually deleting them.
 */
gboolean
ostree_repo_prune_from_reachable (OstreeRepo        *self,
                                  OstreeRepoPruneOptions *options,
                                  gint              *out_objects_total,
                                  gint              *out_objects_pruned,
                                  guint64           *out_pruned_object_size_total,
                                  GCancellable      *cancellable,
                                  GError           **error)
{
  g_autoptr(GHashTable) objects = NULL;

  if (!ostree_repo_list_objects (self, OSTREE_REPO_LIST_OBJECTS_ALL | OSTREE_REPO_LIST_OBJECTS_NO_PARENTS,
                                 &objects, cancellable, error))
    return FALSE;

  return repo_prune_internal (self, objects, options, out_objects_total,
                              out_objects_pruned, out_pruned_object_size_total,
                              cancellable, error);
}
