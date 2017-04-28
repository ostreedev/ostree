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
#include <libglnx.h>
#include <stdlib.h>

#include "ostree-autocleanups.h"
#include "ostree-remote-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-finder.h"
#include "ostree-repo-finder-mount.h"

/**
 * SECTION:ostree-repo-finder-mount
 * @title: OstreeRepoFinderMount
 * @short_description: Finds remote repositories from ref names by looking at
 *    mounted removable volumes
 * @stability: Unstable
 * @include: libostree/ostree-repo-finder-mount.h
 *
 * #OstreeRepoFinderMount is an implementation of #OstreeRepoFinder which looks
 * refs up in well-known locations on any mounted removable volumes.
 *
 * For an #OstreeCollectionRef, (`C`, `R`), it checks whether `.ostree/repos/C/R`
 * exists and is an OSTree repository on each mounted removable volume. Collection
 * IDs and ref names are not escaped when building the path, so if either
 * contains `/` in its name, the repository will be checked for in a
 * subdirectory of `.ostree/repos`. Non-removable volumes are ignored.
 *
 * For each repository which is found, a result will be returned for the
 * intersection of the refs being searched for, and the refs in `refs/heads` and
 * `refs/mirrors` in the repository on the removable volume.
 *
 * Symlinks are followed when resolving the refs, so a volume might contain a
 * single OSTree at some arbitrary path, with a number of refs linking to it
 * from `.ostree/repos`. Any symlink which points outside the volume’s file
 * system will be ignored. Repositories are deduplicated in the results.
 *
 * The volume monitor used to find mounted volumes can be overridden by setting
 * #OstreeRepoFinderMount:monitor. By default, g_volume_monitor_get() is used.
 *
 * Since: 2017.8
 */

typedef GList/*<owned GObject>*/ ObjectList;

static void
object_list_free (ObjectList *list)
{
  g_list_free_full (list, g_object_unref);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ObjectList, object_list_free)

static void ostree_repo_finder_mount_iface_init (OstreeRepoFinderInterface *iface);

struct _OstreeRepoFinderMount
{
  GObject parent_instance;

  GVolumeMonitor *monitor;  /* owned */
};

G_DEFINE_TYPE_WITH_CODE (OstreeRepoFinderMount, ostree_repo_finder_mount, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_REPO_FINDER, ostree_repo_finder_mount_iface_init))

typedef struct
{
  gchar *uri;
  gchar *keyring;
} UriAndKeyring;

