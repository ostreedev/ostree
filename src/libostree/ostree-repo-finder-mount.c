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

#include <gio/gio.h>
#include <gio/gunixmounts.h>
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
 * For each mounted removable volume, the directory `.ostree/repos.d` will be
 * enumerated, and all OSTree repositories below it will be searched, in lexical
 * order, for the requested #OstreeCollectionRefs. The names of the directories
 * below `.ostree/repos.d` are irrelevant, apart from their lexical ordering.
 * The directories `.ostree/repo`, `ostree/repo` and `var/lib/flatpak`
 * will be searched after the others, if they exist.
 * Non-removable volumes are ignored.
 *
 * For each repository which is found, a result will be returned for the
 * intersection of the refs being searched for, and the refs in `refs/heads` and
 * `refs/mirrors` in the repository on the removable volume.
 *
 * Symlinks are followed when listing the repositories, so a volume might
 * contain a single OSTree at some arbitrary path, with a symlink from
 * `.ostree/repos.d`. Any symlink which points outside the volume’s file
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
  OstreeRemote *keyring_remote;  /* (owned) */
} UriAndKeyring;

static void
uri_and_keyring_free (UriAndKeyring *data)
{
  g_free (data->uri);
  ostree_remote_unref (data->keyring_remote);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (UriAndKeyring, uri_and_keyring_free)

static UriAndKeyring *
uri_and_keyring_new (const gchar  *uri,
                     OstreeRemote *keyring_remote)
{
  g_autoptr(UriAndKeyring) data = NULL;

  data = g_new0 (UriAndKeyring, 1);
  data->uri = g_strdup (uri);
  data->keyring_remote = ostree_remote_ref (keyring_remote);

  return g_steal_pointer (&data);
}

static guint
uri_and_keyring_hash (gconstpointer key)
{
  const UriAndKeyring *_key = key;

  return g_str_hash (_key->uri) ^ g_str_hash (_key->keyring_remote->keyring);
}

static gboolean
uri_and_keyring_equal (gconstpointer a,
                       gconstpointer b)
{
  const UriAndKeyring *_a = a, *_b = b;

  return (g_str_equal (_a->uri, _b->uri) &&
          g_str_equal (_a->keyring_remote->keyring, _b->keyring_remote->keyring));
}

/* This must return a valid remote name (suitable for use in a refspec). */
static gchar *
uri_and_keyring_to_name (UriAndKeyring *data)
{
  g_autofree gchar *escaped_uri = g_uri_escape_string (data->uri, NULL, FALSE);
  g_autofree gchar *escaped_keyring = g_uri_escape_string (data->keyring_remote->keyring, NULL, FALSE);

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

typedef struct
{
  char *ordering_name;  /* (owned) */
  OstreeRepo *repo;  /* (owned) */
  GHashTable *refs;  /* (owned) (element-type OstreeCollectionRef utf8) */
} RepoAndRefs;

static void
repo_and_refs_clear (RepoAndRefs *data)
{
  g_hash_table_unref (data->refs);
  g_object_unref (data->repo);
  g_free (data->ordering_name);
}

static gint
repo_and_refs_compare (gconstpointer a,
                       gconstpointer b)
{
  const RepoAndRefs *_a = a;
  const RepoAndRefs *_b = b;

  return strcmp (_a->ordering_name, _b->ordering_name);
}

/* Check whether the repo at @dfd/@path is within the given mount, is not equal
 * to the @parent_repo, and can be opened. If so, return it as @out_repo and
 * all its collection–refs as @out_refs, to be added into the results. */
static gboolean
scan_repo (int                 dfd,
           const char         *path,
           const char         *mount_name,
           const struct stat  *mount_root_stbuf,
           OstreeRepo         *parent_repo,
           OstreeRepo        **out_repo,
           GHashTable        **out_refs,
           GCancellable       *cancellable,
           GError            **error)
{
  g_autoptr(GError) local_error = NULL;

  g_autoptr(OstreeRepo) repo = ostree_repo_open_at (dfd, path, cancellable, &local_error);
  if (repo == NULL)
    {
      g_debug ("Ignoring repository ‘%s’ on mount ‘%s’ as it could not be opened: %s",
               path, mount_name, local_error->message);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  int repo_dfd = ostree_repo_get_dfd (repo);
  struct stat stbuf;

  if (!glnx_fstat (repo_dfd, &stbuf, &local_error))
    {
      g_debug ("Ignoring repository ‘%s’ on mount ‘%s’ as querying its info failed: %s",
               path, mount_name, local_error->message);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  /* Check the resolved repository path is below the mount point. Do not
   * allow ref symlinks to point somewhere outside of the mounted volume. */
  if (stbuf.st_dev != mount_root_stbuf->st_dev)
    {
      g_debug ("Ignoring repository ‘%s’ on mount ‘%s’ as it’s on a different file system from the mount",
               path, mount_name);
      return glnx_throw (error, "Repository is on a different file system from the mount");
    }

  /* Exclude repositories which resolve to @parent_repo. */
  if (stbuf.st_dev == parent_repo->device &&
      stbuf.st_ino == parent_repo->inode)
    {
      g_debug ("Ignoring repository ‘%s’ on mount ‘%s’ as it is the same as the one we are resolving",
               path, mount_name);
      return glnx_throw (error, "Repository is the same as the one we are resolving");
    }

  /* List the repo’s refs and return them. */
  g_autoptr(GHashTable) repo_refs = NULL;  /* (element-type OstreeCollectionRef utf8) */

  if (!ostree_repo_list_collection_refs (repo, NULL, &repo_refs,
                                         OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_REMOTES,
                                         cancellable, &local_error))
    {
      g_debug ("Ignoring repository ‘%s’ on mount ‘%s’ as its refs could not be listed: %s",
               path, mount_name, local_error->message);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  if (out_repo != NULL)
    *out_repo = g_steal_pointer (&repo);
  if (out_refs != NULL)
    *out_refs = g_steal_pointer (&repo_refs);

  return TRUE;
}

static void
scan_and_add_repo (int                 dfd,
                   const char         *path,
                   gboolean            sortable,
                   const char         *mount_name,
                   const struct stat  *mount_root_stbuf,
                   OstreeRepo         *parent_repo,
                   GArray             *inout_repos_refs,
                   GCancellable       *cancellable)
{
  g_autoptr(GHashTable) repo_refs = NULL;
  g_autoptr(OstreeRepo) repo = NULL;

  if (scan_repo (dfd, path,
                 mount_name, mount_root_stbuf,
                 parent_repo, &repo, &repo_refs, cancellable, NULL))
    {
      RepoAndRefs val = {
        sortable ? g_strdup (path) : NULL,
        g_steal_pointer (&repo),
        g_steal_pointer (&repo_refs)
      };
      g_array_append_val (inout_repos_refs, val);

      g_debug ("%s: Adding repo ‘%s’ (%ssortable)",
               G_STRFUNC, path, sortable ? "" : "not ");
    }
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
      glnx_autofd int mount_root_dfd = -1;
      struct stat mount_root_stbuf;
      glnx_autofd int repos_dfd = -1;
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

      mount_root = g_mount_get_root (mount);
      mount_root_path = g_file_get_path (mount_root);

      if (!glnx_opendirat (AT_FDCWD, mount_root_path, TRUE, &mount_root_dfd, &local_error))
        {
          g_debug ("Ignoring mount ‘%s’ as ‘%s’ directory can’t be opened: %s",
                   mount_name, mount_root_path, local_error->message);
          continue;
        }

#if GLIB_CHECK_VERSION(2, 55, 0)
G_GNUC_BEGIN_IGNORE_DEPRECATIONS  /* remove once GLIB_VERSION_MAX_ALLOWED ≥ 2.56 */
      g_autoptr(GUnixMountEntry) mount_entry = g_unix_mount_at (mount_root_path, NULL);

      if (mount_entry != NULL &&
          (g_unix_is_system_fs_type (g_unix_mount_get_fs_type (mount_entry)) ||
           g_unix_is_system_device_path (g_unix_mount_get_device_path (mount_entry))))
        {
          g_debug ("Ignoring mount ‘%s’ as its file system type (%s) or device "
                   "path (%s) indicate it’s a system mount.",
                   mount_name, g_unix_mount_get_fs_type (mount_entry),
                   g_unix_mount_get_device_path (mount_entry));
          continue;
        }
G_GNUC_END_IGNORE_DEPRECATIONS
#endif  /* GLib 2.56.0 */

      /* stat() the mount root so we can later check whether the resolved
       * repositories for individual refs are on the same device (to avoid the
       * symlinks for them pointing outside the mount root). */
      if (!glnx_fstat (mount_root_dfd, &mount_root_stbuf, &local_error))
        {
          g_debug ("Ignoring mount ‘%s’ as querying info of ‘%s’ failed: %s",
                   mount_name, mount_root_path, local_error->message);
          continue;
        }

      /* Check if it contains a .ostree/repos.d directory. If not, move on and
       * try the other well-known subdirectories. */
      if (!glnx_opendirat (mount_root_dfd, ".ostree/repos.d", TRUE, &repos_dfd, NULL))
        repos_dfd = -1;

      /* List all the repositories in the repos.d directory. */
      /* (element-type GHashTable (element-type OstreeCollectionRef utf8)) */
      g_autoptr(GArray) repos_refs = g_array_new (FALSE, TRUE, sizeof (RepoAndRefs));
      g_array_set_clear_func (repos_refs, (GDestroyNotify) repo_and_refs_clear);

      GLnxDirFdIterator repos_iter;

      if (repos_dfd >= 0 &&
          !glnx_dirfd_iterator_init_at (repos_dfd, ".", TRUE, &repos_iter, &local_error))
        {
          g_debug ("Error iterating over ‘%s/.ostree/repos.d’ directory in mount ‘%s’: %s",
                   mount_root_path, mount_name, local_error->message);
          g_clear_error (&local_error);
          /* don’t skip this mount as there’s still the ostree/repo directory to try */
        }
      else if (repos_dfd >= 0)
        {
          while (TRUE)
            {
              struct dirent *repo_dent;

              if (!glnx_dirfd_iterator_next_dent (&repos_iter, &repo_dent, cancellable, &local_error))
                {
                  g_debug ("Error iterating over ‘%s/.ostree/repos.d’ directory in mount ‘%s’: %s",
                           mount_root_path, mount_name, local_error->message);
                  g_clear_error (&local_error);
                  /* don’t skip this mount as there’s still the ostree/repo directory to try */
                  break;
                }

              if (repo_dent == NULL)
                break;

              /* Grab the set of collection–refs from the repo if we can open it. */
              scan_and_add_repo (repos_dfd, repo_dent->d_name, TRUE,
                                 mount_name, &mount_root_stbuf,
                                 parent_repo, repos_refs, cancellable);
            }
        }

      /* Sort the repos lexically. */
      g_array_sort (repos_refs, repo_and_refs_compare);

      /* Also check the well-known special-case directories in the mount.
       * Add them after sorting, so they’re always last. */
      const gchar * const well_known_repos[] =
        {
          ".ostree/repo",
          "ostree/repo",
          "var/lib/flatpak",
        };

      for (i = 0; i < G_N_ELEMENTS (well_known_repos); i++)
        scan_and_add_repo (mount_root_dfd, well_known_repos[i], FALSE,
                           mount_name, &mount_root_stbuf,
                           parent_repo, repos_refs, cancellable);

      /* Check whether a subdirectory exists for any of the @refs we’re looking
       * for. If so, and it’s a symbolic link, dereference it so multiple links
       * to the same repository (containing multiple refs) are coalesced.
       * Otherwise, include it as a result by itself. */
      repo_to_refs = g_hash_table_new_full (uri_and_keyring_hash, uri_and_keyring_equal,
                                            (GDestroyNotify) uri_and_keyring_free, (GDestroyNotify) g_hash_table_unref);

      for (i = 0; refs[i] != NULL; i++)
        {
          const OstreeCollectionRef *ref = refs[i];
          g_autofree gchar *resolved_repo_uri = NULL;
          g_autoptr(UriAndKeyring) resolved_repo = NULL;

          for (gsize j = 0; j < repos_refs->len; j++)
            {
              const RepoAndRefs *repo_and_refs = &g_array_index (repos_refs, RepoAndRefs, j);
              OstreeRepo *repo = repo_and_refs->repo;
              GHashTable *repo_refs = repo_and_refs->refs;
              g_autofree char *repo_path = g_file_get_path (ostree_repo_get_path (repo));
              g_autoptr(OstreeRemote) keyring_remote = NULL;

              const gchar *checksum = g_hash_table_lookup (repo_refs, ref);

              if (checksum == NULL)
                {
                  g_debug ("Ignoring repository ‘%s’ when looking for ref (%s, %s) on mount ‘%s’ as it doesn’t contain the ref.",
                           repo_path, ref->collection_id, ref->ref_name, mount_name);
                  g_clear_error (&local_error);
                  continue;
                }

              /* Finally, look up the GPG keyring for this ref. */
              keyring_remote = ostree_repo_resolve_keyring_for_collection (parent_repo,
                                                                           ref->collection_id,
                                                                           cancellable, &local_error);

              if (keyring_remote == NULL)
                {
                  g_debug ("Ignoring repository ‘%s’ when looking for ref (%s, %s) on mount ‘%s’ due to missing keyring: %s",
                           repo_path, ref->collection_id, ref->ref_name, mount_name, local_error->message);
                  g_clear_error (&local_error);
                  continue;
                }

              /* There is a valid repo at (or pointed to by)
               * $mount_root/.ostree/repos.d/$something.
               * Add it to the results, keyed by the canonicalised repository URI
               * to deduplicate the results. */
              g_autofree char *canonical_repo_path = realpath (repo_path, NULL);
              resolved_repo_uri = g_strconcat ("file://", canonical_repo_path, NULL);
              g_debug ("Resolved ref (%s, %s) on mount ‘%s’ to repo URI ‘%s’ with keyring ‘%s’ from remote ‘%s’.",
                       ref->collection_id, ref->ref_name, mount_name, resolved_repo_uri,
                       keyring_remote->keyring, keyring_remote->name);

              resolved_repo = uri_and_keyring_new (resolved_repo_uri, keyring_remote);

              supported_ref_to_checksum = g_hash_table_lookup (repo_to_refs, resolved_repo);

              if (supported_ref_to_checksum == NULL)
                {
                  supported_ref_to_checksum = g_hash_table_new_full (ostree_collection_ref_hash,
                                                                     ostree_collection_ref_equal,
                                                                     NULL, g_free);
                  g_hash_table_insert (repo_to_refs, g_steal_pointer (&resolved_repo), supported_ref_to_checksum  /* transfer */);
                }

              g_hash_table_insert (supported_ref_to_checksum, (gpointer) ref, g_strdup (checksum));

              /* We’ve found a result for this collection–ref. No point in checking
               * the other repos on the mount, since pulling in parallel from them won’t help. */
              break;
            }
        }

      /* Aggregate the results. */
      g_hash_table_iter_init (&iter, repo_to_refs);

      while (g_hash_table_iter_next (&iter, (gpointer *) &repo, (gpointer *) &supported_ref_to_checksum))
        {
          g_autoptr(OstreeRemote) remote = NULL;

          /* Build an #OstreeRemote. Use the escaped URI, since remote->name
           * is used in file paths, so needs to not contain special characters. */
          g_autofree gchar *name = uri_and_keyring_to_name (repo);
          remote = ostree_remote_new_dynamic (name, repo->keyring_remote->name);

          g_clear_pointer (&remote->keyring, g_free);
          remote->keyring = g_strdup (repo->keyring_remote->keyring);

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
