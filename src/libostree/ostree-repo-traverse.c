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

#include "libglnx.h"
#include "ostree.h"
#include "otutil.h"

struct _OstreeRepoRealCommitTraverseIter {
  gboolean initialized;
  OstreeRepo *repo;
  GVariant *commit;
  GVariant *current_dir;
  const char *name;
  OstreeRepoCommitIterResult state;
  guint idx;
  char checksum_content[65];
  char checksum_meta[65];
};

/**
 * ostree_repo_commit_traverse_iter_init_commit:
 * @iter: An iter
 * @repo: A repo
 * @commit: Variant of type %OSTREE_OBJECT_TYPE_COMMIT
 * @flags: Flags
 * @error: Error
 *
 * Initialize (in place) an iterator over the root of a commit object.
 */
gboolean
ostree_repo_commit_traverse_iter_init_commit (OstreeRepoCommitTraverseIter   *iter,
                                              OstreeRepo                     *repo,
                                              GVariant                       *commit,
                                              OstreeRepoCommitTraverseFlags   flags,
                                              GError                        **error)
{
  struct _OstreeRepoRealCommitTraverseIter *real =
    (struct _OstreeRepoRealCommitTraverseIter*)iter;
  gboolean ret = FALSE;
  const guchar *csum;
  g_autoptr(GVariant) meta_csum_bytes = NULL;
  g_autoptr(GVariant) content_csum_bytes = NULL;

  memset (real, 0, sizeof (*real));
  real->initialized = TRUE;
  real->repo = g_object_ref (repo);
  real->commit = g_variant_ref (commit);
  real->current_dir = NULL;
  real->idx = 0;

  g_variant_get_child (commit, 6, "@ay", &content_csum_bytes);
  csum = ostree_checksum_bytes_peek_validate (content_csum_bytes, error);
  if (!csum)
    goto out;
  ostree_checksum_inplace_from_bytes (csum, real->checksum_content);

  g_variant_get_child (commit, 7, "@ay", &meta_csum_bytes);
  csum = ostree_checksum_bytes_peek_validate (meta_csum_bytes, error);
  if (!csum)
    goto out;
  ostree_checksum_inplace_from_bytes (csum, real->checksum_meta);

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_commit_traverse_iter_init_dirtree:
 * @iter: An iter
 * @repo: A repo
 * @dirtree: Variant of type %OSTREE_OBJECT_TYPE_DIR_TREE
 * @flags: Flags
 * @error: Error
 *
 * Initialize (in place) an iterator over a directory tree.
 */
gboolean
ostree_repo_commit_traverse_iter_init_dirtree (OstreeRepoCommitTraverseIter   *iter,
                                               OstreeRepo                     *repo,
                                               GVariant                       *dirtree,
                                               OstreeRepoCommitTraverseFlags   flags,
                                               GError                        **error)
{
  struct _OstreeRepoRealCommitTraverseIter *real =
    (struct _OstreeRepoRealCommitTraverseIter*)iter;

  memset (real, 0, sizeof (*real));
  real->initialized = TRUE;
  real->repo = g_object_ref (repo);
  real->current_dir = g_variant_ref (dirtree);
  real->idx = 0;

  return TRUE;
}

/**
 * ostree_repo_commit_traverse_iter_next:
 * @iter: An iter
 * @cancellable: Cancellable
 * @error: Error
 *
 * Step the interator to the next item.  Files will be returned first,
 * then subdirectories.  Call this in a loop; upon encountering
 * %OSTREE_REPO_COMMIT_ITER_RESULT_END, there will be no more files or
 * directories.  If %OSTREE_REPO_COMMIT_ITER_RESULT_DIR is returned,
 * then call ostree_repo_commit_traverse_iter_get_dir() to retrieve
 * data for that directory.  Similarly, if
 * %OSTREE_REPO_COMMIT_ITER_RESULT_FILE is returned, call
 * ostree_repo_commit_traverse_iter_get_file().
 * 
 * If %OSTREE_REPO_COMMIT_ITER_RESULT_ERROR is returned, it is a
 * program error to call any further API on @iter except for
 * ostree_repo_commit_traverse_iter_clear().
 */
OstreeRepoCommitIterResult
ostree_repo_commit_traverse_iter_next (OstreeRepoCommitTraverseIter *iter,
                                       GCancellable                 *cancellable,
                                       GError                      **error)
{
  struct _OstreeRepoRealCommitTraverseIter *real =
    (struct _OstreeRepoRealCommitTraverseIter*)iter;
  OstreeRepoCommitIterResult res = OSTREE_REPO_COMMIT_ITER_RESULT_ERROR;

  if (!real->current_dir)
    {
      if (!ostree_repo_load_variant (real->repo, OSTREE_OBJECT_TYPE_DIR_TREE,
                                     real->checksum_content,
                                     &real->current_dir,
                                     error))
        goto out;
      res = OSTREE_REPO_COMMIT_ITER_RESULT_DIR;
    }
  else
    {
      guint nfiles;
      guint ndirs;
      guint idx;
      const guchar *csum;
      g_autoptr(GVariant) content_csum_v = NULL;
      g_autoptr(GVariant) meta_csum_v = NULL;
      g_autoptr(GVariant) files_variant = NULL;
      g_autoptr(GVariant) dirs_variant = NULL;

      files_variant = g_variant_get_child_value (real->current_dir, 0);
      dirs_variant = g_variant_get_child_value (real->current_dir, 1);

      nfiles = g_variant_n_children (files_variant);
      ndirs = g_variant_n_children (dirs_variant);
      if (real->idx < nfiles)
        {
          idx = real->idx;
          g_variant_get_child (files_variant, idx, "(&s@ay)",
                               &real->name,
                               &content_csum_v);

          csum = ostree_checksum_bytes_peek_validate (content_csum_v, error);
          if (!csum)
            goto out;
          ostree_checksum_inplace_from_bytes (csum, real->checksum_content);

          res = OSTREE_REPO_COMMIT_ITER_RESULT_FILE;

          real->idx++;
        }
      else if (real->idx < nfiles + ndirs)
        {
          idx = real->idx - nfiles;

          g_variant_get_child (dirs_variant, idx, "(&s@ay@ay)",
                               &real->name, &content_csum_v, &meta_csum_v);

          csum = ostree_checksum_bytes_peek_validate (content_csum_v, error);
          if (!csum)
            goto out;
          ostree_checksum_inplace_from_bytes (csum, real->checksum_content);

          csum = ostree_checksum_bytes_peek_validate (meta_csum_v, error);
          if (!csum)
            goto out;
          ostree_checksum_inplace_from_bytes (csum, real->checksum_meta);
          
          res = OSTREE_REPO_COMMIT_ITER_RESULT_DIR;

          real->idx++;
        }
      else
        res = OSTREE_REPO_COMMIT_ITER_RESULT_END;
    }
  
  real->state = res;
 out:
  return res;
}

/**
 * ostree_repo_commit_traverse_iter_get_file:
 * @iter: An iter
 * @out_name: (out) (transfer none): Name of current file
 * @out_checksum: (out) (transfer none): Checksum of current file
 *
 * Return information on the current file.  This function may only be
 * called if %OSTREE_REPO_COMMIT_ITER_RESULT_FILE was returned from
 * ostree_repo_commit_traverse_iter_next().
 */
void
ostree_repo_commit_traverse_iter_get_file (OstreeRepoCommitTraverseIter *iter,
                                           char                        **out_name,
                                           char                        **out_checksum)
{
  struct _OstreeRepoRealCommitTraverseIter *real =
    (struct _OstreeRepoRealCommitTraverseIter*)iter;
  *out_name = (char*)real->name;
  *out_checksum = (char*)real->checksum_content;
}

/**
 * ostree_repo_commit_traverse_iter_get_dir:
 * @iter: An iter
 * @out_name: (out) (transfer none): Name of current dir
 * @out_content_checksum: (out) (transfer none): Checksum of current content
 * @out_meta_checksum: (out) (transfer none): Checksum of current metadata
 *
 * Return information on the current directory.  This function may
 * only be called if %OSTREE_REPO_COMMIT_ITER_RESULT_DIR was returned
 * from ostree_repo_commit_traverse_iter_next().
 */
void
ostree_repo_commit_traverse_iter_get_dir (OstreeRepoCommitTraverseIter *iter,
                                          char                        **out_name,
                                          char                        **out_content_checksum,
                                          char                        **out_meta_checksum)
{
  struct _OstreeRepoRealCommitTraverseIter *real =
    (struct _OstreeRepoRealCommitTraverseIter*)iter;
  *out_name = (char*)real->name;
  *out_content_checksum = (char*)real->checksum_content;
  *out_meta_checksum = (char*)real->checksum_meta;
}

void
ostree_repo_commit_traverse_iter_clear (OstreeRepoCommitTraverseIter *iter)
{
  struct _OstreeRepoRealCommitTraverseIter *real =
    (struct _OstreeRepoRealCommitTraverseIter*)iter;
  g_clear_pointer (&real->commit, g_variant_unref);
  g_clear_pointer (&real->current_dir, g_variant_unref);
}

void
ostree_repo_commit_traverse_iter_cleanup (void *p)
{
  OstreeRepoCommitTraverseIter *iter = p;
  struct _OstreeRepoRealCommitTraverseIter *real =
    (struct _OstreeRepoRealCommitTraverseIter*)iter;
  if (real->initialized)
    {
      ostree_repo_commit_traverse_iter_clear (iter);
      real->initialized = FALSE;
    }
}

/**
 * ostree_repo_traverse_new_reachable:
 *
 * This hash table is a set of #GVariant which can be accessed via
 * ostree_object_name_deserialize().
 *
 * Returns: (transfer container) (element-type GVariant GVariant): A new hash table
 */
GHashTable *
ostree_repo_traverse_new_reachable (void)
{
  return g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                NULL, (GDestroyNotify)g_variant_unref);
}

