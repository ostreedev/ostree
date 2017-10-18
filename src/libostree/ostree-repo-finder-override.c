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

#include "ostree-autocleanups.h"
#include "ostree-remote-private.h"
#include "ostree-repo.h"
#include "ostree-repo-private.h"
#include "ostree-repo-finder.h"
#include "ostree-repo-finder-override.h"

/**
 * SECTION:ostree-repo-finder-override
 * @title: OstreeRepoFinderOverride
 * @short_description: Finds remote repositories from a list of repository URIs
 * @stability: Unstable
 * @include: libostree/ostree-repo-finder-override.h
 *
 * #OstreeRepoFinderOverride is an implementation of #OstreeRepoFinder which
 * looks refs up in a list of remotes given by their URI, and returns the URIs
 * which contain the refs. Duplicate remote URIs are combined into a single
 * #OstreeRepoFinderResult which lists multiple refs.
 *
 * Each result is given an #OstreeRepoFinderResult.priority value of 20, which
 * ranks its results above those from the other default #OstreeRepoFinder
 * implementations.
 *
 * Results can only be returned for a ref if a remote and keyring are configured
 * locally for the collection ID of that ref, otherwise there would be no keys
 * available to verify signatures on commits for that ref.
 *
 * This is intended to be used for user-provided overrides and testing software
 * which uses #OstreeRepoFinder. For production use, #OstreeRepoFinderConfig is
 * recommended instead.
 *
 * Since: 2017.13
 */

static void ostree_repo_finder_override_iface_init (OstreeRepoFinderInterface *iface);

struct _OstreeRepoFinderOverride
{
  GObject parent_instance;

  GPtrArray *override_uris;  /* (owned) (element-type utf8) */
};

G_DEFINE_TYPE_WITH_CODE (OstreeRepoFinderOverride, ostree_repo_finder_override, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_REPO_FINDER, ostree_repo_finder_override_iface_init))

static gint
results_compare_cb (gconstpointer a,
                    gconstpointer b)
{
  const OstreeRepoFinderResult *result_a = *((const OstreeRepoFinderResult **) a);
  const OstreeRepoFinderResult *result_b = *((const OstreeRepoFinderResult **) b);

  return ostree_repo_finder_result_compare (result_a, result_b);
}

/* This must return a valid remote name (suitable for use in a refspec). */
static gchar *
uri_and_keyring_to_name (const gchar *uri,
                         const gchar *keyring)
{
  g_autofree gchar *escaped_uri = g_uri_escape_string (uri, NULL, FALSE);
  g_autofree gchar *escaped_keyring = g_uri_escape_string (keyring, NULL, FALSE);

  /* FIXME: Need a better separator than `_`, since it’s not escaped in the input. */
  g_autofree gchar *out = g_strdup_printf ("%s_%s", escaped_uri, escaped_keyring);

  for (gsize i = 0; out[i] != '\0'; i++)
    {
      if (out[i] == '%')
        out[i] = '_';
    }

  g_return_val_if_fail (ostree_validate_remote_name (out, NULL), NULL);

  return g_steal_pointer (&out);
}

/* Version of ostree_repo_remote_list_collection_refs() which takes an
 * #OstreeRemote. */
static gboolean
repo_remote_list_collection_refs (OstreeRepo    *repo,
                                  const gchar   *remote_uri,
                                  GHashTable   **out_all_refs,
                                  GCancellable  *cancellable,
                                  GError       **error)
{
  g_autofree gchar *name = uri_and_keyring_to_name (remote_uri, "");
  g_autoptr(OstreeRemote) remote = ostree_remote_new (name);
  g_key_file_set_string (remote->options, remote->group, "url", remote_uri);

  gboolean remote_already_existed = _ostree_repo_add_remote (repo, remote);
  gboolean success = ostree_repo_remote_list_collection_refs (repo,
                                                              remote->name,
                                                              out_all_refs,
                                                              cancellable,
                                                              error);

  if (!remote_already_existed)
    _ostree_repo_remove_remote (repo, remote);

  return success;
}

