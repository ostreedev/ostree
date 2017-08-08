/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright © 2017 Endless Mobile, Inc.
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
 * Authors:
 *  - Philip Withnall <withnall@endlessm.com>
 */

#include "config.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>

#include "ostree-autocleanups.h"
#include "ostree-core.h"
#include "ostree-remote-private.h"
#include "ostree-repo-finder.h"
#include "ostree-repo.h"

static void ostree_repo_finder_default_init (OstreeRepoFinderInterface *iface);

G_DEFINE_INTERFACE (OstreeRepoFinder, ostree_repo_finder, G_TYPE_OBJECT)

static void
ostree_repo_finder_default_init (OstreeRepoFinderInterface *iface)
{
  /* Nothing to see here. */
}

/* Validate the given struct contains a valid collection ID and ref name, and that
 * the collection ID is non-%NULL. */
static gboolean
is_valid_collection_ref (const OstreeCollectionRef *ref)
{
  return (ref != NULL &&
          ostree_validate_rev (ref->ref_name, NULL) &&
          ostree_validate_collection_id (ref->collection_id, NULL));
}

/* Validate @refs is non-%NULL, non-empty, and contains only valid collection
 * and ref names. */
static gboolean
is_valid_collection_ref_array (const OstreeCollectionRef * const *refs)
{
  gsize i;

  if (refs == NULL || *refs == NULL)
    return FALSE;

  for (i = 0; refs[i] != NULL; i++)
    {
      if (!is_valid_collection_ref (refs[i]))
        return FALSE;
    }

  return TRUE;
}

/* Validate @ref_to_checksum is non-%NULL, non-empty, and contains only valid
 * OstreeCollectionRefs as keys and only valid commit checksums as values. */
static gboolean
is_valid_collection_ref_map (GHashTable *ref_to_checksum)
{
  GHashTableIter iter;
  const OstreeCollectionRef *ref;
  const gchar *checksum;

  if (ref_to_checksum == NULL || g_hash_table_size (ref_to_checksum) == 0)
    return FALSE;

  g_hash_table_iter_init (&iter, ref_to_checksum);

  while (g_hash_table_iter_next (&iter, (gpointer *) &ref, (gpointer *) &checksum))
    {
      g_assert (ref != NULL);
      g_assert (checksum != NULL);

      if (!is_valid_collection_ref (ref))
        return FALSE;
      if (!ostree_validate_checksum_string (checksum, NULL))
        return FALSE;
    }

  return TRUE;
}

static void resolve_cb (GObject      *obj,
                        GAsyncResult *result,
                        gpointer      user_data);

/**
 * ostree_repo_finder_resolve_async:
 * @self: an #OstreeRepoFinder
 * @refs: (array zero-terminated=1): non-empty array of collection–ref pairs to find remotes for
 * @parent_repo: (transfer none): the local repository which the refs are being resolved for,
 *    which provides configuration information and GPG keys
 * @cancellable: (nullable): a #GCancellable, or %NULL
 * @callback: asynchronous completion callback
 * @user_data: data to pass to @callback
 *
 * Find reachable remote URIs which claim to provide any of the given @refs. The
 * specific method for finding the remotes depends on the #OstreeRepoFinder
 * implementation.
 *
 * Any remote which is found and which claims to support any of the given @refs
 * will be returned in the results. It is possible that a remote claims to
 * support a given ref, but turns out not to — it is not possible to verify this
 * until ostree_repo_pull_from_remotes_async() is called.
 *
 * The returned results will be sorted with the most useful first — this is
 * typically the remote which claims to provide the most @refs, at the lowest
 * latency.
 *
 * Each result contains a mapping of @refs to the checksums of the commits
 * which the result provides. If the result provides the latest commit for a ref
 * across all of the results, the checksum will be set. Otherwise, if the
 * result provides an outdated commit, or doesn’t provide a given ref at all,
 * the ref will not be set. Results which provide none of the requested @refs
 * may be listed with an empty refs map.
 *
 * Pass the results to ostree_repo_pull_from_remotes_async() to pull the given
 * @refs from those remotes.
 *
 * Since: 2017.8
 */
void
ostree_repo_finder_resolve_async (OstreeRepoFinder                  *self,
                                  const OstreeCollectionRef * const *refs,
                                  OstreeRepo                        *parent_repo,
                                  GCancellable                      *cancellable,
                                  GAsyncReadyCallback                callback,
                                  gpointer                           user_data)
{
  g_autoptr(GTask) task = NULL;
  OstreeRepoFinder *finders[2] = { NULL, };

  g_return_if_fail (OSTREE_IS_REPO_FINDER (self));
  g_return_if_fail (is_valid_collection_ref_array (refs));
  g_return_if_fail (OSTREE_IS_REPO (parent_repo));
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, ostree_repo_finder_resolve_async);

  finders[0] = self;

  ostree_repo_finder_resolve_all_async (finders, refs, parent_repo, cancellable,
                                        resolve_cb, g_steal_pointer (&task));
}