static gboolean
traverse_dirtree (OstreeRepo           *repo,
                  const char           *checksum,
                  GHashTable           *inout_reachable,
                  GCancellable         *cancellable,
                  GError              **error);

static gboolean
traverse_iter (OstreeRepo                          *repo,
               OstreeRepoCommitTraverseIter        *iter,
               GHashTable                          *inout_reachable,
               GCancellable                        *cancellable,
               GError                             **error)
{
  gboolean ret = FALSE;

  while (TRUE)
    {
      g_autoptr(GVariant) key = NULL;
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

          ostree_repo_commit_traverse_iter_get_file (iter, &name, &checksum);

          g_debug ("Found file object %s", checksum);
          key = ostree_object_name_serialize (checksum, OSTREE_OBJECT_TYPE_FILE);
          g_hash_table_replace (inout_reachable, key, key);
          key = NULL;
        }
      else if (iterres == OSTREE_REPO_COMMIT_ITER_RESULT_DIR)
        {
          char *name;
          char *content_checksum;
          char *meta_checksum;

          ostree_repo_commit_traverse_iter_get_dir (iter, &name, &content_checksum,
                                                    &meta_checksum);

          g_debug ("Found dirtree object %s", content_checksum);
          g_debug ("Found dirmeta object %s", meta_checksum);
          key = ostree_object_name_serialize (meta_checksum, OSTREE_OBJECT_TYPE_DIR_META);
          g_hash_table_replace (inout_reachable, key, key);
          key = NULL;

          key = ostree_object_name_serialize (content_checksum, OSTREE_OBJECT_TYPE_DIR_TREE);
          if (!g_hash_table_lookup (inout_reachable, key))
            {
              g_hash_table_replace (inout_reachable, key, key);
              key = NULL;

              if (!traverse_dirtree (repo, content_checksum, inout_reachable,
                                     cancellable, error))
                goto out;
            }
        }
      else
        g_assert_not_reached ();
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
traverse_dirtree (OstreeRepo           *repo,
                  const char           *checksum,
                  GHashTable           *inout_reachable,
                  GCancellable         *cancellable,
                  GError              **error)
{
  gboolean ret = FALSE;
  g_autoptr(GVariant) dirtree = NULL;
  ostree_cleanup_repo_commit_traverse_iter
    OstreeRepoCommitTraverseIter iter = { 0, };

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_DIR_TREE, checksum,
                                 &dirtree, error))
    goto out;

  g_debug ("Traversing dirtree %s", checksum);
  if (!ostree_repo_commit_traverse_iter_init_dirtree (&iter, repo, dirtree,
                                                      OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                                      error))
    goto out;

  if (!traverse_iter (repo, &iter, inout_reachable, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_traverse_commit_union: (skip)
 * @repo: Repo
 * @commit_checksum: ASCII SHA256 checksum
 * @maxdepth: Traverse this many parent commits, -1 for unlimited
 * @inout_reachable: Set of reachable objects
 * @cancellable: Cancellable
 * @error: Error
 *
 * Update the set @inout_reachable containing all objects reachable
 * from @commit_checksum, traversing @maxdepth parent commits.
 */
gboolean
ostree_repo_traverse_commit_union (OstreeRepo      *repo,
                                   const char      *commit_checksum,
                                   int              maxdepth,
                                   GHashTable      *inout_reachable,
                                   GCancellable    *cancellable,
                                   GError         **error)
{
  gboolean ret = FALSE;
  g_autofree char *tmp_checksum = NULL;

  while (TRUE)
    {
      gboolean recurse = FALSE;
      g_autoptr(GVariant) key = NULL;
      g_autoptr(GVariant) commit = NULL;
      ostree_cleanup_repo_commit_traverse_iter
        OstreeRepoCommitTraverseIter iter = { 0, };

      key = ostree_object_name_serialize (commit_checksum, OSTREE_OBJECT_TYPE_COMMIT);

      if (g_hash_table_contains (inout_reachable, key))
        break;

      if (!ostree_repo_load_variant_if_exists (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                               commit_checksum, &commit,
                                               error))
        goto out;
        
      /* Just return if the parent isn't found; we do expect most
       * people to have partial repositories.
       */
      if (!commit)
        break;

      g_hash_table_add (inout_reachable, key);
      key = NULL;

      g_debug ("Traversing commit %s", commit_checksum);
      if (!ostree_repo_commit_traverse_iter_init_commit (&iter, repo, commit,
                                                         OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                                         error))
        goto out;

      if (!traverse_iter (repo, &iter, inout_reachable, cancellable, error))
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

/**
 * ostree_repo_traverse_commit:
 * @repo: Repo
 * @commit_checksum: ASCII SHA256 checksum
 * @maxdepth: Traverse this many parent commits, -1 for unlimited
 * @out_reachable: (out) (transfer container) (element-type GVariant GVariant): Set of reachable objects
 * @cancellable: Cancellable
 * @error: Error
 *
 * Create a new set @out_reachable containing all objects reachable
 * from @commit_checksum, traversing @maxdepth parent commits.
 */
gboolean
ostree_repo_traverse_commit (OstreeRepo      *repo,
                             const char      *commit_checksum,
                             int              maxdepth,
                             GHashTable     **out_reachable,
                             GCancellable    *cancellable,
                             GError         **error)
{
  gboolean ret = FALSE;
  g_autoptr(GHashTable) ret_reachable =
    ostree_repo_traverse_new_reachable ();

  if (!ostree_repo_traverse_commit_union (repo, commit_checksum, maxdepth,
                                          ret_reachable, cancellable, error))
    goto out;

  ret = TRUE;
  if (out_reachable)
    *out_reachable = g_steal_pointer (&ret_reachable);
 out:
  return ret;
}