static void
uri_and_keyring_free (UriAndKeyring *data)
{
  g_free (data->uri);
  g_free (data->keyring);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (UriAndKeyring, uri_and_keyring_free)

static UriAndKeyring *
uri_and_keyring_new (const gchar *uri,
                     const gchar *keyring)
{
  g_autoptr(UriAndKeyring) data = NULL;

  data = g_new0 (UriAndKeyring, 1);
  data->uri = g_strdup (uri);
  data->keyring = g_strdup (keyring);

  return g_steal_pointer (&data);
}

static guint
uri_and_keyring_hash (gconstpointer key)
{
  const UriAndKeyring *_key = key;

  return g_str_hash (_key->uri) ^ g_str_hash (_key->keyring);
}

static gboolean
uri_and_keyring_equal (gconstpointer a,
                       gconstpointer b)
{
  const UriAndKeyring *_a = a, *_b = b;

  return g_str_equal (_a->uri, _b->uri) && g_str_equal (_a->keyring, _b->keyring);
}

/* This must return a valid remote name (suitable for use in a refspec). */
static gchar *
uri_and_keyring_to_name (UriAndKeyring *data)
{
  g_autofree gchar *escaped_uri = g_uri_escape_string (data->uri, NULL, FALSE);
  g_autofree gchar *escaped_keyring = g_uri_escape_string (data->keyring, NULL, FALSE);

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

static gint
results_compare_cb (gconstpointer a,
                    gconstpointer b)
{
  const OstreeRepoFinderResult *result_a = *((const OstreeRepoFinderResult **) a);
  const OstreeRepoFinderResult *result_b = *((const OstreeRepoFinderResult **) b);

  return ostree_repo_finder_result_compare (result_a, result_b);
}

static void
ostree_repo_finder_mount_resolve_async (OstreeRepoFinder                  *finder,
                                        const OstreeCollectionRef * const *refs,
                                        OstreeRepo                        *parent_repo,
                                        GCancellable                      *cancellable,
                                        GAsyncReadyCallback                callback,
                                        gpointer                           user_data)
{
  OstreeRepoFinderMount *self = OSTREE_REPO_FINDER_MOUNT (finder);
  g_autoptr(GTask) task = NULL;
  g_autoptr(ObjectList) mounts = NULL;
  g_autoptr(GPtrArray) results = NULL;  /* (element-type OstreeRepoFinderResult) */
  GList *l;
  const gint priority = 50;  /* arbitrarily chosen */

  task = g_task_new (finder, cancellable, callback, user_data);
  g_task_set_source_tag (task, ostree_repo_finder_mount_resolve_async);

  mounts = g_volume_monitor_get_mounts (self->monitor);
  results = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_repo_finder_result_free);

  g_debug ("%s: Found %u mounts", G_STRFUNC, g_list_length (mounts));

  for (l = mounts; l != NULL; l = l->next)
    {
      GMount *mount = G_MOUNT (l->data);
      g_autofree gchar *mount_name = NULL;
      g_autoptr(GFile) mount_root = NULL;
      g_autofree gchar *mount_root_path = NULL;
      glnx_fd_close int mount_root_dfd = -1;
      struct stat mount_root_stbuf;
      glnx_fd_close int repos_dfd = -1;
      gsize i;
      g_autoptr(GHashTable) repo_to_refs = NULL;  /* (element-type UriAndKeyring GHashTable) */
      GHashTable *supported_ref_to_checksum;  /* (element-type OstreeCollectionRef utf8) */
      GHashTableIter iter;
      UriAndKeyring *repo;
      g_autoptr(GError) local_error = NULL;

      mount_name = g_mount_get_name (mount);

      /* Check the mount’s general properties. */
      if (g_mount_is_shadowed (mount))
        {
          g_debug ("Ignoring mount ‘%s’ as it’s shadowed.", mount_name);
          continue;
        }

      /* Check if it contains a .ostree/repos directory. */
      mount_root = g_mount_get_root (mount);
      mount_root_path = g_file_get_path (mount_root);

      if (!glnx_opendirat (AT_FDCWD, mount_root_path, TRUE, &mount_root_dfd, &local_error))
        {
          g_debug ("Ignoring mount ‘%s’ as ‘%s’ directory can’t be opened: %s",
                   mount_name, mount_root_path, local_error->message);
          continue;
        }

      if (!glnx_opendirat (mount_root_dfd, ".ostree/repos", TRUE, &repos_dfd, &local_error))
        {
          if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            g_debug ("Ignoring mount ‘%s’ as ‘%s/.ostree/repos’ directory doesn’t exist.",
                     mount_name, mount_root_path);
          else
            g_debug ("Ignoring mount ‘%s’ as ‘%s/.ostree/repos’ directory can’t be opened: %s",
                     mount_name, mount_root_path, local_error->message);

          continue;
        }

      /* stat() the mount root so we can later check whether the resolved
       * repositories for individual refs are on the same device (to avoid the
       * symlinks for them pointing outside the mount root). */
      if (!glnx_fstat (mount_root_dfd, &mount_root_stbuf, &local_error))
        {
          g_debug ("Ignoring mount ‘%s’ as querying info of ‘%s’ failed: %s",
                   mount_name, mount_root_path, local_error->message);
          continue;
        }

      /* Check whether a subdirectory exists for any of the @refs we’re looking
       * for. If so, and it’s a symbolic link, dereference it so multiple links
       * to the same repository (containing multiple refs) are coalesced.
       * Otherwise, include it as a result by itself. */
      repo_to_refs = g_hash_table_new_full (uri_and_keyring_hash, uri_and_keyring_equal,
                                            (GDestroyNotify) uri_and_keyring_free, (GDestroyNotify) g_hash_table_unref);

      for (i = 0; refs[i] != NULL; i++)
        {
          struct stat stbuf;
          g_autofree gchar *collection_and_ref = NULL;
          g_autofree gchar *resolved_repo_uri = NULL;
          g_autofree gchar *keyring = NULL;
          g_autoptr(UriAndKeyring) resolved_repo = NULL;

          collection_and_ref = g_build_filename (refs[i]->collection_id, refs[i]->ref_name, NULL);

          if (!glnx_fstatat (repos_dfd, collection_and_ref, &stbuf, AT_NO_AUTOMOUNT, &local_error))
            {
              g_debug ("Ignoring ref (%s, %s) on mount ‘%s’ as querying info of ‘%s’ failed: %s",
                       refs[i]->collection_id, refs[i]->ref_name, mount_name, collection_and_ref, local_error->message);
              g_clear_error (&local_error);
              continue;
            }

          if ((stbuf.st_mode & S_IFMT) != S_IFDIR)
            {
              g_debug ("Ignoring ref (%s, %s) on mount ‘%s’ as ‘%s’ is of type %u, not a directory.",
                       refs[i]->collection_id, refs[i]->ref_name, mount_name, collection_and_ref, (stbuf.st_mode & S_IFMT));
              g_clear_error (&local_error);
              continue;
            }

          /* Check the resolved repository path is below the mount point. Do not
           * allow ref symlinks to point somewhere outside of the mounted
           * volume. */
          if (stbuf.st_dev != mount_root_stbuf.st_dev)
            {
              g_debug ("Ignoring ref (%s, %s) on mount ‘%s’ as it’s on a different file system from the mount.",
                       refs[i]->collection_id, refs[i]->ref_name, mount_name);
              g_clear_error (&local_error);
              continue;
            }

          /* Exclude repositories which resolve to @parent_repo. */
          if (stbuf.st_dev == parent_repo->device &&
              stbuf.st_ino == parent_repo->inode)
            {
              g_debug ("Ignoring ref (%s, %s) on mount ‘%s’ as it is the same as the one we are resolving",
                       refs[i]->collection_id, refs[i]->ref_name, mount_name);
              g_clear_error (&local_error);
              continue;
            }

          /* Grab the given ref and a checksum for it from the repo, if it appears to be a valid repo */
          g_autoptr(OstreeRepo) repo = ostree_repo_open_at (repos_dfd, collection_and_ref,
                                                            cancellable, &local_error);
          if (!repo)
            {
              g_debug ("Ignoring ref (%s, %s) on mount ‘%s’ as its repository could not be opened: %s",
                       refs[i]->collection_id, refs[i]->ref_name, mount_name, local_error->message);
              g_clear_error (&local_error);
              continue;
            }

          g_autoptr(GHashTable) repo_refs = NULL;  /* (element-type OstreeCollectionRef utf8) */

          if (!ostree_repo_list_collection_refs (repo, refs[i]->collection_id, &repo_refs, cancellable, &local_error))
            {
              g_debug ("Ignoring ref (%s, %s) on mount ‘%s’ as its refs could not be listed: %s",
                       refs[i]->collection_id, refs[i]->ref_name, mount_name, local_error->message);
              g_clear_error (&local_error);
              continue;
            }

          const gchar *checksum = g_hash_table_lookup (repo_refs, refs[i]);

          if (checksum == NULL)
            {
              g_debug ("Ignoring ref (%s, %s) on mount ‘%s’ as its repository doesn’t contain the ref.",
                       refs[i]->collection_id, refs[i]->ref_name, mount_name);
              g_clear_error (&local_error);
              continue;
            }

          /* Finally, look up the GPG keyring for this ref. */
          keyring = ostree_repo_resolve_keyring_for_collection (parent_repo, refs[i]->collection_id,
                                                                cancellable, &local_error);

          if (keyring == NULL)
            {
              g_debug ("Ignoring ref (%s, %s) on mount ‘%s’ due to missing keyring: %s",
                       refs[i]->collection_id, refs[i]->ref_name, mount_name, local_error->message);
              g_clear_error (&local_error);
              continue;
            }

          /* There is a valid repo at (or pointed to by)
           * $mount_root/.ostree/repos/$refs[i]->collection_id/$refs[i]->ref_name.
           * Add it to the results, keyed by the canonicalised repository URI
           * to deduplicate the results. */

          g_autofree char *repo_abspath = g_build_filename (mount_root_path, ".ostree/repos",
                                                            collection_and_ref, NULL);
          /* FIXME - why are we using realpath here? */
          g_autofree char *canonical_repo_dir_path = realpath (repo_abspath, NULL);
          resolved_repo_uri = g_strconcat ("file://", canonical_repo_dir_path, NULL);
          g_debug ("Resolved ref (%s, %s) on mount ‘%s’ to repo URI ‘%s’ with keyring ‘%s’.",
                   refs[i]->collection_id, refs[i]->ref_name, mount_name, resolved_repo_uri, keyring);

          resolved_repo = uri_and_keyring_new (resolved_repo_uri, keyring);

          supported_ref_to_checksum = g_hash_table_lookup (repo_to_refs, resolved_repo);

          if (supported_ref_to_checksum == NULL)
            {
              supported_ref_to_checksum = g_hash_table_new_full (ostree_collection_ref_hash,
                                                                 ostree_collection_ref_equal,
                                                                 NULL, g_free);
              g_hash_table_insert (repo_to_refs, g_steal_pointer (&resolved_repo), supported_ref_to_checksum  /* transfer */);
            }

          g_hash_table_insert (supported_ref_to_checksum, (gpointer) refs[i], g_strdup (checksum));
        }

      /* Aggregate the results. */
      g_hash_table_iter_init (&iter, repo_to_refs);

      while (g_hash_table_iter_next (&iter, (gpointer *) &repo, (gpointer *) &supported_ref_to_checksum))
        {
          g_autoptr(OstreeRemote) remote = NULL;

          /* Build an #OstreeRemote. Use the escaped URI, since remote->name
           * is used in file paths, so needs to not contain special characters. */
          g_autofree gchar *name = uri_and_keyring_to_name (repo);
          remote = ostree_remote_new (name);

          g_clear_pointer (&remote->keyring, g_free);
          remote->keyring = g_strdup (repo->keyring);

          /* gpg-verify-summary is false since we use the unsigned summary file support. */
          g_key_file_set_string (remote->options, remote->group, "url", repo->uri);
          g_key_file_set_boolean (remote->options, remote->group, "gpg-verify", TRUE);
          g_key_file_set_boolean (remote->options, remote->group, "gpg-verify-summary", FALSE);

          /* Set the timestamp in the #OstreeRepoFinderResult to 0 because
           * the code in ostree_repo_pull_from_remotes_async() will be able to
           * check it just as quickly as we can here; so don’t duplicate the
           * code. */
          g_ptr_array_add (results, ostree_repo_finder_result_new (remote, finder, priority, supported_ref_to_checksum, 0));
        }
    }

  g_ptr_array_sort (results, results_compare_cb);

  g_task_return_pointer (task, g_steal_pointer (&results), (GDestroyNotify) g_ptr_array_unref);
}

static GPtrArray *
ostree_repo_finder_mount_resolve_finish (OstreeRepoFinder  *self,
                                         GAsyncResult      *result,
                                         GError           **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
ostree_repo_finder_mount_init (OstreeRepoFinderMount *self)
{
  /* Nothing to see here. */
}

static void
ostree_repo_finder_mount_constructed (GObject *object)
{
  OstreeRepoFinderMount *self = OSTREE_REPO_FINDER_MOUNT (object);

  G_OBJECT_CLASS (ostree_repo_finder_mount_parent_class)->constructed (object);

  if (self->monitor == NULL)
    self->monitor = g_volume_monitor_get ();
}

typedef enum
{
  PROP_MONITOR = 1,
} OstreeRepoFinderMountProperty;

static void
ostree_repo_finder_mount_get_property (GObject    *object,
                                       guint       property_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  OstreeRepoFinderMount *self = OSTREE_REPO_FINDER_MOUNT (object);

  switch ((OstreeRepoFinderMountProperty) property_id)
    {
    case PROP_MONITOR:
      g_value_set_object (value, self->monitor);
      break;
    default:
      g_assert_not_reached ();
    }
}

static void
ostree_repo_finder_mount_set_property (GObject      *object,
                                       guint         property_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  OstreeRepoFinderMount *self = OSTREE_REPO_FINDER_MOUNT (object);

  switch ((OstreeRepoFinderMountProperty) property_id)
    {
    case PROP_MONITOR:
      /* Construct-only. */
      g_assert (self->monitor == NULL);
      self->monitor = g_value_dup_object (value);
      break;
    default:
      g_assert_not_reached ();
    }
}

static void
ostree_repo_finder_mount_dispose (GObject *object)
{
  OstreeRepoFinderMount *self = OSTREE_REPO_FINDER_MOUNT (object);

  g_clear_object (&self->monitor);

  G_OBJECT_CLASS (ostree_repo_finder_mount_parent_class)->dispose (object);
}

static void
ostree_repo_finder_mount_class_init (OstreeRepoFinderMountClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = ostree_repo_finder_mount_get_property;
  object_class->set_property = ostree_repo_finder_mount_set_property;
  object_class->constructed = ostree_repo_finder_mount_constructed;
  object_class->dispose = ostree_repo_finder_mount_dispose;

  /**
   * OstreeRepoFinderMount:monitor:
   *
   * Volume monitor to use to look up mounted volumes when queried.
   *
   * Since: 2017.8
   */
  g_object_class_install_property (object_class, PROP_MONITOR,
                                   g_param_spec_object ("monitor",
                                                        "Volume Monitor",
                                                        "Volume monitor to use "
                                                        "to look up mounted "
                                                        "volumes when queried.",
                                                        G_TYPE_VOLUME_MONITOR,
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));
}

static void
ostree_repo_finder_mount_iface_init (OstreeRepoFinderInterface *iface)
{
  iface->resolve_async = ostree_repo_finder_mount_resolve_async;
  iface->resolve_finish = ostree_repo_finder_mount_resolve_finish;
}

/**
 * ostree_repo_finder_mount_new:
 * @monitor: (nullable) (transfer none): volume monitor to use, or %NULL to use
 *    the system default
 *
 * Create a new #OstreeRepoFinderMount, using the given @monitor to look up
 * volumes. If @monitor is %NULL, the monitor from g_volume_monitor_get() will
 * be used.
 *
 * Returns: (transfer full): a new #OstreeRepoFinderMount
 * Since: 2017.8
 */
OstreeRepoFinderMount *
ostree_repo_finder_mount_new (GVolumeMonitor *monitor)
{
  g_return_val_if_fail (monitor == NULL || G_IS_VOLUME_MONITOR (monitor), NULL);

  return g_object_new (OSTREE_TYPE_REPO_FINDER_MOUNT,
                       "monitor", monitor,
                       NULL);
}
