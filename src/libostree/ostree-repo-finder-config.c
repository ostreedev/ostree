/*
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

#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <libglnx.h>

#include "ostree-remote-private.h"
#include "ostree-repo.h"
#include "ostree-repo-private.h"
#include "ostree-repo-finder.h"
#include "ostree-repo-finder-config.h"

/**
 * SECTION:ostree-repo-finder-config
 * @title: OstreeRepoFinderConfig
 * @short_description: Finds remote repositories from ref names using the local
 *    repository configuration files
 * @stability: Unstable
 * @include: libostree/ostree-repo-finder-config.h
 *
 * #OstreeRepoFinderConfig is an implementation of #OstreeRepoFinder which looks
 * refs up in locally configured remotes and returns remote URIs.
 * Duplicate remote URIs are combined into a single #OstreeRepoFinderResult
 * which lists multiple refs.
 *
 * For all the locally configured remotes which have an `collection-id` specified
 * (see [ostree.repo-config(5)](man:ostree.repo-config(5))), it finds the
 * intersection of their refs and the set of refs to resolve. If the
 * intersection is non-empty, that remote is returned as a result. Remotes which
 * do not have their `collection-id` key configured are ignored.
 *
 * Since: 2017.8
 */

static void ostree_repo_finder_config_iface_init (OstreeRepoFinderInterface *iface);

struct _OstreeRepoFinderConfig
{
  GObject parent_instance;
};

G_DEFINE_TYPE_WITH_CODE (OstreeRepoFinderConfig, ostree_repo_finder_config, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_REPO_FINDER, ostree_repo_finder_config_iface_init))

static gint
results_compare_cb (gconstpointer a,
                    gconstpointer b)
{
  const OstreeRepoFinderResult *result_a = *((const OstreeRepoFinderResult **) a);
  const OstreeRepoFinderResult *result_b = *((const OstreeRepoFinderResult **) b);

  return ostree_repo_finder_result_compare (result_a, result_b);
}