static void
resolve_cb (GObject      *obj,
            GAsyncResult *result,
            gpointer      user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GPtrArray) results = NULL;
  g_autoptr(GError) local_error = NULL;

  task = G_TASK (user_data);

  results = ostree_repo_finder_resolve_all_finish (result, &local_error);

  g_assert ((local_error == NULL) != (results == NULL));

  if (local_error != NULL)
    g_task_return_error (task, g_steal_pointer (&local_error));
  else
    g_task_return_pointer (task, g_steal_pointer (&results), (GDestroyNotify) g_ptr_array_unref);
}

/**
 * ostree_repo_finder_resolve_finish:
 * @self: an #OstreeRepoFinder
 * @result: #GAsyncResult from the callback
 * @error: return location for a #GError
 *
 * Get the results from a ostree_repo_finder_resolve_async() operation.
 *
 * Returns: (transfer full) (element-type OstreeRepoFinderResult): array of zero
 *    or more results
 * Since: 2017.8
 */
GPtrArray *
ostree_repo_finder_resolve_finish (OstreeRepoFinder  *self,
                                   GAsyncResult      *result,
                                   GError           **error)
{
  g_return_val_if_fail (OSTREE_IS_REPO_FINDER (self), NULL);
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static gint
sort_results_cb (gconstpointer a,
                 gconstpointer b)
{
  const OstreeRepoFinderResult *result_a = *((const OstreeRepoFinderResult **) a);
  const OstreeRepoFinderResult *result_b = *((const OstreeRepoFinderResult **) b);

  return ostree_repo_finder_result_compare (result_a, result_b);
}

typedef struct
{
  gsize n_finders_pending;
  GPtrArray *results;
} ResolveAllData;

static void
resolve_all_data_free (ResolveAllData *data)
{
  g_assert (data->n_finders_pending == 0);
  g_clear_pointer (&data->results, g_ptr_array_unref);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ResolveAllData, resolve_all_data_free)

static void resolve_all_cb (GObject      *obj,
                            GAsyncResult *result,
                            gpointer      user_data);
static void resolve_all_finished_one (GTask *task);

/**
 * ostree_repo_finder_resolve_all_async:
 * @finders: (array zero-terminated=1): non-empty array of #OstreeRepoFinders
 * @refs: (array zero-terminated=1): non-empty array of collection–ref pairs to find remotes for
 * @parent_repo: (transfer none): the local repository which the refs are being resolved for,
 *    which provides configuration information and GPG keys
 * @cancellable: (nullable): a #GCancellable, or %NULL
 * @callback: asynchronous completion callback
 * @user_data: data to pass to @callback
 *
 * A version of ostree_repo_finder_resolve_async() which queries one or more
 * @finders in parallel and combines the results.
 *
 * Since: 2017.8
 */
void
ostree_repo_finder_resolve_all_async (OstreeRepoFinder * const          *finders,
                                      const OstreeCollectionRef * const *refs,
                                      OstreeRepo                        *parent_repo,
                                      GCancellable                      *cancellable,
                                      GAsyncReadyCallback                callback,
                                      gpointer                           user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(ResolveAllData) data = NULL;
  gsize i;
  g_autoptr(GString) refs_str = NULL;
  g_autoptr(GString) finders_str = NULL;

  g_return_if_fail (finders != NULL && finders[0] != NULL);
  g_return_if_fail (is_valid_collection_ref_array (refs));
  g_return_if_fail (OSTREE_IS_REPO (parent_repo));
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

  refs_str = g_string_new ("");
  for (i = 0; refs[i] != NULL; i++)
    {
      if (i != 0)
        g_string_append (refs_str, ", ");
      g_string_append_printf (refs_str, "(%s, %s)",
                              refs[i]->collection_id, refs[i]->ref_name);
    }

  finders_str = g_string_new ("");
  for (i = 0; finders[i] != NULL; i++)
    {
      if (i != 0)
        g_string_append (finders_str, ", ");
      g_string_append (finders_str, g_type_name (G_TYPE_FROM_INSTANCE (finders[i])));
    }

  g_debug ("%s: Resolving refs [%s] with finders [%s]", G_STRFUNC,
           refs_str->str, finders_str->str);

  task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, ostree_repo_finder_resolve_all_async);

  data = g_new0 (ResolveAllData, 1);
  data->n_finders_pending = 1;  /* while setting up the loop */
  data->results = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_repo_finder_result_free);
  g_task_set_task_data (task, data, (GDestroyNotify) resolve_all_data_free);

  /* Start all the asynchronous queries in parallel. */
  for (i = 0; finders[i] != NULL; i++)
    {
      OstreeRepoFinder *finder = OSTREE_REPO_FINDER (finders[i]);
      OstreeRepoFinderInterface *iface;

      iface = OSTREE_REPO_FINDER_GET_IFACE (finder);
      g_assert (iface->resolve_async != NULL);
      iface->resolve_async (finder, refs, parent_repo, cancellable, resolve_all_cb, g_object_ref (task));
      data->n_finders_pending++;
    }

  resolve_all_finished_one (task);
  data = NULL;  /* passed to the GTask above */
}