static void
ostree_repo_finder_override_resolve_async (OstreeRepoFinder                  *finder,
                                           const OstreeCollectionRef * const *refs,
                                           OstreeRepo                        *parent_repo,
                                           GCancellable                      *cancellable,
                                           GAsyncReadyCallback                callback,
                                           gpointer                           user_data)
{
  OstreeRepoFinderOverride *self = OSTREE_REPO_FINDER_OVERRIDE (finder);
  g_autoptr(GTask) task = NULL;
  g_autoptr(GPtrArray) results = NULL;
  const gint priority = 20;  /* arbitrarily chosen; higher priority than the others */
  gsize i, j;
  g_autoptr(GHashTable) repo_remote_to_refs = NULL;  /* (element-type OstreeRemote GHashTable) */
  GHashTable *supported_ref_to_checksum;  /* (element-type OstreeCollectionRef utf8) */
  GHashTableIter iter;
  const gchar *remote_uri;
  OstreeRemote *remote;

  task = g_task_new (finder, cancellable, callback, user_data);
  g_task_set_source_tag (task, ostree_repo_finder_override_resolve_async);
  results = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_repo_finder_result_free);
  repo_remote_to_refs = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                               (GDestroyNotify) ostree_remote_unref,
                                               (GDestroyNotify) g_hash_table_unref);

  g_debug ("%s: Checking %u overrides", G_STRFUNC, self->override_uris->len);

  for (i = 0; i < self->override_uris->len; i++)
    {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GHashTable) remote_refs = NULL;  /* (element-type OstreeCollectionRef utf8) */
      const gchar *checksum;
      gboolean resolved_a_ref = FALSE;

      remote_uri = self->override_uris->pdata[i];

      if (!repo_remote_list_collection_refs (parent_repo, remote_uri,
                                             &remote_refs, cancellable,
                                             &local_error))
        {
          g_debug ("Ignoring remote ‘%s’ due to error loading its refs: %s",
                   remote_uri, local_error->message);
          g_clear_error (&local_error);
          continue;
        }

      for (j = 0; refs[j] != NULL; j++)
        {
          g_autoptr(OstreeRemote) keyring_remote = NULL;

          /* Look up the GPG keyring for this ref. */
          keyring_remote = ostree_repo_resolve_keyring_for_collection (parent_repo,
                                                                       refs[j]->collection_id,
                                                                       cancellable, &local_error);

          if (keyring_remote == NULL)
            {
              g_debug ("Ignoring ref (%s, %s) due to missing keyring: %s",
                       refs[j]->collection_id, refs[j]->ref_name, local_error->message);
              g_clear_error (&local_error);
              continue;
            }

          if (g_hash_table_lookup_extended (remote_refs, refs[j], NULL, (gpointer *) &checksum))
            {
              g_autoptr(OstreeRemote) remote = NULL;

              /* The requested ref is listed in the refs for this remote. Add
               * the remote to the results, and the ref to its
               * @supported_ref_to_checksum. */
              g_debug ("Resolved ref (%s, %s) to remote ‘%s’.",
                       refs[j]->collection_id, refs[j]->ref_name, remote_uri);
              resolved_a_ref = TRUE;

              /* Build an #OstreeRemote. Use the escaped URI, since remote->name
               * is used in file paths, so needs to not contain special characters. */
              g_autofree gchar *name = uri_and_keyring_to_name (remote_uri, keyring_remote->name);
              remote = ostree_remote_new_dynamic (name, keyring_remote->name);

              /* gpg-verify-summary is false since we use the unsigned summary file support. */
              g_key_file_set_string (remote->options, remote->group, "url", remote_uri);
              g_key_file_set_boolean (remote->options, remote->group, "gpg-verify", TRUE);
              g_key_file_set_boolean (remote->options, remote->group, "gpg-verify-summary", FALSE);

              supported_ref_to_checksum = g_hash_table_lookup (repo_remote_to_refs, remote);

              if (supported_ref_to_checksum == NULL)
                {
                  supported_ref_to_checksum = g_hash_table_new_full (ostree_collection_ref_hash,
                                                                     ostree_collection_ref_equal,
                                                                     NULL, g_free);
                  g_hash_table_insert (repo_remote_to_refs, ostree_remote_ref (remote), supported_ref_to_checksum  /* transfer */);
                }

              g_hash_table_insert (supported_ref_to_checksum,
                                   (gpointer) refs[j], g_strdup (checksum));
            }
        }

      if (!resolved_a_ref)
        g_debug ("Ignoring remote ‘%s’ due to it not advertising any of the requested refs.",
                 remote_uri);
    }

  /* Aggregate the results. */
  g_hash_table_iter_init (&iter, repo_remote_to_refs);

  while (g_hash_table_iter_next (&iter, (gpointer *) &remote, (gpointer *) &supported_ref_to_checksum))
    g_ptr_array_add (results, ostree_repo_finder_result_new (remote, finder, priority, supported_ref_to_checksum, 0));

  g_ptr_array_sort (results, results_compare_cb);

  g_task_return_pointer (task, g_steal_pointer (&results), (GDestroyNotify) g_ptr_array_unref);
}

static GPtrArray *
ostree_repo_finder_override_resolve_finish (OstreeRepoFinder  *finder,
                                            GAsyncResult      *result,
                                            GError           **error)
{
  g_return_val_if_fail (g_task_is_valid (result, finder), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
ostree_repo_finder_override_init (OstreeRepoFinderOverride *self)
{
  self->override_uris = g_ptr_array_new_with_free_func ((GDestroyNotify) g_free);
}

static void
ostree_repo_finder_override_finalize (GObject *object)
{
  OstreeRepoFinderOverride *self = OSTREE_REPO_FINDER_OVERRIDE (object);

  g_clear_pointer (&self->override_uris, g_ptr_array_unref);

  G_OBJECT_CLASS (ostree_repo_finder_override_parent_class)->finalize (object);
}

static void
ostree_repo_finder_override_class_init (OstreeRepoFinderOverrideClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = ostree_repo_finder_override_finalize;
}

static void
ostree_repo_finder_override_iface_init (OstreeRepoFinderInterface *iface)
{
  iface->resolve_async = ostree_repo_finder_override_resolve_async;
  iface->resolve_finish = ostree_repo_finder_override_resolve_finish;
}

/**
 * ostree_repo_finder_override_new:
 *
 * Create a new #OstreeRepoFinderOverride.
 *
 * Returns: (transfer full): a new #OstreeRepoFinderOverride
 * Since: 2017.13
 */
OstreeRepoFinderOverride *
ostree_repo_finder_override_new (void)
{
  return g_object_new (OSTREE_TYPE_REPO_FINDER_OVERRIDE, NULL);
}

/**
 * ostree_repo_finder_override_add_uri:
 * @uri: URI to add to the repo finder
 *
 * Add the given @uri to the set of URIs which the repo finder will search for
 * matching refs when ostree_repo_finder_resolve_async() is called on it.
 *
 * Since: 2017.13
 */
void
ostree_repo_finder_override_add_uri (OstreeRepoFinderOverride *self,
                                     const gchar              *uri)
{
  g_return_if_fail (OSTREE_IS_REPO_FINDER_OVERRIDE (self));
  g_return_if_fail (uri != NULL);

  g_ptr_array_add (self->override_uris, g_strdup (uri));
}