static void
ostree_repo_finder_config_resolve_async (OstreeRepoFinder                  *finder,
                                         const OstreeCollectionRef * const *refs,
                                         OstreeRepo                        *parent_repo,
                                         GCancellable                      *cancellable,
                                         GAsyncReadyCallback                callback,
                                         gpointer                           user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GPtrArray) results = NULL;
  const gint priority = 100;  /* arbitrarily chosen; lower than the others */
  gsize i, j;
  g_autoptr(GHashTable) repo_name_to_refs = NULL;  /* (element-type utf8 GHashTable) */
  GHashTable *supported_ref_to_checksum;  /* (element-type OstreeCollectionRef utf8) */
  GHashTableIter iter;
  const gchar *remote_name;
  g_auto(GStrv) remotes = NULL;
  gsize n_remotes = 0;

  task = g_task_new (finder, cancellable, callback, user_data);
  g_task_set_source_tag (task, ostree_repo_finder_config_resolve_async);
  results = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_repo_finder_result_free);
  repo_name_to_refs = g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
                                             (GDestroyNotify) g_hash_table_unref);

  /* List all remotes in this #OstreeRepo and see which of their ref lists
   * intersect with @refs. */
  remotes = ostree_repo_remote_list (parent_repo, (guint *) &n_remotes);

  g_debug ("%s: Checking %" G_GSIZE_FORMAT " remotes", G_STRFUNC, n_remotes);

  for (i = 0; i < n_remotes; i++)
    {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GHashTable) remote_refs = NULL;  /* (element-type OstreeCollectionRef utf8) */
      const gchar *checksum;
      g_autofree gchar *remote_collection_id = NULL;
      gboolean resolved_a_ref = FALSE;

      remote_name = remotes[i];

      if (!ostree_repo_get_remote_option (parent_repo, remote_name, "collection-id",
                                          NULL, &remote_collection_id, &local_error) ||
          !ostree_validate_collection_id (remote_collection_id, &local_error))
        {
          g_debug ("Ignoring remote ‘%s’ due to no valid collection ID being configured for it: %s",
                   remote_name, local_error->message);
          g_clear_error (&local_error);
          continue;
        }

      if (!ostree_repo_remote_list_collection_refs (parent_repo, remote_name,
                                                    &remote_refs, cancellable,
                                                    &local_error))
        {
          g_debug ("Ignoring remote ‘%s’ due to error loading its refs: %s",
                   remote_name, local_error->message);
          g_clear_error (&local_error);
          continue;
        }

      for (j = 0; refs[j] != NULL; j++)
        {
          if (g_strcmp0 (refs[j]->collection_id, remote_collection_id) == 0 &&
              g_hash_table_lookup_extended (remote_refs, refs[j], NULL, (gpointer *) &checksum))
            {
              /* The requested ref is listed in the refs for this remote. Add
               * the remote to the results, and the ref to its
               * @supported_ref_to_checksum. */
              g_debug ("Resolved ref (%s, %s) to remote ‘%s’.",
                       refs[j]->collection_id, refs[j]->ref_name, remote_name);
              resolved_a_ref = TRUE;

              supported_ref_to_checksum = g_hash_table_lookup (repo_name_to_refs, remote_name);

              if (supported_ref_to_checksum == NULL)
                {
                  supported_ref_to_checksum = g_hash_table_new_full (ostree_collection_ref_hash,
                                                                     ostree_collection_ref_equal,
                                                                     NULL, g_free);
                  g_hash_table_insert (repo_name_to_refs, (gpointer) remote_name, supported_ref_to_checksum  /* transfer */);
                }

              g_hash_table_insert (supported_ref_to_checksum,
                                   (gpointer) refs[j], g_strdup (checksum));
            }
        }

      if (!resolved_a_ref)
        g_debug ("Ignoring remote ‘%s’ due to it not advertising any of the requested refs.", remote_name);
    }

  /* Aggregate the results. */
  g_hash_table_iter_init (&iter, repo_name_to_refs);

  while (g_hash_table_iter_next (&iter, (gpointer *) &remote_name, (gpointer *) &supported_ref_to_checksum))
    {
      g_autoptr(GError) local_error = NULL;
      OstreeRemote *remote;

      /* We don’t know what last-modified timestamp the remote has without
       * making expensive HTTP queries, so leave that information blank. We
       * assume that the configuration which says the refs and commits in
       * @supported_ref_to_checksum are in the repository is correct; the code
       * in ostree_repo_find_remotes_async() will check that. */
      remote = _ostree_repo_get_remote_inherited (parent_repo, remote_name, &local_error);
      if (remote == NULL)
        {
          g_debug ("Configuration for remote ‘%s’ could not be found. Ignoring.",
                   remote_name);
          continue;
        }

      g_ptr_array_add (results, ostree_repo_finder_result_new (remote, finder, priority, supported_ref_to_checksum, 0));
    }

  g_ptr_array_sort (results, results_compare_cb);

  g_task_return_pointer (task, g_steal_pointer (&results), (GDestroyNotify) g_ptr_array_unref);
}

static GPtrArray *
ostree_repo_finder_config_resolve_finish (OstreeRepoFinder  *finder,
                                          GAsyncResult      *result,
                                          GError           **error)
{
  g_return_val_if_fail (g_task_is_valid (result, finder), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
ostree_repo_finder_config_init (OstreeRepoFinderConfig *self)
{
  /* Nothing to see here. */
}

static void
ostree_repo_finder_config_class_init (OstreeRepoFinderConfigClass *klass)
{
  /* Nothing to see here. */
}

static void
ostree_repo_finder_config_iface_init (OstreeRepoFinderInterface *iface)
{
  iface->resolve_async = ostree_repo_finder_config_resolve_async;
  iface->resolve_finish = ostree_repo_finder_config_resolve_finish;
}

/**
 * ostree_repo_finder_config_new:
 *
 * Create a new #OstreeRepoFinderConfig.
 *
 * Returns: (transfer full): a new #OstreeRepoFinderConfig
 * Since: 2017.8
 */
OstreeRepoFinderConfig *
ostree_repo_finder_config_new (void)
{
  return g_object_new (OSTREE_TYPE_REPO_FINDER_CONFIG, NULL);
}