/* Modifies both arrays in place. */
static void
array_concatenate_steal (GPtrArray *array,
                         GPtrArray *to_concatenate)  /* (transfer full) */
{
  g_autoptr(GPtrArray) array_to_concatenate = to_concatenate;
  gsize i;

  for (i = 0; i < array_to_concatenate->len; i++)
    {
      /* Sanity check that the arrays do not contain any %NULL elements
       * (particularly NULL terminators). */
      g_assert (g_ptr_array_index (array_to_concatenate, i) != NULL);
      g_ptr_array_add (array, g_steal_pointer (&g_ptr_array_index (array_to_concatenate, i)));
    }

  g_ptr_array_set_free_func (array_to_concatenate, NULL);
  g_ptr_array_set_size (array_to_concatenate, 0);
}

static void
resolve_all_cb (GObject      *obj,
                GAsyncResult *result,
                gpointer      user_data)
{
  OstreeRepoFinder *finder;
  OstreeRepoFinderInterface *iface;
  g_autoptr(GTask) task = NULL;
  g_autoptr(GPtrArray) results = NULL;
  g_autoptr(GError) local_error = NULL;
  ResolveAllData *data;

  finder = OSTREE_REPO_FINDER (obj);
  iface = OSTREE_REPO_FINDER_GET_IFACE (finder);
  task = G_TASK (user_data);
  data = g_task_get_task_data (task);
  results = iface->resolve_finish (finder, result, &local_error);

  g_assert ((local_error == NULL) != (results == NULL));

  if (local_error != NULL)
    g_debug ("Error resolving refs to repository URI using %s: %s",
             g_type_name (G_TYPE_FROM_INSTANCE (finder)), local_error->message);
  else
    array_concatenate_steal (data->results, g_steal_pointer (&results));

  resolve_all_finished_one (task);
}

static void
resolve_all_finished_one (GTask *task)
{
  ResolveAllData *data;

  data = g_task_get_task_data (task);

  data->n_finders_pending--;

  if (data->n_finders_pending == 0)
    {
      gsize i;
      g_autoptr(GString) results_str = NULL;

      g_ptr_array_sort (data->results, sort_results_cb);

      results_str = g_string_new ("");
      for (i = 0; i < data->results->len; i++)
        {
          const OstreeRepoFinderResult *result = g_ptr_array_index (data->results, i);

          if (i != 0)
            g_string_append (results_str, ", ");
          g_string_append (results_str, ostree_remote_get_name (result->remote));
        }
      if (i == 0)
        g_string_append (results_str, "(none)");

      g_debug ("%s: Finished, results: %s", G_STRFUNC, results_str->str);

      g_task_return_pointer (task, g_steal_pointer (&data->results), (GDestroyNotify) g_ptr_array_unref);
    }
}

/**
 * ostree_repo_finder_resolve_all_finish:
 * @result: #GAsyncResult from the callback
 * @error: return location for a #GError
 *
 * Get the results from a ostree_repo_finder_resolve_all_async() operation.
 *
 * Returns: (transfer full) (element-type OstreeRepoFinderResult): array of zero
 *    or more results
 * Since: 2017.8
 */
GPtrArray *
ostree_repo_finder_resolve_all_finish (GAsyncResult  *result,
                                       GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

G_DEFINE_BOXED_TYPE (OstreeRepoFinderResult, ostree_repo_finder_result,
                     ostree_repo_finder_result_dup, ostree_repo_finder_result_free)

/**
 * ostree_repo_finder_result_new:
 * @remote: (transfer none): an #OstreeRemote containing the transport details
 *    for the result
 * @finder: (transfer none): the #OstreeRepoFinder instance which produced the
 *    result
 * @priority: static priority of the result, where higher numbers indicate lower
 *    priority
 * @ref_to_checksum: (element-type OstreeCollectionRef utf8): map of collection–ref pairs
 *    to checksums provided by this result
 * @summary_last_modified: Unix timestamp (seconds since the epoch, UTC) when
 *    the summary file for the result was last modified, or `0` if this is unknown
 *
 * Create a new #OstreeRepoFinderResult instance. The semantics for the arguments
 * are as described in the #OstreeRepoFinderResult documentation.
 *
 * Returns: (transfer full): a new #OstreeRepoFinderResult
 * Since: 2017.8
 */
OstreeRepoFinderResult *
ostree_repo_finder_result_new (OstreeRemote     *remote,
                               OstreeRepoFinder *finder,
                               gint              priority,
                               GHashTable       *ref_to_checksum,
                               guint64           summary_last_modified)
{
  g_autoptr(OstreeRepoFinderResult) result = NULL;

  g_return_val_if_fail (remote != NULL, NULL);
  g_return_val_if_fail (OSTREE_IS_REPO_FINDER (finder), NULL);
  g_return_val_if_fail (is_valid_collection_ref_map (ref_to_checksum), NULL);

  result = g_new0 (OstreeRepoFinderResult, 1);
  result->remote = ostree_remote_ref (remote);
  result->finder = g_object_ref (finder);
  result->priority = priority;
  result->ref_to_checksum = g_hash_table_ref (ref_to_checksum);
  result->summary_last_modified = summary_last_modified;

  return g_steal_pointer (&result);
}

/**
 * ostree_repo_finder_result_dup:
 * @result: (transfer none): an #OstreeRepoFinderResult to copy
 *
 * Copy an #OstreeRepoFinderResult.
 *
 * Returns: (transfer full): a newly allocated copy of @result
 * Since: 2017.8
 */
OstreeRepoFinderResult *
ostree_repo_finder_result_dup (OstreeRepoFinderResult *result)
{
  g_return_val_if_fail (result != NULL, NULL);

  return ostree_repo_finder_result_new (result->remote, result->finder,
                                        result->priority, result->ref_to_checksum,
                                        result->summary_last_modified);
}

/**
 * ostree_repo_finder_result_compare:
 * @a: an #OstreeRepoFinderResult
 * @b: an #OstreeRepoFinderResult
 *
 * Compare two #OstreeRepoFinderResult instances to work out which one is better
 * to pull from, and hence needs to be ordered before the other.
 *
 * Returns: <0 if @a is ordered before @b, 0 if they are ordered equally,
 *    >0 if @b is ordered before @a
 * Since: 2017.8
 */
gint
ostree_repo_finder_result_compare (const OstreeRepoFinderResult *a,
                                   const OstreeRepoFinderResult *b)
{
  guint a_n_refs, b_n_refs;

  g_return_val_if_fail (a != NULL, 0);
  g_return_val_if_fail (b != NULL, 0);

  /* FIXME: Check if this is really the ordering we want. For example, we
   * probably don’t want a result with 0 refs to be ordered before one with >0
   * refs, just because its priority is higher. */
  if (a->priority != b->priority)
    return a->priority - b->priority;

  if (a->summary_last_modified != 0 && b->summary_last_modified != 0 &&
      a->summary_last_modified != b->summary_last_modified)
    return a->summary_last_modified - b->summary_last_modified;

  gpointer value;
  GHashTableIter iter;
  a_n_refs = b_n_refs = 0;

  g_hash_table_iter_init (&iter, a->ref_to_checksum);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    if (value != NULL)
      a_n_refs++;

  g_hash_table_iter_init (&iter, b->ref_to_checksum);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    if (value != NULL)
      b_n_refs++;

  if (a_n_refs != b_n_refs)
    return (gint) a_n_refs - (gint) b_n_refs;

  return g_strcmp0 (a->remote->name, b->remote->name);
}

/**
 * ostree_repo_finder_result_free:
 * @result: (transfer full): an #OstreeRepoFinderResult
 *
 * Free the given @result.
 *
 * Since: 2017.8
 */
void
ostree_repo_finder_result_free (OstreeRepoFinderResult *result)
{
  g_return_if_fail (result != NULL);

  /* This may be NULL iff the result is freed half-way through find_remotes_cb()
   * in ostree-repo-pull.c, and at no other time. */
  g_clear_pointer (&result->ref_to_checksum, g_hash_table_unref);
  g_object_unref (result->finder);
  ostree_remote_unref (result->remote);
  g_free (result);
}

/**
 * ostree_repo_finder_result_freev:
 * @results: (array zero-terminated=1) (transfer full): an #OstreeRepoFinderResult
 *
 * Free the given @results array, freeing each element and the container.
 *
 * Since: 2017.8
 */
void
ostree_repo_finder_result_freev (OstreeRepoFinderResult **results)
{
  gsize i;

  for (i = 0; results[i] != NULL; i++)
    ostree_repo_finder_result_free (results[i]);

  g_free (results);
}
