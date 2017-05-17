/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2012,2013 Colin Walters <walters@verbum.org>
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

#ifdef HAVE_LIBCURL_OR_LIBSOUP

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-metalink.h"
#include "ostree-fetcher-util.h"
#include "ot-fs-utils.h"

#include <gio/gunixinputstream.h>

#define OSTREE_REPO_PULL_CONTENT_PRIORITY  (OSTREE_FETCHER_DEFAULT_PRIORITY)
#define OSTREE_REPO_PULL_METADATA_PRIORITY (OSTREE_REPO_PULL_CONTENT_PRIORITY - 100)

typedef struct {
  OstreeRepo   *repo;
  int           tmpdir_dfd;
  OstreeRepoPullFlags flags;
  char         *remote_name;
  OstreeRepoMode remote_mode;
  OstreeFetcher *fetcher;
  GPtrArray     *meta_mirrorlist;    /* List of base URIs for fetching metadata */
  GPtrArray     *content_mirrorlist; /* List of base URIs for fetching content */
  OstreeRepo   *remote_repo_local;

  GMainContext    *main_context;
  GCancellable *cancellable;
  OstreeAsyncProgress *progress;

  GVariant         *extra_headers;

  gboolean      dry_run;
  gboolean      dry_run_emitted_progress;
  gboolean      legacy_transaction_resuming;
  enum {
    OSTREE_PULL_PHASE_FETCHING_REFS,
    OSTREE_PULL_PHASE_FETCHING_OBJECTS
  }             phase;
  gint          n_scanned_metadata;

  gboolean          gpg_verify;
  gboolean          require_static_deltas;
  gboolean          disable_static_deltas;
  gboolean          gpg_verify_summary;
  gboolean          has_tombstone_commits;

  GBytes           *summary_data;
  GBytes           *summary_data_sig;
  GVariant         *summary;
  GHashTable       *summary_deltas_checksums;
  GPtrArray        *static_delta_superblocks;
  GHashTable       *expected_commit_sizes; /* Maps commit checksum to known size */
  GHashTable       *commit_to_depth; /* Maps commit checksum maximum depth */
  GHashTable       *scanned_metadata; /* Maps object name to itself */
  GHashTable       *requested_metadata; /* Maps object name to itself */
  GHashTable       *requested_content; /* Maps checksum to itself */
  GHashTable       *requested_fallback_content; /* Maps checksum to itself */
  GHashTable       *pending_fetch_metadata; /* Map<ObjectName,FetchObjectData> */
  GHashTable       *pending_fetch_content; /* Map<checksum,FetchObjectData> */
  GHashTable       *pending_fetch_deltaparts; /* Set<FetchStaticDeltaData> */
  guint             n_outstanding_metadata_fetches;
  guint             n_outstanding_metadata_write_requests;
  guint             n_outstanding_content_fetches;
  guint             n_outstanding_content_write_requests;
  guint             n_outstanding_deltapart_fetches;
  guint             n_outstanding_deltapart_write_requests;
  guint             n_total_deltaparts;
  guint             n_total_delta_fallbacks;
  guint64           fetched_deltapart_size; /* How much of the delta we have now */
  guint64           total_deltapart_size;
  guint64           total_deltapart_usize;
  gint              n_requested_metadata;
  gint              n_requested_content;
  guint             n_fetched_deltaparts;
  guint             n_fetched_deltapart_fallbacks;
  guint             n_fetched_metadata;
  guint             n_fetched_content;

  int               maxdepth;
  guint64           start_time;

  gboolean          is_mirror;
  gboolean          is_commit_only;
  gboolean          is_untrusted;

  GPtrArray        *dirs;

  gboolean      have_previous_bytes;
  guint64       previous_bytes_sec;
  guint64       previous_total_downloaded;

  GError       *cached_async_error;
  GError      **async_error;
  gboolean      caught_error;

  GQueue scan_object_queue;
  GSource *idle_src;
} OtPullData;

typedef struct {
  OtPullData  *pull_data;
  GVariant    *object;
  char        *path;
  gboolean     is_detached_meta;

  /* Only relevant when is_detached_meta is TRUE.  Controls
   * whether to fetch the primary object after fetching its
   * detached metadata (no need if it's already stored). */
  gboolean     object_is_stored;
} FetchObjectData;

typedef struct {
  OtPullData  *pull_data;
  GVariant *objects;
  char *expected_checksum;
  char *from_revision;
  char *to_revision;
  guint i;
  guint64 size;
} FetchStaticDeltaData;

typedef struct {
  guchar csum[OSTREE_SHA256_DIGEST_LEN];
  char *path;
  OstreeObjectType objtype;
  guint recursion_depth;
} ScanObjectQueueData;

static void start_fetch (OtPullData *pull_data, FetchObjectData *fetch);
static void start_fetch_deltapart (OtPullData *pull_data,
                                   FetchStaticDeltaData *fetch);
static gboolean fetcher_queue_is_full (OtPullData *pull_data);
static void queue_scan_one_metadata_object (OtPullData         *pull_data,
                                            const char         *csum,
                                            OstreeObjectType    objtype,
                                            const char         *path,
                                            guint               recursion_depth);

static void queue_scan_one_metadata_object_c (OtPullData         *pull_data,
                                              const guchar       *csum,
                                              OstreeObjectType    objtype,
                                              const char         *path,
                                              guint               recursion_depth);

static gboolean scan_one_metadata_object_c (OtPullData         *pull_data,
                                            const guchar       *csum,
                                            OstreeObjectType    objtype,
                                            const char         *path,
                                            guint               recursion_depth,
                                            GCancellable       *cancellable,
                                            GError            **error);

static gboolean
update_progress (gpointer user_data)
{
  OtPullData *pull_data;
  guint outstanding_writes;
  guint outstanding_fetches;
  guint64 bytes_transferred;
  guint fetched;
  guint requested;
  guint n_scanned_metadata;
  guint64 start_time;

  pull_data = user_data;

  if (! pull_data->progress)
    return FALSE;

  /* In dry run, we only emit progress once metadata is done */
  if (pull_data->dry_run && pull_data->n_outstanding_metadata_fetches > 0)
    return TRUE;

  outstanding_writes = pull_data->n_outstanding_content_write_requests +
    pull_data->n_outstanding_metadata_write_requests +
    pull_data->n_outstanding_deltapart_write_requests;
  outstanding_fetches = pull_data->n_outstanding_content_fetches +
    pull_data->n_outstanding_metadata_fetches +
    pull_data->n_outstanding_deltapart_fetches;
  bytes_transferred = _ostree_fetcher_bytes_transferred (pull_data->fetcher);
  fetched = pull_data->n_fetched_metadata + pull_data->n_fetched_content;
  requested = pull_data->n_requested_metadata + pull_data->n_requested_content;
  n_scanned_metadata = pull_data->n_scanned_metadata;
  start_time = pull_data->start_time;

  ostree_async_progress_set (pull_data->progress,
                             "outstanding-fetches", "u", outstanding_fetches,
                             "outstanding-writes", "u", outstanding_writes,
                             "fetched", "u", fetched,
                             "requested", "u", requested,
                             "scanning", "u", g_queue_is_empty (&pull_data->scan_object_queue) ? 0 : 1,
                             "scanned-metadata", "u", n_scanned_metadata,
                             "bytes-transferred", "t", bytes_transferred,
                             "start-time", "t", start_time,
                             /* Deltas */
                             "fetched-delta-parts",
                                  "u", pull_data->n_fetched_deltaparts,
                             "total-delta-parts",
                                  "u", pull_data->n_total_deltaparts,
                             "fetched-delta-fallbacks",
                                  "u", pull_data->n_fetched_deltapart_fallbacks,
                             "total-delta-fallbacks",
                                  "u", pull_data->n_total_delta_fallbacks,
                             "fetched-delta-part-size",
                                  "t", pull_data->fetched_deltapart_size,
                             "total-delta-part-size",
                                  "t", pull_data->total_deltapart_size,
                             "total-delta-part-usize",
                                  "t", pull_data->total_deltapart_usize,
                             "total-delta-superblocks",
                                  "u", pull_data->static_delta_superblocks->len,
                             /* We fetch metadata before content.  These allow us to report metadata fetch progress specifically. */
                             "outstanding-metadata-fetches", "u", pull_data->n_outstanding_metadata_fetches,
                             "metadata-fetched", "u", pull_data->n_fetched_metadata,
                             /* Overall status. */
                             "status", "s", "",
                             NULL);

  if (pull_data->dry_run)
    pull_data->dry_run_emitted_progress = TRUE;

  return TRUE;
}

/* The core logic function for whether we should continue the main loop */
static gboolean
pull_termination_condition (OtPullData          *pull_data)
{
  gboolean current_fetch_idle = (pull_data->n_outstanding_metadata_fetches == 0 &&
                                 pull_data->n_outstanding_content_fetches == 0 &&
                                 pull_data->n_outstanding_deltapart_fetches == 0);
  gboolean current_write_idle = (pull_data->n_outstanding_metadata_write_requests == 0 &&
                                 pull_data->n_outstanding_content_write_requests == 0 &&
                                 pull_data->n_outstanding_deltapart_write_requests == 0 );
  gboolean current_scan_idle = g_queue_is_empty (&pull_data->scan_object_queue);
  gboolean current_idle = current_fetch_idle && current_write_idle && current_scan_idle;

  /* we only enter the main loop when we're fetching objects */
  g_assert (pull_data->phase == OSTREE_PULL_PHASE_FETCHING_OBJECTS);

  if (pull_data->caught_error)
    return TRUE;

  if (pull_data->dry_run)
    return pull_data->dry_run_emitted_progress;

  if (current_idle)
    g_debug ("pull: idle, exiting mainloop");

  return current_idle;
}

static void
check_outstanding_requests_handle_error (OtPullData          *pull_data,
                                         GError             **errorp)
{
  g_assert (errorp);

  GError *error = *errorp;
  if (error)
    {
      if (!pull_data->caught_error)
        {
          pull_data->caught_error = TRUE;
          g_propagate_error (pull_data->async_error, g_steal_pointer (errorp));
        }
      else
        {
          g_clear_error (errorp);
        }
    }
  else
    {
      GHashTableIter hiter;
      gpointer key, value;

      /* We may have just completed an async fetch operation. Now we look at
       * possibly enqueuing more requests. The goal of queuing is to both avoid
       * overloading the fetcher backend with HTTP requests, but also to
       * prioritize metadata fetches over content, so we have accurate
       * reporting. Hence here, we process metadata fetches first.
       */

      /* Try filling the queue with metadata we need to fetch */
      g_hash_table_iter_init (&hiter, pull_data->pending_fetch_metadata);
      while (!fetcher_queue_is_full (pull_data) &&
             g_hash_table_iter_next (&hiter, &key, &value))
        {
          GVariant *objname = key;
          FetchObjectData *fetch = value;

          /* Steal both key and value */
          g_hash_table_iter_steal (&hiter);

          /* This takes ownership of the value */
          start_fetch (pull_data, fetch);
          /* And unref the key */
          g_variant_unref (objname);
        }

      /* Now, process deltapart requests */
      g_hash_table_iter_init (&hiter, pull_data->pending_fetch_deltaparts);
      while (!fetcher_queue_is_full (pull_data) &&
             g_hash_table_iter_next (&hiter, &key, &value))
        {
          FetchStaticDeltaData *fetch = key;
          g_hash_table_iter_steal (&hiter);
          /* Takes ownership */
          start_fetch_deltapart (pull_data, fetch);
        }

      /* Next, fill the queue with content */
      g_hash_table_iter_init (&hiter, pull_data->pending_fetch_content);
      while (!fetcher_queue_is_full (pull_data) &&
             g_hash_table_iter_next (&hiter, &key, &value))
        {
          char *checksum = key;
          FetchObjectData *fetch = value;

          /* Steal both key and value */
          g_hash_table_iter_steal (&hiter);

          /* This takes ownership of the value */
          start_fetch (pull_data, fetch);
          /* And unref the key */
          g_free (checksum);
        }

    }
}

/* We have a total-request limit, as well has a hardcoded max of 2 for delta
 * parts. The logic for the delta one is that processing them is expensive, and
 * doing multiple simultaneously could risk space/memory on smaller devices. We
 * also throttle on outstanding writes in case fetches are faster.
 */
static gboolean
fetcher_queue_is_full (OtPullData *pull_data)
{
  const gboolean fetch_full =
      ((pull_data->n_outstanding_metadata_fetches +
        pull_data->n_outstanding_content_fetches +
        pull_data->n_outstanding_deltapart_fetches) ==
         _OSTREE_MAX_OUTSTANDING_FETCHER_REQUESTS);
  const gboolean deltas_full =
      (pull_data->n_outstanding_deltapart_fetches ==
        _OSTREE_MAX_OUTSTANDING_DELTAPART_REQUESTS);
  const gboolean writes_full =
      ((pull_data->n_outstanding_metadata_write_requests +
        pull_data->n_outstanding_content_write_requests +
        pull_data->n_outstanding_deltapart_write_requests) >=
         _OSTREE_MAX_OUTSTANDING_WRITE_REQUESTS);
  return fetch_full || deltas_full || writes_full;
}

static gboolean
idle_worker (gpointer user_data)
{
  OtPullData *pull_data = user_data;
  ScanObjectQueueData *scan_data;
  g_autoptr(GError) error = NULL;

  scan_data = g_queue_pop_head (&pull_data->scan_object_queue);
  if (!scan_data)
    {
      g_clear_pointer (&pull_data->idle_src, (GDestroyNotify) g_source_destroy);
      return G_SOURCE_REMOVE;
    }

  scan_one_metadata_object_c (pull_data,
                              scan_data->csum,
                              scan_data->objtype,
                              scan_data->path,
                              scan_data->recursion_depth,
                              pull_data->cancellable,
                              &error);
  check_outstanding_requests_handle_error (pull_data, &error);

  g_free (scan_data->path);
  g_free (scan_data);
  return G_SOURCE_CONTINUE;
}

static void
ensure_idle_queued (OtPullData *pull_data)
{
  GSource *idle_src;

  if (pull_data->idle_src)
    return;

  idle_src = g_idle_source_new ();
  g_source_set_callback (idle_src, idle_worker, pull_data, NULL);
  g_source_attach (idle_src, pull_data->main_context);
  g_source_unref (idle_src);
  pull_data->idle_src = idle_src;
}

typedef struct {
  OtPullData     *pull_data;
  GInputStream   *result_stream;
} OstreeFetchUriSyncData;

static gboolean
fetch_mirrored_uri_contents_utf8_sync (OstreeFetcher  *fetcher,
                                       GPtrArray      *mirrorlist,
                                       const char     *filename,
                                       char          **out_contents,
                                       GCancellable   *cancellable,
                                       GError        **error)
{
  g_autoptr(GBytes) bytes = NULL;
  if (!_ostree_fetcher_mirrored_request_to_membuf (fetcher, mirrorlist,
                                                   filename, TRUE, FALSE,
                                                   &bytes,
                                                   OSTREE_MAX_METADATA_SIZE,
                                                   cancellable, error))
    return FALSE;

  gsize len;
  g_autofree char *ret_contents = g_bytes_unref_to_data (g_steal_pointer (&bytes), &len);

  if (!g_utf8_validate (ret_contents, -1, NULL))
    return glnx_throw (error, "Invalid UTF-8");

  ot_transfer_out_value (out_contents, &ret_contents);
  return TRUE;
}

static gboolean
fetch_uri_contents_utf8_sync (OstreeFetcher  *fetcher,
                              OstreeFetcherURI *uri,
                              char          **out_contents,
                              GCancellable   *cancellable,
                              GError        **error)
{
  g_autoptr(GPtrArray) mirrorlist = g_ptr_array_new ();
  g_ptr_array_add (mirrorlist, uri); /* no transfer */
  return fetch_mirrored_uri_contents_utf8_sync (fetcher, mirrorlist,
                                                NULL, out_contents,
                                                cancellable, error);
}

static gboolean
write_commitpartial_for (OtPullData *pull_data,
                         const char *checksum,
                         GError **error)
{
  g_autofree char *commitpartial_path = _ostree_get_commitpartial_path (checksum);
  glnx_fd_close int fd = openat (pull_data->repo->repo_dir_fd, commitpartial_path, O_EXCL | O_CREAT | O_WRONLY | O_CLOEXEC | O_NOCTTY, 0644);
  if (fd == -1)
    {
      if (errno != EEXIST)
        return glnx_throw_errno_prefix (error, "open(%s)", commitpartial_path);
    }
  return TRUE;
}

static void
enqueue_one_object_request (OtPullData        *pull_data,
                            const char        *checksum,
                            OstreeObjectType   objtype,
                            const char        *path,
                            gboolean           is_detached_meta,
                            gboolean           object_is_stored);

static gboolean
matches_pull_dir (const char *current_file,
                  const char *pull_dir,
                  gboolean current_file_is_dir)
{
  const char *rest;

  if (g_str_has_prefix (pull_dir, current_file))
    {
      rest = pull_dir + strlen (current_file);
      if (*rest == 0)
        {
          /* The current file is exactly the same as the specified
             pull dir. This matches always, even if the file is not a
             directory. */
          return TRUE;
        }

      if (*rest == '/')
        {
          /* The current file is a directory-prefix of the pull_dir.
             Match only if this is supposed to be a directory */
          return current_file_is_dir;
        }

      /* Matched a non-directory prefix such as /foo being a prefix of /fooo,
         no match */
      return FALSE;
    }

  if (g_str_has_prefix (current_file, pull_dir))
    {
      rest = current_file + strlen (pull_dir);
      /* Only match if the prefix match matched the entire directory
         component */
      return *rest == '/';
    }

  return FALSE;
}


static gboolean
pull_matches_subdir (OtPullData *pull_data,
                     const char *path,
                     const char *basename,
                     gboolean basename_is_dir)
{
  if (pull_data->dirs == NULL)
    return TRUE;

  g_autofree char *file = g_strconcat (path, basename, NULL);

  for (guint i = 0; i < pull_data->dirs->len; i++)
    {
      const char *pull_dir = g_ptr_array_index (pull_data->dirs, i);
      if (matches_pull_dir (file, pull_dir, basename_is_dir))
        return TRUE;
    }

  return FALSE;
}

static gboolean
scan_dirtree_object (OtPullData   *pull_data,
                     const char   *checksum,
                     const char   *path,
                     int           recursion_depth,
                     GCancellable *cancellable,
                     GError      **error)
{
  if (recursion_depth > OSTREE_MAX_RECURSION)
    return glnx_throw (error, "Exceeded maximum recursion");

  g_autoptr(GVariant) tree = NULL;
  if (!ostree_repo_load_variant (pull_data->repo, OSTREE_OBJECT_TYPE_DIR_TREE, checksum,
                                 &tree, error))
    return FALSE;

  /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
  g_autoptr(GVariant) files_variant = g_variant_get_child_value (tree, 0);
  const guint n = g_variant_n_children (files_variant);
  for (guint i = 0; i < n; i++)
    {
      const char *filename;
      gboolean file_is_stored;
      g_autoptr(GVariant) csum = NULL;
      g_autofree char *file_checksum = NULL;

      g_variant_get_child (files_variant, i, "(&s@ay)", &filename, &csum);

      if (!ot_util_filename_validate (filename, error))
        return FALSE;

      /* Skip files if we're traversing a request only directory, unless it exactly
       * matches the path */
      if (!pull_matches_subdir (pull_data, path, filename, FALSE))
        continue;

      file_checksum = ostree_checksum_from_bytes_v (csum);

      if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_FILE, file_checksum,
                                   &file_is_stored, cancellable, error))
        return FALSE;

      if (!file_is_stored && pull_data->remote_repo_local)
        {
          if (!ostree_repo_import_object_from_with_trust (pull_data->repo, pull_data->remote_repo_local,
                                                          OSTREE_OBJECT_TYPE_FILE, file_checksum, !pull_data->is_untrusted,
                                                          cancellable, error))
            return FALSE;
        }
      else if (!file_is_stored && !g_hash_table_lookup (pull_data->requested_content, file_checksum))
        {
          g_hash_table_add (pull_data->requested_content, file_checksum);
          enqueue_one_object_request (pull_data, file_checksum, OSTREE_OBJECT_TYPE_FILE, path, FALSE, FALSE);
          file_checksum = NULL;  /* Transfer ownership */
        }
    }

  g_autoptr(GVariant) dirs_variant = g_variant_get_child_value (tree, 1);
  const guint m = g_variant_n_children (dirs_variant);
  for (guint i = 0; i < m; i++)
    {
      const char *dirname = NULL;
      g_autoptr(GVariant) tree_csum = NULL;
      g_autoptr(GVariant) meta_csum = NULL;
      g_variant_get_child (dirs_variant, i, "(&s@ay@ay)",
                           &dirname, &tree_csum, &meta_csum);

      if (!ot_util_filename_validate (dirname, error))
        return FALSE;

      if (!pull_matches_subdir (pull_data, path, dirname, TRUE))
        continue;

      const guchar *tree_csum_bytes = ostree_checksum_bytes_peek_validate (tree_csum, error);
      if (tree_csum_bytes == NULL)
        return FALSE;

      const guchar *meta_csum_bytes = ostree_checksum_bytes_peek_validate (meta_csum, error);
      if (meta_csum_bytes == NULL)
        return FALSE;

      g_autofree char *subpath = g_strconcat (path, dirname, "/", NULL);
      queue_scan_one_metadata_object_c (pull_data, tree_csum_bytes,
                                        OSTREE_OBJECT_TYPE_DIR_TREE, subpath, recursion_depth + 1);
      queue_scan_one_metadata_object_c (pull_data, meta_csum_bytes,
                                        OSTREE_OBJECT_TYPE_DIR_META, subpath, recursion_depth + 1);
    }

  return TRUE;
}

static gboolean
fetch_ref_contents (OtPullData    *pull_data,
                    const char    *ref,
                    char         **out_contents,
                    GCancellable  *cancellable,
                    GError       **error)
{
  g_autofree char *filename = g_build_filename ("refs", "heads", ref, NULL);
  g_autofree char *ret_contents = NULL;
  if (!fetch_mirrored_uri_contents_utf8_sync (pull_data->fetcher,
                                              pull_data->meta_mirrorlist,
                                              filename, &ret_contents,
                                              cancellable, error))
    return FALSE;

  g_strchomp (ret_contents);

  if (!ostree_validate_checksum_string (ret_contents, error))
    return FALSE;

  ot_transfer_out_value (out_contents, &ret_contents);
  return TRUE;
}

static gboolean
lookup_commit_checksum_from_summary (OtPullData    *pull_data,
                                     const char    *ref,
                                     char         **out_checksum,
                                     gsize         *out_size,
                                     GError       **error)
{
  g_autoptr(GVariant) refs = g_variant_get_child_value (pull_data->summary, 0);
  int i;
  if (!ot_variant_bsearch_str (refs, ref, &i))
    return glnx_throw (error, "No such branch '%s' in repository summary", ref);

  g_autoptr(GVariant) refdata = g_variant_get_child_value (refs, i);
  g_autoptr(GVariant) reftargetdata = g_variant_get_child_value (refdata, 1);
  guint64 commit_size;
  g_autoptr(GVariant) commit_csum_v = NULL;
  g_variant_get (reftargetdata, "(t@ay@a{sv})", &commit_size, &commit_csum_v, NULL);

  if (!ostree_validate_structureof_csum_v (commit_csum_v, error))
    return FALSE;

  *out_checksum = ostree_checksum_from_bytes_v (commit_csum_v);
  *out_size = commit_size;
  return TRUE;
}

static void
fetch_object_data_free (FetchObjectData *fetch_data)
{
  g_variant_unref (fetch_data->object);
  g_free (fetch_data->path);
  g_free (fetch_data);
}

static void
content_fetch_on_write_complete (GObject        *object,
                                 GAsyncResult   *result,
                                 gpointer        user_data)
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  OstreeObjectType objtype;
  const char *expected_checksum;
  g_autofree guchar *csum = NULL;
  g_autofree char *checksum = NULL;
  g_autofree char *checksum_obj = NULL;

  if (!ostree_repo_write_content_finish ((OstreeRepo*)object, result,
                                         &csum, error))
    goto out;

  checksum = ostree_checksum_from_bytes (csum);

  ostree_object_name_deserialize (fetch_data->object, &expected_checksum, &objtype);
  g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);

  checksum_obj = ostree_object_to_string (checksum, objtype);
  g_debug ("write of %s complete", checksum_obj);

  if (strcmp (checksum, expected_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted content object; checksum expected='%s' actual='%s'",
                   expected_checksum, checksum);
      goto out;
    }

  pull_data->n_fetched_content++;
  /* Was this a delta fallback? */
  if (g_hash_table_remove (pull_data->requested_fallback_content, expected_checksum))
    pull_data->n_fetched_deltapart_fallbacks++;
 out:
  pull_data->n_outstanding_content_write_requests--;
  check_outstanding_requests_handle_error (pull_data, &local_error);
  fetch_object_data_free (fetch_data);
}

static void
content_fetch_on_complete (GObject        *object,
                           GAsyncResult   *result,
                           gpointer        user_data) 
{
  OstreeFetcher *fetcher = (OstreeFetcher *)object;
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  GCancellable *cancellable = NULL;
  guint64 length;
  g_autoptr(GFileInfo) file_info = NULL;
  g_autoptr(GVariant) xattrs = NULL;
  g_autoptr(GInputStream) file_in = NULL;
  g_autoptr(GInputStream) object_input = NULL;
  g_autofree char *temp_path = NULL;
  const char *checksum;
  g_autofree char *checksum_obj = NULL;
  OstreeObjectType objtype;
  gboolean free_fetch_data = TRUE;

  if (!_ostree_fetcher_request_to_tmpfile_finish (fetcher, result, &temp_path, error))
    goto out;

  ostree_object_name_deserialize (fetch_data->object, &checksum, &objtype);
  g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);

  checksum_obj = ostree_object_to_string (checksum, objtype);
  g_debug ("fetch of %s complete", checksum_obj);

  if (pull_data->is_mirror && pull_data->repo->mode == OSTREE_REPO_MODE_ARCHIVE_Z2)
    {
      gboolean have_object;
      if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_FILE, checksum,
                                   &have_object,
                                   cancellable, error))
        goto out;

      if (!have_object)
        {
          if (!_ostree_repo_commit_loose_final (pull_data->repo, checksum, OSTREE_OBJECT_TYPE_FILE,
                                                _ostree_fetcher_get_dfd (fetcher), -1, temp_path,
                                                cancellable, error))
            goto out;
        }
      pull_data->n_fetched_content++;
    }
  else
    {
      /* Non-mirroring path */

      if (!ostree_content_file_parse_at (TRUE, _ostree_fetcher_get_dfd (fetcher),
                                         temp_path, FALSE,
                                         &file_in, &file_info, &xattrs,
                                         cancellable, error))
        {
          /* If it appears corrupted, delete it */
          (void) unlinkat (_ostree_fetcher_get_dfd (fetcher), temp_path, 0);
          goto out;
        }

      /* Also, delete it now that we've opened it, we'll hold
       * a reference to the fd.  If we fail to write later, then
       * the temp space will be cleaned up.
       */
      (void) unlinkat (_ostree_fetcher_get_dfd (fetcher), temp_path, 0);

      if (!ostree_raw_file_to_content_stream (file_in, file_info, xattrs,
                                              &object_input, &length,
                                              cancellable, error))
        goto out;
  
      pull_data->n_outstanding_content_write_requests++;
      ostree_repo_write_content_async (pull_data->repo, checksum,
                                       object_input, length,
                                       cancellable,
                                       content_fetch_on_write_complete, fetch_data);
      free_fetch_data = FALSE;
    }

 out:
  pull_data->n_outstanding_content_fetches--;
  check_outstanding_requests_handle_error (pull_data, &local_error);
  if (free_fetch_data)
    fetch_object_data_free (fetch_data);
}

static void
on_metadata_written (GObject           *object,
                     GAsyncResult      *result,
                     gpointer           user_data)
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  const char *expected_checksum;
  OstreeObjectType objtype;
  g_autofree char *checksum = NULL;
  g_autofree guchar *csum = NULL;
  g_autofree char *stringified_object = NULL;

  if (!ostree_repo_write_metadata_finish ((OstreeRepo*)object, result, 
                                          &csum, error))
    goto out;

  checksum = ostree_checksum_from_bytes (csum);

  ostree_object_name_deserialize (fetch_data->object, &expected_checksum, &objtype);
  g_assert (OSTREE_OBJECT_TYPE_IS_META (objtype));

  stringified_object = ostree_object_to_string (checksum, objtype);
  g_debug ("write of %s complete", stringified_object);

  if (strcmp (checksum, expected_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted metadata object; checksum expected='%s' actual='%s'",
                   expected_checksum, checksum);
      goto out;
    }

  queue_scan_one_metadata_object_c (pull_data, csum, objtype, fetch_data->path, 0);

 out:
  pull_data->n_outstanding_metadata_write_requests--;
  fetch_object_data_free (fetch_data);

  check_outstanding_requests_handle_error (pull_data, &local_error);
}

static void
meta_fetch_on_complete (GObject           *object,
                        GAsyncResult      *result,
                        gpointer           user_data)
{
  OstreeFetcher *fetcher = (OstreeFetcher *)object;
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *temp_path = NULL;
  const char *checksum;
  g_autofree char *checksum_obj = NULL;
  OstreeObjectType objtype;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  glnx_fd_close int fd = -1;
  gboolean free_fetch_data = TRUE;

  ostree_object_name_deserialize (fetch_data->object, &checksum, &objtype);
  checksum_obj = ostree_object_to_string (checksum, objtype);
  g_debug ("fetch of %s%s complete", checksum_obj,
           fetch_data->is_detached_meta ? " (detached)" : "");

  if (!_ostree_fetcher_request_to_tmpfile_finish (fetcher, result, &temp_path, error))
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          if (fetch_data->is_detached_meta)
            {
              /* There isn't any detached metadata, just fetch the commit */
              g_clear_error (&local_error);
              if (!fetch_data->object_is_stored)
                enqueue_one_object_request (pull_data, checksum, objtype, fetch_data->path, FALSE, FALSE);
            }

          /* When traversing parents, do not fail on a missing commit.
           * We may be pulling from a partial repository that ends in
           * a dangling parent reference. */
          else if (objtype == OSTREE_OBJECT_TYPE_COMMIT &&
                   pull_data->maxdepth != 0)
            {
              g_clear_error (&local_error);
              /* If the remote repo supports tombstone commits, check if the commit was intentionally
                 deleted.  */
              if (pull_data->has_tombstone_commits)
                {
                  enqueue_one_object_request (pull_data, checksum, OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT,
                                              fetch_data->path, FALSE, FALSE);
                }
            }
        }

      goto out;
    }

  /* Tombstone commits are always empty, so skip all processing here */
  if (objtype == OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT)
    goto out;

  fd = openat (_ostree_fetcher_get_dfd (fetcher), temp_path, O_RDONLY | O_CLOEXEC);
  if (fd == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (fetch_data->is_detached_meta)
    {
      if (!ot_util_variant_map_fd (fd, 0, G_VARIANT_TYPE ("a{sv}"),
                                   FALSE, &metadata, error))
        goto out;

      /* Now delete it, see comment in corresponding content fetch path */
      (void) unlinkat (_ostree_fetcher_get_dfd (fetcher), temp_path, 0);

      if (!ostree_repo_write_commit_detached_metadata (pull_data->repo, checksum, metadata,
                                                       pull_data->cancellable, error))
        goto out;

      if (!fetch_data->object_is_stored)
        enqueue_one_object_request (pull_data, checksum, objtype, fetch_data->path, FALSE, FALSE);
    }
  else
    {
      if (!ot_util_variant_map_fd (fd, 0, ostree_metadata_variant_type (objtype),
                                   FALSE, &metadata, error))
        goto out;

      (void) unlinkat (_ostree_fetcher_get_dfd (fetcher), temp_path, 0);

      /* Write the commitpartial file now while we're still fetching data */
      if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
        {
          if (!write_commitpartial_for (pull_data, checksum, error))
            goto out;
        }
      
      ostree_repo_write_metadata_async (pull_data->repo, objtype, checksum, metadata,
                                        pull_data->cancellable,
                                        on_metadata_written, fetch_data);
      pull_data->n_outstanding_metadata_write_requests++;
      free_fetch_data = FALSE;
    }

 out:
  g_assert (pull_data->n_outstanding_metadata_fetches > 0);
  pull_data->n_outstanding_metadata_fetches--;
  pull_data->n_fetched_metadata++;
  check_outstanding_requests_handle_error (pull_data, &local_error);
  if (free_fetch_data)
    fetch_object_data_free (fetch_data);
}

static void
fetch_static_delta_data_free (gpointer  data)
{
  FetchStaticDeltaData *fetch_data = data;
  g_free (fetch_data->expected_checksum);
  g_variant_unref (fetch_data->objects);
  g_free (fetch_data->from_revision);
  g_free (fetch_data->to_revision);
  g_free (fetch_data);
}

static void
on_static_delta_written (GObject           *object,
                         GAsyncResult      *result,
                         gpointer           user_data)
{
  FetchStaticDeltaData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;

  g_debug ("execute static delta part %s complete", fetch_data->expected_checksum);

  if (!_ostree_static_delta_part_execute_finish (pull_data->repo, result, error))
    goto out;

 out:
  g_assert (pull_data->n_outstanding_deltapart_write_requests > 0);
  pull_data->n_outstanding_deltapart_write_requests--;
  check_outstanding_requests_handle_error (pull_data, &local_error);
  /* Always free state */
  fetch_static_delta_data_free (fetch_data);
}

static void
static_deltapart_fetch_on_complete (GObject           *object,
                                    GAsyncResult      *result,
                                    gpointer           user_data)
{
  OstreeFetcher *fetcher = (OstreeFetcher *)object;
  FetchStaticDeltaData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_autofree char *temp_path = NULL;
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(GVariant) part = NULL;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  glnx_fd_close int fd = -1;
  gboolean free_fetch_data = TRUE;

  g_debug ("fetch static delta part %s complete", fetch_data->expected_checksum);

  if (!_ostree_fetcher_request_to_tmpfile_finish (fetcher, result, &temp_path, error))
    goto out;

  fd = openat (_ostree_fetcher_get_dfd (fetcher), temp_path, O_RDONLY | O_CLOEXEC);
  if (fd == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  /* From here on, if we fail to apply the delta, we'll re-fetch it */
  if (unlinkat (_ostree_fetcher_get_dfd (fetcher), temp_path, 0) < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  in = g_unix_input_stream_new (fd, FALSE);

  /* TODO - make async */
  if (!_ostree_static_delta_part_open (in, NULL, 0, fetch_data->expected_checksum,
                                       &part, pull_data->cancellable, error))
    goto out;

  _ostree_static_delta_part_execute_async (pull_data->repo,
                                           fetch_data->objects,
                                           part,
                                           pull_data->cancellable,
                                           on_static_delta_written,
                                           fetch_data);
  pull_data->n_outstanding_deltapart_write_requests++;
  free_fetch_data = FALSE;

 out:
  g_assert (pull_data->n_outstanding_deltapart_fetches > 0);
  pull_data->n_outstanding_deltapart_fetches--;
  pull_data->n_fetched_deltaparts++;
  check_outstanding_requests_handle_error (pull_data, &local_error);
  if (free_fetch_data)
    fetch_static_delta_data_free (fetch_data);
}

static gboolean
process_verify_result (OtPullData            *pull_data,
                       const char            *checksum,
                       OstreeGpgVerifyResult *result,
                       GError               **error)
{
  if (result == NULL)
    {
      g_prefix_error (error, "Commit %s: ", checksum);
      return FALSE;
    }

  /* Allow callers to output the results immediately. */
  g_signal_emit_by_name (pull_data->repo,
                         "gpg-verify-result",
                         checksum, result);

  if (!ostree_gpg_verify_result_require_valid_signature (result, error))
    {
      g_prefix_error (error, "Commit %s: ", checksum);
      return FALSE;
    }
  return TRUE;
}

static gboolean
gpg_verify_unwritten_commit (OtPullData         *pull_data,
                             const char         *checksum,
                             GVariant           *commit,
                             GVariant           *detached_metadata,
                             GCancellable       *cancellable,
                             GError            **error)
{
  if (pull_data->gpg_verify)
    {
      glnx_unref_object OstreeGpgVerifyResult *result = NULL;
      g_autoptr(GBytes) signed_data = g_variant_get_data_as_bytes (commit);

      if (!detached_metadata)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Commit %s: no detached metadata found for GPG verification",
                       checksum);
          return FALSE;
        }

      result = _ostree_repo_gpg_verify_with_metadata (pull_data->repo,
                                                      signed_data,
                                                      detached_metadata,
                                                      pull_data->remote_name,
                                                      NULL, NULL,
                                                      cancellable,
                                                      error);
      if (!process_verify_result (pull_data, checksum, result, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
commitstate_is_partial (OtPullData   *pull_data,
                        OstreeRepoCommitState commitstate)
{
  return pull_data->legacy_transaction_resuming
    || (commitstate & OSTREE_REPO_COMMIT_STATE_PARTIAL) > 0;
}

static gboolean
scan_commit_object (OtPullData         *pull_data,
                    const char         *checksum,
                    guint               recursion_depth,
                    GCancellable       *cancellable,
                    GError            **error)
{
  gboolean ret = FALSE;
  /* If we found a legacy transaction flag, assume we have to scan.
   * We always do a scan of dirtree objects; see
   * https://github.com/ostreedev/ostree/issues/543
   */
  OstreeRepoCommitState commitstate;
  g_autoptr(GVariant) commit = NULL;
  g_autoptr(GVariant) parent_csum = NULL;
  const guchar *parent_csum_bytes = NULL;
  gpointer depthp;
  gint depth;
  gboolean is_partial;

  if (recursion_depth > OSTREE_MAX_RECURSION)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exceeded maximum recursion");
      goto out;
    }

  if (g_hash_table_lookup_extended (pull_data->commit_to_depth, checksum,
                                    NULL, &depthp))
    {
      depth = GPOINTER_TO_INT (depthp);
    }
  else
    {
      depth = pull_data->maxdepth;
      g_hash_table_insert (pull_data->commit_to_depth, g_strdup (checksum),
                           GINT_TO_POINTER (depth));
    }

  if (pull_data->gpg_verify)
    {
      glnx_unref_object OstreeGpgVerifyResult *result = NULL;

      result = ostree_repo_verify_commit_for_remote (pull_data->repo,
                                                     checksum,
                                                     pull_data->remote_name,
                                                     cancellable,
                                                     error);
      if (!process_verify_result (pull_data, checksum, result, error))
        goto out;
    }

  if (!ostree_repo_load_commit (pull_data->repo, checksum, &commit, &commitstate, error))
    goto out;

  /* If we found a legacy transaction flag, assume all commits are partial */
  is_partial = commitstate_is_partial (pull_data, commitstate);

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  g_variant_get_child (commit, 1, "@ay", &parent_csum);
  if (g_variant_n_children (parent_csum) > 0)
    {
      parent_csum_bytes = ostree_checksum_bytes_peek_validate (parent_csum, error);
      if (parent_csum_bytes == NULL)
        goto out;
    }

  if (parent_csum_bytes != NULL && pull_data->maxdepth == -1)
    {
      queue_scan_one_metadata_object_c (pull_data, parent_csum_bytes,
                                        OSTREE_OBJECT_TYPE_COMMIT, NULL,
                                        recursion_depth + 1);
    }
  else if (parent_csum_bytes != NULL && depth > 0)
    {
      char parent_checksum[OSTREE_SHA256_STRING_LEN+1];
      gpointer parent_depthp;
      int parent_depth;

      ostree_checksum_inplace_from_bytes (parent_csum_bytes, parent_checksum);
  
      if (g_hash_table_lookup_extended (pull_data->commit_to_depth, parent_checksum,
                                        NULL, &parent_depthp))
        {
          parent_depth = GPOINTER_TO_INT (parent_depthp);
        }
      else
        {
          parent_depth = depth - 1;
        }

      if (parent_depth >= 0)
        {
          g_hash_table_insert (pull_data->commit_to_depth, g_strdup (parent_checksum),
                               GINT_TO_POINTER (parent_depth));
          queue_scan_one_metadata_object_c (pull_data, parent_csum_bytes,
                                            OSTREE_OBJECT_TYPE_COMMIT,
                                            NULL,
                                            recursion_depth + 1);
        }
    }

  /* We only recurse to looking whether we need dirtree/dirmeta
   * objects if the commit is partial, and we're not doing a
   * commit-only fetch.
   */
  if (is_partial && !pull_data->is_commit_only)
    {
      g_autoptr(GVariant) tree_contents_csum = NULL;
      g_autoptr(GVariant) tree_meta_csum = NULL;
      const guchar *tree_contents_csum_bytes;
      const guchar *tree_meta_csum_bytes;

      g_variant_get_child (commit, 6, "@ay", &tree_contents_csum);
      g_variant_get_child (commit, 7, "@ay", &tree_meta_csum);

      tree_contents_csum_bytes = ostree_checksum_bytes_peek_validate (tree_contents_csum, error);
      if (tree_contents_csum_bytes == NULL)
        goto out;

      tree_meta_csum_bytes = ostree_checksum_bytes_peek_validate (tree_meta_csum, error);
      if (tree_meta_csum_bytes == NULL)
        goto out;

      queue_scan_one_metadata_object_c (pull_data, tree_contents_csum_bytes,
                                        OSTREE_OBJECT_TYPE_DIR_TREE, "/", recursion_depth + 1);

      queue_scan_one_metadata_object_c (pull_data, tree_meta_csum_bytes,
                                        OSTREE_OBJECT_TYPE_DIR_META, NULL, recursion_depth + 1);
    }

  ret = TRUE;
 out:
  return ret;
}

static void
queue_scan_one_metadata_object (OtPullData         *pull_data,
                                const char         *csum,
                                OstreeObjectType    objtype,
                                const char         *path,
                                guint               recursion_depth)
{
  guchar buf[OSTREE_SHA256_DIGEST_LEN];
  ostree_checksum_inplace_to_bytes (csum, buf);
  queue_scan_one_metadata_object_c (pull_data, buf, objtype, path, recursion_depth);
}

static void
queue_scan_one_metadata_object_c (OtPullData         *pull_data,
                                  const guchar         *csum,
                                  OstreeObjectType    objtype,
                                  const char         *path,
                                  guint               recursion_depth)
{
  ScanObjectQueueData *scan_data = g_new0 (ScanObjectQueueData, 1);

  memcpy (scan_data->csum, csum, sizeof (scan_data->csum));
  scan_data->objtype = objtype;
  scan_data->path = g_strdup (path);
  scan_data->recursion_depth = recursion_depth;

  g_queue_push_tail (&pull_data->scan_object_queue, scan_data);
  ensure_idle_queued (pull_data);
}

static gboolean
scan_one_metadata_object_c (OtPullData         *pull_data,
                            const guchar         *csum,
                            OstreeObjectType    objtype,
                            const char         *path,
                            guint               recursion_depth,
                            GCancellable       *cancellable,
                            GError            **error)
{
  gboolean ret = FALSE;
  g_autoptr(GVariant) object = NULL;
  g_autofree char *tmp_checksum = NULL;
  gboolean is_requested;
  gboolean is_stored;

  tmp_checksum = ostree_checksum_from_bytes (csum);
  object = ostree_object_name_serialize (tmp_checksum, objtype);

  if (g_hash_table_lookup (pull_data->scanned_metadata, object))
    return TRUE;

  is_requested = g_hash_table_lookup (pull_data->requested_metadata, object) != NULL;
  if (!ostree_repo_has_object (pull_data->repo, objtype, tmp_checksum, &is_stored,
                               cancellable, error))
    goto out;

  if (pull_data->remote_repo_local)
    {
      if (!is_stored)
        {
          if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
            {
              if (!write_commitpartial_for (pull_data, tmp_checksum, error))
                goto out;
            }
          if (!ostree_repo_import_object_from_with_trust (pull_data->repo, pull_data->remote_repo_local,
                                                          objtype, tmp_checksum, !pull_data->is_untrusted,
                                                          cancellable, error))
            goto out;
        }
      is_stored = TRUE;
      is_requested = TRUE;
    }

  if (!is_stored && !is_requested)
    {
      gboolean do_fetch_detached;

      g_hash_table_add (pull_data->requested_metadata, g_variant_ref (object));

      do_fetch_detached = (objtype == OSTREE_OBJECT_TYPE_COMMIT);
      enqueue_one_object_request (pull_data, tmp_checksum, objtype, path, do_fetch_detached, FALSE);
    }
  else if (is_stored && objtype == OSTREE_OBJECT_TYPE_COMMIT)
    {
      /* For commits, always refetch detached metadata. */
      enqueue_one_object_request (pull_data, tmp_checksum, objtype, path, TRUE, TRUE);

      if (!scan_commit_object (pull_data, tmp_checksum, recursion_depth,
                               pull_data->cancellable, error))
        goto out;

      g_hash_table_add (pull_data->scanned_metadata, g_variant_ref (object));
      pull_data->n_scanned_metadata++;
    }
  else if (is_stored && objtype == OSTREE_OBJECT_TYPE_DIR_TREE)
    {
      if (!scan_dirtree_object (pull_data, tmp_checksum, path, recursion_depth,
                                pull_data->cancellable, error))
        goto out;

      g_hash_table_add (pull_data->scanned_metadata, g_variant_ref (object));
      pull_data->n_scanned_metadata++;
    }

  ret = TRUE;
 out:
  return ret;
}

static void
enqueue_one_object_request (OtPullData        *pull_data,
                            const char        *checksum,
                            OstreeObjectType   objtype,
                            const char        *path,
                            gboolean           is_detached_meta,
                            gboolean           object_is_stored)
{
  gboolean is_meta;
  FetchObjectData *fetch_data;

  is_meta = OSTREE_OBJECT_TYPE_IS_META (objtype);

  fetch_data = g_new0 (FetchObjectData, 1);
  fetch_data->pull_data = pull_data;
  fetch_data->object = ostree_object_name_serialize (checksum, objtype);
  fetch_data->path = g_strdup (path);
  fetch_data->is_detached_meta = is_detached_meta;
  fetch_data->object_is_stored = object_is_stored;

  if (is_meta)
    pull_data->n_requested_metadata++;
  else
    pull_data->n_requested_content++;

  /* Are too many requests are in flight? */
  if (fetcher_queue_is_full (pull_data))
    {
      g_debug ("queuing fetch of %s.%s%s", checksum,
               ostree_object_type_to_string (objtype),
               is_detached_meta ? " (detached)" : "");

      if (is_meta)
        {
          GVariant *objname = ostree_object_name_serialize (checksum, objtype);
          g_hash_table_insert (pull_data->pending_fetch_metadata, objname, fetch_data);
        }
      else
        {
          g_hash_table_insert (pull_data->pending_fetch_content, g_strdup (checksum), fetch_data);
        }
    }
  else
    {
      start_fetch (pull_data, fetch_data);
    }
}

static void
start_fetch (OtPullData *pull_data,
             FetchObjectData *fetch)
{
  gboolean is_meta;
  g_autofree char *obj_subpath = NULL;
  guint64 *expected_max_size_p;
  guint64 expected_max_size;
  const char *expected_checksum;
  OstreeObjectType objtype;
  GPtrArray *mirrorlist = NULL;

  ostree_object_name_deserialize (fetch->object, &expected_checksum, &objtype);
  is_meta = OSTREE_OBJECT_TYPE_IS_META (objtype);

  g_debug ("starting fetch of %s.%s%s", expected_checksum,
           ostree_object_type_to_string (objtype),
           fetch->is_detached_meta ? " (detached)" : "");

  is_meta = OSTREE_OBJECT_TYPE_IS_META (objtype);
  if (is_meta)
    pull_data->n_outstanding_metadata_fetches++;
  else
    pull_data->n_outstanding_content_fetches++;

  /* Override the path if we're trying to fetch the .commitmeta file first */
  if (fetch->is_detached_meta)
    {
      char buf[_OSTREE_LOOSE_PATH_MAX];
      _ostree_loose_path (buf, expected_checksum, OSTREE_OBJECT_TYPE_COMMIT_META, pull_data->remote_mode);
      obj_subpath = g_build_filename ("objects", buf, NULL);
      mirrorlist = pull_data->meta_mirrorlist;
    }
  else
    {
      obj_subpath = _ostree_get_relative_object_path (expected_checksum, objtype, TRUE);
      mirrorlist = pull_data->content_mirrorlist;
    }

  /* We may have determined maximum sizes from the summary file content; if so,
   * honor it. Otherwise, metadata has a baseline max size.
   */
  expected_max_size_p = fetch->is_detached_meta ? NULL : g_hash_table_lookup (pull_data->expected_commit_sizes, expected_checksum);
  if (expected_max_size_p)
    expected_max_size = *expected_max_size_p;
  else if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    expected_max_size = OSTREE_MAX_METADATA_SIZE;
  else
    expected_max_size = 0;

  _ostree_fetcher_request_to_tmpfile (pull_data->fetcher, mirrorlist,
                                      obj_subpath, expected_max_size,
                                      is_meta ? OSTREE_REPO_PULL_METADATA_PRIORITY
                                      : OSTREE_REPO_PULL_CONTENT_PRIORITY,
                                      pull_data->cancellable,
                                      is_meta ? meta_fetch_on_complete : content_fetch_on_complete, fetch);
}

static gboolean
load_remote_repo_config (OtPullData    *pull_data,
                         GKeyFile     **out_keyfile,
                         GCancellable  *cancellable,
                         GError       **error)
{
  gboolean ret = FALSE;
  g_autofree char *contents = NULL;
  GKeyFile *ret_keyfile = NULL;

  if (!fetch_mirrored_uri_contents_utf8_sync (pull_data->fetcher,
                                              pull_data->meta_mirrorlist,
                                              "config", &contents,
                                              cancellable, error))
    goto out;

  ret_keyfile = g_key_file_new ();
  if (!g_key_file_load_from_data (ret_keyfile, contents, strlen (contents),
                                  0, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_keyfile, &ret_keyfile);
 out:
  g_clear_pointer (&ret_keyfile, (GDestroyNotify) g_key_file_unref);
  return ret;
}

static gboolean
process_one_static_delta_fallback (OtPullData   *pull_data,
                                   gboolean      delta_byteswap,
                                   GVariant     *fallback_object,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  gboolean ret = FALSE;
  g_autoptr(GVariant) csum_v = NULL;
  g_autofree char *checksum = NULL;
  guint8 objtype_y;
  OstreeObjectType objtype;
  gboolean is_stored;
  guint64 compressed_size, uncompressed_size;

  g_variant_get (fallback_object, "(y@aytt)",
                 &objtype_y, &csum_v, &compressed_size, &uncompressed_size);
  if (!ostree_validate_structureof_objtype (objtype_y, error))
    goto out;
  if (!ostree_validate_structureof_csum_v (csum_v, error))
    goto out;

  compressed_size = maybe_swap_endian_u64 (delta_byteswap, compressed_size);
  uncompressed_size = maybe_swap_endian_u64 (delta_byteswap, uncompressed_size);

  pull_data->n_total_delta_fallbacks += 1;
  pull_data->total_deltapart_size += compressed_size;
  pull_data->total_deltapart_usize += uncompressed_size;

  objtype = (OstreeObjectType)objtype_y;
  checksum = ostree_checksum_from_bytes_v (csum_v);

  if (!ostree_repo_has_object (pull_data->repo, objtype, checksum,
                               &is_stored,
                               cancellable, error))
    goto out;

  if (is_stored)
    pull_data->fetched_deltapart_size += compressed_size;

  if (pull_data->dry_run)
    {
      ret = TRUE;
      goto out;
    }

  if (!is_stored)
    {
      /* The delta compiler never did this, there's no reason to support it */
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Found metadata object as fallback: %s.%s", checksum,
                       ostree_object_type_to_string (objtype));
          goto out;
        }
      else
        {
          if (!g_hash_table_lookup (pull_data->requested_content, checksum))
            {
              /* Mark this as requested, like we do in the non-delta path */
              g_hash_table_add (pull_data->requested_content, checksum);
              /* But also record it's a delta fallback object, so we can account
               * for it as logically part of the delta fetch.
               */
              g_hash_table_add (pull_data->requested_fallback_content, g_strdup (checksum));
              enqueue_one_object_request (pull_data, checksum, OSTREE_OBJECT_TYPE_FILE, NULL, FALSE, FALSE);
              checksum = NULL;  /* We transferred ownership to the requested_content hash */
            }
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static void
start_fetch_deltapart (OtPullData *pull_data,
                       FetchStaticDeltaData *fetch)
{
  g_autofree char *deltapart_path = _ostree_get_relative_static_delta_part_path (fetch->from_revision, fetch->to_revision, fetch->i);
  pull_data->n_outstanding_deltapart_fetches++;
  g_assert_cmpint (pull_data->n_outstanding_deltapart_fetches, <=, _OSTREE_MAX_OUTSTANDING_DELTAPART_REQUESTS);
  _ostree_fetcher_request_to_tmpfile (pull_data->fetcher,
                                      pull_data->content_mirrorlist,
                                      deltapart_path, fetch->size,
                                      OSTREE_FETCHER_DEFAULT_PRIORITY,
                                      pull_data->cancellable,
                                      static_deltapart_fetch_on_complete,
                                      fetch);
}

static gboolean
process_one_static_delta (OtPullData   *pull_data,
                          const char   *from_revision,
                          const char   *to_revision,
                          GVariant     *delta_superblock,
                          GCancellable *cancellable,
                          GError      **error)
{
  gboolean ret = FALSE;
  gboolean delta_byteswap;
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(GVariant) headers = NULL;
  g_autoptr(GVariant) fallback_objects = NULL;
  guint i, n;

  delta_byteswap = _ostree_delta_needs_byteswap (delta_superblock);

  /* Parsing OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT */
  metadata = g_variant_get_child_value (delta_superblock, 0);
  headers = g_variant_get_child_value (delta_superblock, 6);
  fallback_objects = g_variant_get_child_value (delta_superblock, 7);

  /* First process the fallbacks */
  n = g_variant_n_children (fallback_objects);
  for (i = 0; i < n; i++)
    {
      g_autoptr(GVariant) fallback_object =
        g_variant_get_child_value (fallback_objects, i);

      if (!process_one_static_delta_fallback (pull_data, delta_byteswap,
                                              fallback_object,
                                              cancellable, error))
        goto out;
    }

  /* Write the to-commit object */
  if (!pull_data->dry_run)
  {
    g_autoptr(GVariant) to_csum_v = NULL;
    g_autofree char *to_checksum = NULL;
    gboolean have_to_commit;

    to_csum_v = g_variant_get_child_value (delta_superblock, 3);
    if (!ostree_validate_structureof_csum_v (to_csum_v, error))
      goto out;
    to_checksum = ostree_checksum_from_bytes_v (to_csum_v);

    if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_COMMIT, to_checksum,
                                 &have_to_commit, cancellable, error))
      goto out;
    
    if (!have_to_commit)
      {
        FetchObjectData *fetch_data;
        g_autoptr(GVariant) to_commit = g_variant_get_child_value (delta_superblock, 4);
        g_autofree char *detached_path = _ostree_get_relative_static_delta_path (from_revision, to_revision, "commitmeta");
        g_autoptr(GVariant) detached_data = NULL;

        detached_data = g_variant_lookup_value (metadata, detached_path, G_VARIANT_TYPE("a{sv}"));

        if (!gpg_verify_unwritten_commit (pull_data, to_revision, to_commit, detached_data,
                                          cancellable, error))
          goto out;

        if (detached_data && !ostree_repo_write_commit_detached_metadata (pull_data->repo,
                                                                          to_revision,
                                                                          detached_data,
                                                                          cancellable,
                                                                          error))
          goto out;

        fetch_data = g_new0 (FetchObjectData, 1);
        fetch_data->pull_data = pull_data;
        fetch_data->object = ostree_object_name_serialize (to_checksum, OSTREE_OBJECT_TYPE_COMMIT);
        fetch_data->is_detached_meta = FALSE;
        fetch_data->object_is_stored = FALSE;

        ostree_repo_write_metadata_async (pull_data->repo, OSTREE_OBJECT_TYPE_COMMIT, to_checksum,
                                          to_commit,
                                          pull_data->cancellable,
                                          on_metadata_written, fetch_data);
        pull_data->n_outstanding_metadata_write_requests++;
      }
  }

  n = g_variant_n_children (headers);
  pull_data->n_total_deltaparts += n;
  
  for (i = 0; i < n; i++)
    {
      const guchar *csum;
      g_autoptr(GVariant) header = NULL;
      gboolean have_all = FALSE;
      g_autofree char *deltapart_path = NULL;
      FetchStaticDeltaData *fetch_data;
      g_autoptr(GVariant) csum_v = NULL;
      g_autoptr(GVariant) objects = NULL;
      g_autoptr(GBytes) inline_part_bytes = NULL;
      guint64 size, usize;
      guint32 version;

      header = g_variant_get_child_value (headers, i);
      g_variant_get (header, "(u@aytt@ay)", &version, &csum_v, &size, &usize, &objects);

      version = maybe_swap_endian_u32 (delta_byteswap, version);
      size = maybe_swap_endian_u64 (delta_byteswap, size);
      usize = maybe_swap_endian_u64 (delta_byteswap, usize);

      if (version > OSTREE_DELTAPART_VERSION)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Delta part has too new version %u", version);
          goto out;
        }

      csum = ostree_checksum_bytes_peek_validate (csum_v, error);
      if (!csum)
        goto out;

      if (!_ostree_repo_static_delta_part_have_all_objects (pull_data->repo,
                                                            objects,
                                                            &have_all,
                                                            cancellable, error))
        goto out;

      pull_data->total_deltapart_size += size;
      pull_data->total_deltapart_usize += usize;

      if (have_all)
        {
          g_debug ("Have all objects from static delta %s-%s part %u",
                   from_revision ? from_revision : "empty", to_revision,
                   i);
          pull_data->fetched_deltapart_size += size;
          pull_data->n_fetched_deltaparts++;
          continue;
        }

      deltapart_path = _ostree_get_relative_static_delta_part_path (from_revision, to_revision, i);

      { g_autoptr(GVariant) part_datav =
          g_variant_lookup_value (metadata, deltapart_path, G_VARIANT_TYPE ("(yay)"));

        if (part_datav)
          inline_part_bytes = g_variant_get_data_as_bytes (part_datav);
      }

      if (pull_data->dry_run)
        continue;
      
      fetch_data = g_new0 (FetchStaticDeltaData, 1);
      fetch_data->from_revision = g_strdup (from_revision);
      fetch_data->to_revision = g_strdup (to_revision);
      fetch_data->pull_data = pull_data;
      fetch_data->objects = g_variant_ref (objects);
      fetch_data->expected_checksum = ostree_checksum_from_bytes_v (csum_v);
      fetch_data->size = size;
      fetch_data->i = i;

      if (inline_part_bytes != NULL)
        {
          g_autoptr(GInputStream) memin = g_memory_input_stream_new_from_bytes (inline_part_bytes);
          g_autoptr(GVariant) inline_delta_part = NULL;

          /* For inline parts we are relying on per-commit GPG, so don't bother checksumming. */
          if (!_ostree_static_delta_part_open (memin, inline_part_bytes,
                                               OSTREE_STATIC_DELTA_OPEN_FLAGS_SKIP_CHECKSUM,
                                               NULL, &inline_delta_part,
                                               cancellable, error))
            goto out;

          _ostree_static_delta_part_execute_async (pull_data->repo,
                                                   fetch_data->objects,
                                                   inline_delta_part,
                                                   pull_data->cancellable,
                                                   on_static_delta_written,
                                                   fetch_data);
          pull_data->n_outstanding_deltapart_write_requests++;
        }
      else
        {
          if (!fetcher_queue_is_full (pull_data))
            start_fetch_deltapart (pull_data, fetch_data);
          else
            {
              g_hash_table_add (pull_data->pending_fetch_deltaparts, fetch_data);
            }
        }
    }

  ret = TRUE;
 out:
  return ret;
}

/* Loop over the static delta data we got from the summary,
 * and find the newest commit for @out_from_revision that
 * goes to @to_revision.
 *
 * Additionally, @out_have_scratch_delta will be set to %TRUE
 * if there is a %NULL → @to_revision delta, also known as
 * a "from scratch" delta.
 */
static gboolean
get_best_static_delta_start_for (OtPullData *pull_data,
                                 const char *to_revision,
                                 gboolean   *out_have_scratch_delta,
                                 char      **out_from_revision,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  GHashTableIter hiter;
  gpointer hkey, hvalue;
  /* Array<char*> of possible from checksums */
  g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func (g_free);
  const char *newest_candidate = NULL;
  guint64 newest_candidate_timestamp = 0;

  g_assert (pull_data->summary_deltas_checksums != NULL);
  g_hash_table_iter_init (&hiter, pull_data->summary_deltas_checksums);

  *out_have_scratch_delta = FALSE;

  /* Loop over all deltas known from the summary file,
   * finding ones which go to to_revision */
  while (g_hash_table_iter_next (&hiter, &hkey, &hvalue))
    {
      const char *delta_name = hkey;
      g_autofree char *cur_from_rev = NULL;
      g_autofree char *cur_to_rev = NULL;

      /* Gracefully handle corrupted (or malicious) summary files */
      if (!_ostree_parse_delta_name (delta_name, &cur_from_rev, &cur_to_rev, error))
        return FALSE;

      /* Is this the checksum we want? */
      if (strcmp (cur_to_rev, to_revision) != 0)
        continue;

      if (cur_from_rev)
        g_ptr_array_add (candidates, g_steal_pointer (&cur_from_rev));
      else
        *out_have_scratch_delta = TRUE;
    }

  /* Loop over our candidates, find the newest one */
  for (guint i = 0; i < candidates->len; i++)
    {
      const char *candidate = candidates->pdata[i];
      guint64 candidate_ts = 0;
      g_autoptr(GVariant) commit = NULL;
      OstreeRepoCommitState state;
      gboolean have_candidate;

      /* Do we have this commit at all?  If not, skip it */
      if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_COMMIT,
                                   candidate, &have_candidate,
                                   NULL, error))
        return FALSE;
      if (!have_candidate)
        continue;

      /* Load it */
      if (!ostree_repo_load_commit (pull_data->repo, candidate,
                                    &commit, &state, error))
        return FALSE;

      /* Ignore partial commits, we can't use them */
      if (state & OSTREE_REPO_COMMIT_STATE_PARTIAL)
        continue;

      /* Is it newer? */
      candidate_ts = ostree_commit_get_timestamp (commit);
      if (newest_candidate == NULL ||
          candidate_ts > newest_candidate_timestamp)
        {
          newest_candidate = candidate;
          newest_candidate_timestamp = candidate_ts;
        }
    }

  *out_from_revision = g_strdup (newest_candidate);
  return TRUE;
}

typedef struct {
  OtPullData *pull_data;
  char *from_revision;
  char *to_revision;
} FetchDeltaSuperData;

static void
set_required_deltas_error (GError **error,
                           const char *from_revision,
                           const char *to_revision)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "Static deltas required, but none found for %s to %s",
               from_revision, to_revision);
}

static void
on_superblock_fetched (GObject   *src,
                       GAsyncResult *res,
                       gpointer      data)

{
  FetchDeltaSuperData *fdata = data;
  OtPullData *pull_data = fdata->pull_data;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  g_autoptr(GBytes) delta_superblock_data = NULL;
  const char *from_revision = fdata->from_revision;
  const char *to_revision = fdata->to_revision;

  if (!_ostree_fetcher_request_to_membuf_finish ((OstreeFetcher*)src,
                                                 res,
                                                 &delta_superblock_data,
                                                 error))
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        goto out;
      g_clear_error (&local_error);

      if (pull_data->require_static_deltas)
        {
          set_required_deltas_error (error, from_revision, to_revision);
          goto out;
        }

      queue_scan_one_metadata_object (pull_data, to_revision, OSTREE_OBJECT_TYPE_COMMIT, NULL, 0);
    }
  else
    {
      g_autofree gchar *delta = NULL;
      g_autofree guchar *ret_csum = NULL;
      guchar *summary_csum;
      g_autoptr (GInputStream) summary_is = NULL;
      g_autoptr(GVariant) delta_superblock = NULL;

      summary_is = g_memory_input_stream_new_from_data (g_bytes_get_data (delta_superblock_data, NULL),
                                                        g_bytes_get_size (delta_superblock_data),
                                                        NULL);

      if (!ot_gio_checksum_stream (summary_is, &ret_csum, pull_data->cancellable, error))
        goto out;

      delta = g_strconcat (from_revision ? from_revision : "", from_revision ? "-" : "", to_revision, NULL);
      summary_csum = g_hash_table_lookup (pull_data->summary_deltas_checksums, delta);

      /* At this point we've GPG verified the data, so in theory
       * could trust that they provided the right data, but let's
       * make this a hard error.
       */
      if (pull_data->gpg_verify_summary && !summary_csum)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "GPG verification enabled, but no summary signatures found (use gpg-verify-summary=false in remote config to disable)");
          goto out;
        }

      if (summary_csum && memcmp (summary_csum, ret_csum, 32))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid checksum for static delta %s", delta);
          goto out;
        }

      delta_superblock = g_variant_ref_sink (g_variant_new_from_bytes ((GVariantType*)OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT,
                                                                       delta_superblock_data, FALSE));

      g_ptr_array_add (pull_data->static_delta_superblocks, g_variant_ref (delta_superblock));
      if (!process_one_static_delta (pull_data, from_revision, to_revision, delta_superblock,
                                     pull_data->cancellable, error))
        goto out;
    }

 out:
  g_free (fdata->from_revision);
  g_free (fdata->to_revision);
  g_free (fdata);
  g_assert (pull_data->n_outstanding_metadata_fetches > 0);
  pull_data->n_outstanding_metadata_fetches--;
  pull_data->n_fetched_metadata++;
  check_outstanding_requests_handle_error (pull_data, &local_error);
}

static gboolean
validate_variant_is_csum (GVariant       *csum,
                          GError        **error)
{
  gboolean ret = FALSE;

  if (!g_variant_is_of_type (csum, G_VARIANT_TYPE ("ay")))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid checksum variant of type '%s', expected 'ay'",
                   g_variant_get_type_string (csum));
      goto out;
    }

  if (!ostree_validate_structureof_csum_v (csum, error))
    goto out;
  
  ret = TRUE;
 out:
  return ret;
}

/* Load the summary from the cache if the provided .sig file is the same as the
   cached version.  */
static gboolean
_ostree_repo_load_cache_summary_if_same_sig (OstreeRepo        *self,
                                             const char        *remote,
                                             GBytes            *summary_sig,
                                             GBytes            **summary,
                                             GCancellable      *cancellable,
                                             GError           **error)
{
  gboolean ret = FALSE;
  const char *summary_cache_sig_file = glnx_strjoina (_OSTREE_SUMMARY_CACHE_DIR, "/", remote, ".sig");

  glnx_fd_close int prev_fd = -1;
  g_autoptr(GBytes) old_sig_contents = NULL;

  if (self->cache_dir_fd == -1)
    return TRUE;

  if (!ot_openat_ignore_enoent (self->cache_dir_fd, summary_cache_sig_file, &prev_fd, error))
    goto out;

  if (prev_fd < 0)
    {
      ret = TRUE;
      goto out;
    }

  old_sig_contents = glnx_fd_readall_bytes (prev_fd, cancellable, error);
  if (!old_sig_contents)
    goto out;

  if (g_bytes_compare (old_sig_contents, summary_sig) == 0)
    {
      const char *summary_cache_file = glnx_strjoina (_OSTREE_SUMMARY_CACHE_DIR, "/", remote);
      glnx_fd_close int summary_fd = -1;
      GBytes *summary_data;


      summary_fd = openat (self->cache_dir_fd, summary_cache_file, O_CLOEXEC | O_RDONLY);
      if (summary_fd < 0)
        {
          if (errno == ENOENT)
            {
              (void) unlinkat (self->cache_dir_fd, summary_cache_sig_file, 0);
              ret = TRUE;
              goto out;
            }

          glnx_set_error_from_errno (error);
          goto out;
        }

      summary_data = glnx_fd_readall_bytes (summary_fd, cancellable, error);
      if (!summary_data)
        goto out;
      *summary = summary_data;
    }
  ret = TRUE;

 out:
  return ret;
}

static gboolean
_ostree_repo_cache_summary (OstreeRepo        *self,
                            const char        *remote,
                            GBytes            *summary,
                            GBytes            *summary_sig,
                            GCancellable      *cancellable,
                            GError           **error)
{
  gboolean ret = FALSE;
  const char *summary_cache_file = glnx_strjoina (_OSTREE_SUMMARY_CACHE_DIR, "/", remote);
  const char *summary_cache_sig_file = glnx_strjoina (_OSTREE_SUMMARY_CACHE_DIR, "/", remote, ".sig");

  if (self->cache_dir_fd == -1)
    return TRUE;

  if (!glnx_shutil_mkdir_p_at (self->cache_dir_fd, _OSTREE_SUMMARY_CACHE_DIR, 0775, cancellable, error))
    goto out;

  if (!glnx_file_replace_contents_at (self->cache_dir_fd,
                                      summary_cache_file,
                                      g_bytes_get_data (summary, NULL),
                                      g_bytes_get_size (summary),
                                      self->disable_fsync ? GLNX_FILE_REPLACE_NODATASYNC : GLNX_FILE_REPLACE_DATASYNC_NEW,
                                      cancellable, error))
    goto out;

  if (!glnx_file_replace_contents_at (self->cache_dir_fd,
                                      summary_cache_sig_file,
                                      g_bytes_get_data (summary_sig, NULL),
                                      g_bytes_get_size (summary_sig),
                                      self->disable_fsync ? GLNX_FILE_REPLACE_NODATASYNC : GLNX_FILE_REPLACE_DATASYNC_NEW,
                                      cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;

}

static OstreeFetcher *
_ostree_repo_remote_new_fetcher (OstreeRepo  *self,
                                 const char  *remote_name,
                                 GError     **error)
{
  OstreeFetcher *fetcher = NULL;
  OstreeFetcherConfigFlags fetcher_flags = 0;
  gboolean tls_permissive = FALSE;
  gboolean success = FALSE;

  g_return_val_if_fail (OSTREE_IS_REPO (self), NULL);
  g_return_val_if_fail (remote_name != NULL, NULL);

  if (!ostree_repo_get_remote_boolean_option (self, remote_name,
                                              "tls-permissive", FALSE,
                                              &tls_permissive, error))
    goto out;

  if (tls_permissive)
    fetcher_flags |= OSTREE_FETCHER_FLAGS_TLS_PERMISSIVE;

  fetcher = _ostree_fetcher_new (self->tmp_dir_fd, remote_name, fetcher_flags);

  {
    g_autofree char *tls_client_cert_path = NULL;
    g_autofree char *tls_client_key_path = NULL;

    if (!ostree_repo_get_remote_option (self, remote_name,
                                        "tls-client-cert-path", NULL,
                                        &tls_client_cert_path, error))
      goto out;
    if (!ostree_repo_get_remote_option (self, remote_name,
                                        "tls-client-key-path", NULL,
                                        &tls_client_key_path, error))
      goto out;

    if ((tls_client_cert_path != NULL) != (tls_client_key_path != NULL))
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Remote \"%s\" must specify both "
                     "\"tls-client-cert-path\" and \"tls-client-key-path\"",
                     remote_name);
        goto out;
      }
    else if (tls_client_cert_path != NULL)
      {
        _ostree_fetcher_set_client_cert (fetcher, tls_client_cert_path, tls_client_key_path);
      }
  }

  {
    g_autofree char *tls_ca_path = NULL;

    if (!ostree_repo_get_remote_option (self, remote_name,
                                        "tls-ca-path", NULL,
                                        &tls_ca_path, error))
      goto out;

    if (tls_ca_path != NULL)
      {
        _ostree_fetcher_set_tls_database (fetcher, tls_ca_path);
      }
  }

  {
    g_autofree char *http_proxy = NULL;

    if (!ostree_repo_get_remote_option (self, remote_name,
                                        "proxy", NULL,
                                        &http_proxy, error))
      goto out;

    if (http_proxy != NULL)
      _ostree_fetcher_set_proxy (fetcher, http_proxy);
  }

  {
    g_autofree char *jar_path = NULL;
    g_autofree char *cookie_file = g_strdup_printf ("%s.cookies.txt",
                                                    remote_name);

    jar_path = g_build_filename (gs_file_get_path_cached (self->repodir), cookie_file,
                                 NULL);

    if (g_file_test(jar_path, G_FILE_TEST_IS_REGULAR))
      _ostree_fetcher_set_cookie_jar (fetcher, jar_path);

  }

  success = TRUE;

out:
  if (!success)
    g_clear_object (&fetcher);

  return fetcher;
}

static gboolean
_ostree_preload_metadata_file (OstreeRepo    *self,
                               OstreeFetcher *fetcher,
                               GPtrArray     *mirrorlist,
                               const char    *filename,
                               gboolean      is_metalink,
                               GBytes        **out_bytes,
                               GCancellable  *cancellable,
                               GError        **error)
{
  gboolean ret = FALSE;

  if (is_metalink)
    {
      glnx_unref_object OstreeMetalink *metalink = NULL;
      GError *local_error = NULL;

      /* the metalink uri is buried in the mirrorlist as the first (and only)
       * element */
      metalink = _ostree_metalink_new (fetcher, filename,
                                       OSTREE_MAX_METADATA_SIZE,
                                       mirrorlist->pdata[0]);

      _ostree_metalink_request_sync (metalink, NULL, out_bytes,
                                     cancellable, &local_error);

      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&local_error);
          *out_bytes = NULL;
        }
      else if (local_error != NULL)
        {
          g_propagate_error (error, local_error);
          goto out;
        }
    }
  else
    {
      ret = _ostree_fetcher_mirrored_request_to_membuf (fetcher, mirrorlist,
                                                        filename, FALSE, TRUE,
                                                        out_bytes,
                                                        OSTREE_MAX_METADATA_SIZE,
                                                        cancellable, error);

      if (!ret)
        goto out;
    }

  ret = TRUE;
out:
  return ret;
}

static gboolean
fetch_mirrorlist (OstreeFetcher  *fetcher,
                  const char     *mirrorlist_url,
                  GPtrArray     **out_mirrorlist,
                  GCancellable   *cancellable,
                  GError        **error)
{
  gboolean ret = FALSE;
  g_auto(GStrv) lines = NULL;
  g_autofree char *contents = NULL;
  g_autoptr(OstreeFetcherURI) mirrorlist = NULL;
  g_autoptr(GPtrArray) ret_mirrorlist =
    g_ptr_array_new_with_free_func ((GDestroyNotify) _ostree_fetcher_uri_free);

  mirrorlist = _ostree_fetcher_uri_parse (mirrorlist_url, error);
  if (!mirrorlist)
    goto out;

  if (!fetch_uri_contents_utf8_sync (fetcher, mirrorlist, &contents,
                                     cancellable, error))
    {
      g_prefix_error (error, "While fetching mirrorlist '%s': ",
                      mirrorlist_url);
      goto out;
    }

  /* go through each mirror in mirrorlist and do a quick sanity check that it
   * works so that we don't waste the fetcher's time when it goes through them
   * */
  lines = g_strsplit (contents, "\n", -1);
  g_debug ("Scanning mirrorlist from '%s'", mirrorlist_url);
  for (char **iter = lines; iter && *iter; iter++)
    {
      const char *mirror_uri_str = *iter;
      g_autoptr(OstreeFetcherURI) mirror_uri = NULL;
      g_autofree char *scheme = NULL;

      /* let's be nice and support empty lines and comments */
      if (*mirror_uri_str == '\0' || *mirror_uri_str == '#')
        continue;

      mirror_uri = _ostree_fetcher_uri_parse (mirror_uri_str, NULL);
      if (!mirror_uri)
        {
          g_debug ("Can't parse mirrorlist line '%s'", mirror_uri_str);
          continue;
        }

      scheme = _ostree_fetcher_uri_get_scheme (mirror_uri);
      if (!(g_str_equal (scheme, "http") || (g_str_equal (scheme, "https"))))
        {
          /* let's not support mirrorlists that contain non-http based URIs for
           * now (e.g. local URIs) -- we need to think about if and how we want
           * to support this since we set up things differently depending on
           * whether we're pulling locally or not */
          g_debug ("Ignoring non-http/s mirrorlist entry '%s'", mirror_uri_str);
          continue;
        }

      /* We keep sanity checking until we hit a working mirror; there's no need
       * to waste resources checking the remaining ones. At the same time,
       * guaranteeing that the first mirror in the list works saves the fetcher
       * time from always iterating through a few bad first mirrors. */
      if (ret_mirrorlist->len == 0)
        {
          GError *local_error = NULL;
          g_autoptr(OstreeFetcherURI) config_uri = _ostree_fetcher_uri_new_subpath (mirror_uri, "config");

          if (fetch_uri_contents_utf8_sync (fetcher, config_uri, NULL,
                                            cancellable, &local_error))
            g_ptr_array_add (ret_mirrorlist, g_steal_pointer (&mirror_uri));
          else
            {
              g_debug ("Failed to fetch config from mirror '%s': %s",
                       mirror_uri_str, local_error->message);
              g_clear_error (&local_error);
            }
        }
      else
        {
          g_ptr_array_add (ret_mirrorlist, g_steal_pointer (&mirror_uri));
        }
    }

  if (ret_mirrorlist->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No valid mirrors were found in mirrorlist '%s'",
                   mirrorlist_url);
      goto out;
    }

  *out_mirrorlist = g_steal_pointer (&ret_mirrorlist);
  ret = TRUE;

out:
  return ret;
}

static gboolean
repo_remote_fetch_summary (OstreeRepo    *self,
                           const char    *name,
                           const char    *metalink_url_string,
                           GVariant      *options,
                           GBytes       **out_summary,
                           GBytes       **out_signatures,
                           GCancellable  *cancellable,
                           GError       **error)
{
  glnx_unref_object OstreeFetcher *fetcher = NULL;
  g_autoptr(GMainContext) mainctx = NULL;
  gboolean ret = FALSE;
  gboolean from_cache = FALSE;
  const char *url_override = NULL;
  g_autoptr(GVariant) extra_headers = NULL;
  g_autoptr(GPtrArray) mirrorlist = NULL;

  if (options)
    {
      (void) g_variant_lookup (options, "override-url", "&s", &url_override);
      (void) g_variant_lookup (options, "http-headers", "@a(ss)", &extra_headers);
    }

  mainctx = g_main_context_new ();
  g_main_context_push_thread_default (mainctx);

  fetcher = _ostree_repo_remote_new_fetcher (self, name, error);
  if (fetcher == NULL)
    goto out;

  if (extra_headers)
    _ostree_fetcher_set_extra_headers (fetcher, extra_headers);

  {
    g_autofree char *url_string = NULL;
    if (metalink_url_string)
      url_string = g_strdup (metalink_url_string);
    else if (url_override)
      url_string = g_strdup (url_override);
    else if (!ostree_repo_remote_get_url (self, name, &url_string, error))
      goto out;

    if (metalink_url_string == NULL &&
        g_str_has_prefix (url_string, "mirrorlist="))
      {
        if (!fetch_mirrorlist (fetcher, url_string + strlen ("mirrorlist="),
                               &mirrorlist, cancellable, error))
          goto out;
      }
    else
      {
        g_autoptr(OstreeFetcherURI) uri = _ostree_fetcher_uri_parse (url_string, error);

        if (!uri)
          goto out;

        mirrorlist =
          g_ptr_array_new_with_free_func ((GDestroyNotify) _ostree_fetcher_uri_free);
        g_ptr_array_add (mirrorlist, g_steal_pointer (&uri));
      }
  }

  if (!_ostree_preload_metadata_file (self,
                                      fetcher,
                                      mirrorlist,
                                      "summary.sig",
                                      metalink_url_string ? TRUE : FALSE,
                                      out_signatures,
                                      cancellable,
                                      error))
    goto out;

  if (*out_signatures)
    {
      if (!_ostree_repo_load_cache_summary_if_same_sig (self,
                                                        name,
                                                        *out_signatures,
                                                        out_summary,
                                                        cancellable,
                                                        error))
        goto out;
    }

  if (*out_summary)
    from_cache = TRUE;
  else
    {
      if (!_ostree_preload_metadata_file (self,
                                          fetcher,
                                          mirrorlist,
                                          "summary",
                                          metalink_url_string ? TRUE : FALSE,
                                          out_summary,
                                          cancellable,
                                          error))
        goto out;
    }

  if (!from_cache && *out_summary && *out_signatures)
    {
      g_autoptr(GError) temp_error = NULL;

      if (!_ostree_repo_cache_summary (self,
                                       name,
                                       *out_summary,
                                       *out_signatures,
                                       cancellable,
                                       &temp_error))
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
            g_debug ("No permissions to save summary cache");
          else
            {
              g_propagate_error (error, g_steal_pointer (&temp_error));
              goto out;
            }
        }
    }

  ret = TRUE;

 out:
  if (mainctx)
    g_main_context_pop_thread_default (mainctx);
  return ret;
}

/* Create the fetcher by unioning options from the remote config, plus
 * any options specific to this pull (such as extra headers).
 */
static gboolean
reinitialize_fetcher (OtPullData *pull_data, const char *remote_name, GError **error)
{
  g_clear_object (&pull_data->fetcher);
  pull_data->fetcher = _ostree_repo_remote_new_fetcher (pull_data->repo, remote_name, error);
  if (pull_data->fetcher == NULL)
    return FALSE;

  if (pull_data->extra_headers)
    _ostree_fetcher_set_extra_headers (pull_data->fetcher, pull_data->extra_headers);

  return TRUE;
}

/* Start a request for a static delta */
static void
initiate_delta_request (OtPullData *pull_data,
                        const char *from_revision,
                        const char *to_revision)
{
  g_autofree char *delta_name =
    _ostree_get_relative_static_delta_superblock_path (from_revision, to_revision);
  FetchDeltaSuperData *fdata = g_new0(FetchDeltaSuperData, 1);
  fdata->pull_data = pull_data;
  fdata->from_revision = g_strdup (from_revision);
  fdata->to_revision = g_strdup (to_revision);

  _ostree_fetcher_request_to_membuf (pull_data->fetcher,
                                     pull_data->content_mirrorlist,
                                     delta_name, 0,
                                     OSTREE_MAX_METADATA_SIZE,
                                     0, pull_data->cancellable,
                                     on_superblock_fetched, fdata);
  pull_data->n_outstanding_metadata_fetches++;
  pull_data->n_requested_metadata++;
}

/* @ref - Optional ref name
 * @to_revision: Target commit revision we want to fetch
 *
 * Start a request for either a ref or a commit.  In the
 * ref case, we know both the name and the target commit.
 *
 * This function primarily handles the semantics around
 * `disable_static_deltas` and `require_static_deltas`.
 */
static gboolean
initiate_request (OtPullData *pull_data,
                  const char *ref,
                  const char *to_revision,
                  GError    **error)
{
  g_autofree char *delta_from_revision = NULL;

  /* Are deltas disabled?  OK, just start an object fetch and be done */
  if (pull_data->disable_static_deltas)
    {
      queue_scan_one_metadata_object (pull_data, to_revision, OSTREE_OBJECT_TYPE_COMMIT, NULL, 0);
      return TRUE;
    }

  /* If we have a summary, we can use the newer logic */
  if (pull_data->summary)
    {
      gboolean have_scratch_delta = FALSE;

      /* Look for a delta to @to_revision in the summary data */
      if (!get_best_static_delta_start_for (pull_data, to_revision,
                                            &have_scratch_delta, &delta_from_revision,
                                            pull_data->cancellable, error))
        return FALSE;

      if (delta_from_revision)   /* Did we find a delta FROM commit? */
        initiate_delta_request (pull_data, delta_from_revision, to_revision);
      else if (have_scratch_delta)    /* No delta FROM, do we have a scratch? */
        initiate_delta_request (pull_data, NULL, to_revision);
      else if (pull_data->require_static_deltas) /* No deltas found; are they required? */
        {
          set_required_deltas_error (error, ref, to_revision);
          return FALSE;
        }
      else /* No deltas, fall back to object fetches. */
        queue_scan_one_metadata_object (pull_data, to_revision, OSTREE_OBJECT_TYPE_COMMIT, NULL, 0);
    }
  else if (ref != NULL)
    {
      /* Are we doing a delta via a ref?  In that case we can fall back to the older
       * logic of just using the current tip of the ref as a delta FROM source. */
      if (!ostree_repo_resolve_rev (pull_data->repo, ref, TRUE,
                                    &delta_from_revision, error))
        return FALSE;

      /* Determine whether the from revision we have is partial; this
       * can happen if e.g. one uses `ostree pull --commit-metadata-only`.
       * This mirrors the logic in get_best_static_delta_start_for().
       */
      if (delta_from_revision)
        {
          OstreeRepoCommitState from_commitstate;

          if (!ostree_repo_load_commit (pull_data->repo, delta_from_revision, NULL,
                                        &from_commitstate, error))
            return FALSE;

          /* Was it partial?  Then we can't use it. */
          if (commitstate_is_partial (pull_data, from_commitstate))
            g_clear_pointer (&delta_from_revision, g_free);
        }

      /* This is similar to the below, except we *might* use the previous
       * commit, or we might do a scratch delta first.
       */
      initiate_delta_request (pull_data, delta_from_revision ?: NULL, to_revision);
    }
  else
    {
      /* Legacy path without a summary file - let's try a scratch delta, if that
       * doesn't work, it'll drop down to object requests.
       */
      initiate_delta_request (pull_data, NULL, to_revision);
    }

  return TRUE;
}

/* ------------------------------------------------------------------------------------------
 * Below is the libsoup-invariant API; these should match
 * the stub functions in the #else clause
 * ------------------------------------------------------------------------------------------
 */

/**
 * ostree_repo_pull_with_options:
 * @self: Repo
 * @remote_name_or_baseurl: Name of remote or file:// url
 * @options: A GVariant a{sv} with an extensible set of flags.
 * @progress: (allow-none): Progress
 * @cancellable: Cancellable
 * @error: Error
 *
 * Like ostree_repo_pull(), but supports an extensible set of flags.
 * The following are currently defined:
 *
 *   * refs (as): Array of string refs
 *   * flags (i): An instance of #OstreeRepoPullFlags
 *   * subdir (s): Pull just this subdirectory
 *   * subdirs (as): Pull just these subdirectories
 *   * override-remote-name (s): If local, add this remote to refspec
 *   * gpg-verify (b): GPG verify commits
 *   * gpg-verify-summary (b): GPG verify summary
 *   * depth (i): How far in the history to traverse; default is 0, -1 means infinite
 *   * disable-static-deltas (b): Do not use static deltas
 *   * require-static-deltas (b): Require static deltas
 *   * override-commit-ids (as): Array of specific commit IDs to fetch for refs
 *   * dry-run (b): Only print information on what will be downloaded (requires static deltas)
 *   * override-url (s): Fetch objects from this URL if remote specifies no metalink in options
 *   * inherit-transaction (b): Don't initiate, finish or abort a transaction, usefult to do multiple pulls in one transaction.
 *   * http-headers (a(ss)): Additional headers to add to all HTTP requests
 *   * update-frequency (u): Frequency to call the async progress callback in milliseconds, if any; only values higher than 0 are valid
 */
gboolean
ostree_repo_pull_with_options (OstreeRepo             *self,
                               const char             *remote_name_or_baseurl,
                               GVariant               *options,
                               OstreeAsyncProgress    *progress,
                               GCancellable           *cancellable,
                               GError                **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  g_autoptr(GBytes) bytes_summary = NULL;
  g_autofree char *metalink_url_str = NULL;
  g_autoptr(GHashTable) requested_refs_to_fetch = NULL;
  g_autoptr(GHashTable) commits_to_fetch = NULL;
  g_autofree char *remote_mode_str = NULL;
  glnx_unref_object OstreeMetalink *metalink = NULL;
  OtPullData pull_data_real = { 0, };
  OtPullData *pull_data = &pull_data_real;
  GKeyFile *remote_config = NULL;
  char **configured_branches = NULL;
  guint64 bytes_transferred;
  guint64 end_time;
  guint update_frequency = 0;
  OstreeRepoPullFlags flags = 0;
  const char *dir_to_pull = NULL;
  g_autofree char **dirs_to_pull = NULL;
  g_autofree char **refs_to_fetch = NULL;
  g_autofree char **override_commit_ids = NULL;
  GSource *update_timeout = NULL;
  gboolean opt_gpg_verify_set = FALSE;
  gboolean opt_gpg_verify_summary_set = FALSE;
  const char *url_override = NULL;
  gboolean inherit_transaction = FALSE;
  int i;

  if (options)
    {
      int flags_i = OSTREE_REPO_PULL_FLAGS_NONE;
      (void) g_variant_lookup (options, "refs", "^a&s", &refs_to_fetch);
      (void) g_variant_lookup (options, "flags", "i", &flags_i);
      /* Reduce risk of issues if enum happens to be 64 bit for some reason */
      flags = flags_i;
      (void) g_variant_lookup (options, "subdir", "&s", &dir_to_pull);
      (void) g_variant_lookup (options, "subdirs", "^a&s", &dirs_to_pull);
      (void) g_variant_lookup (options, "override-remote-name", "s", &pull_data->remote_name);
      opt_gpg_verify_set =
        g_variant_lookup (options, "gpg-verify", "b", &pull_data->gpg_verify);
      opt_gpg_verify_summary_set =
        g_variant_lookup (options, "gpg-verify-summary", "b", &pull_data->gpg_verify_summary);
      (void) g_variant_lookup (options, "depth", "i", &pull_data->maxdepth);
      (void) g_variant_lookup (options, "disable-static-deltas", "b", &pull_data->disable_static_deltas);
      (void) g_variant_lookup (options, "require-static-deltas", "b", &pull_data->require_static_deltas);
      (void) g_variant_lookup (options, "override-commit-ids", "^a&s", &override_commit_ids);
      (void) g_variant_lookup (options, "dry-run", "b", &pull_data->dry_run);
      (void) g_variant_lookup (options, "override-url", "&s", &url_override);
      (void) g_variant_lookup (options, "inherit-transaction", "b", &inherit_transaction);
      (void) g_variant_lookup (options, "http-headers", "@a(ss)", &pull_data->extra_headers);
      (void) g_variant_lookup (options, "update-frequency", "u", &update_frequency);
    }

  g_return_val_if_fail (pull_data->maxdepth >= -1, FALSE);
  if (refs_to_fetch && override_commit_ids)
    g_return_val_if_fail (g_strv_length (refs_to_fetch) == g_strv_length (override_commit_ids), FALSE);

  if (dir_to_pull)
    g_return_val_if_fail (dir_to_pull[0] == '/', FALSE);

  for (i = 0; dirs_to_pull != NULL && dirs_to_pull[i] != NULL; i++)
    g_return_val_if_fail (dirs_to_pull[i][0] == '/', FALSE);

  g_return_val_if_fail (!(pull_data->disable_static_deltas && pull_data->require_static_deltas), FALSE);

  /* We only do dry runs with static deltas, because we don't really have any
   * in-advance information for bare fetches.
   */
  g_return_val_if_fail (!pull_data->dry_run || pull_data->require_static_deltas, FALSE);

  pull_data->is_mirror = (flags & OSTREE_REPO_PULL_FLAGS_MIRROR) > 0;
  pull_data->is_commit_only = (flags & OSTREE_REPO_PULL_FLAGS_COMMIT_ONLY) > 0;
  pull_data->is_untrusted = (flags & OSTREE_REPO_PULL_FLAGS_UNTRUSTED) > 0;
  pull_data->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  if (error)
    pull_data->async_error = &pull_data->cached_async_error;
  else
    pull_data->async_error = NULL;
  pull_data->main_context = g_main_context_ref_thread_default ();
  pull_data->flags = flags;

  pull_data->repo = self;
  pull_data->progress = progress;

  pull_data->expected_commit_sizes = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                            (GDestroyNotify)g_free,
                                                            (GDestroyNotify)g_free);
  pull_data->commit_to_depth = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free,
                                                      NULL);
  pull_data->summary_deltas_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                               (GDestroyNotify)g_free,
                                                               (GDestroyNotify)g_free);
  pull_data->scanned_metadata = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                                       (GDestroyNotify)g_variant_unref, NULL);
  pull_data->requested_content = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                        (GDestroyNotify)g_free, NULL);
  pull_data->requested_fallback_content = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                                 (GDestroyNotify)g_free, NULL);
  pull_data->requested_metadata = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                                         (GDestroyNotify)g_variant_unref, NULL);
  pull_data->pending_fetch_content = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                            (GDestroyNotify)g_free,
                                                            (GDestroyNotify)fetch_object_data_free);
  pull_data->pending_fetch_metadata = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                                             (GDestroyNotify)g_variant_unref,
                                                             (GDestroyNotify)fetch_object_data_free);
  pull_data->pending_fetch_deltaparts = g_hash_table_new_full (NULL, NULL, (GDestroyNotify)fetch_static_delta_data_free, NULL);

  if (dir_to_pull != NULL || dirs_to_pull != NULL)
    {
      pull_data->dirs = g_ptr_array_new_with_free_func (g_free);
      if (dir_to_pull != NULL)
        g_ptr_array_add (pull_data->dirs, g_strdup (dir_to_pull));

      if (dirs_to_pull != NULL)
        {
          for (i = 0; dirs_to_pull[i] != NULL; i++)
            g_ptr_array_add (pull_data->dirs, g_strdup (dirs_to_pull[i]));
        }
    }

  g_queue_init (&pull_data->scan_object_queue);

  pull_data->start_time = g_get_monotonic_time ();

  if (_ostree_repo_remote_name_is_file (remote_name_or_baseurl))
    {
      /* For compatibility with pull-local, don't gpg verify local
       * pulls by default.
       */
      if ((pull_data->gpg_verify || pull_data->gpg_verify_summary) &&
          pull_data->remote_name == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Must specify remote name to enable gpg verification");
          goto out;
        }
    }
  else
    {
      g_autofree char *unconfigured_state = NULL;

      pull_data->remote_name = g_strdup (remote_name_or_baseurl);

      /* Fetch GPG verification settings from remote if it wasn't already
       * explicitly set in the options. */
      if (!opt_gpg_verify_set)
        if (!ostree_repo_remote_get_gpg_verify (self, pull_data->remote_name,
                                                &pull_data->gpg_verify, error))
          goto out;

      if (!opt_gpg_verify_summary_set)
        if (!ostree_repo_remote_get_gpg_verify_summary (self, pull_data->remote_name,
                                                        &pull_data->gpg_verify_summary, error))
          goto out;

      /* NOTE: If changing this, see the matching implementation in
       * ostree-sysroot-upgrader.c
       */
      if (!ostree_repo_get_remote_option (self, pull_data->remote_name,
                                          "unconfigured-state", NULL,
                                          &unconfigured_state,
                                          error))
        goto out;

      if (unconfigured_state)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "remote unconfigured-state: %s", unconfigured_state);
          goto out;
        }
    }

  pull_data->phase = OSTREE_PULL_PHASE_FETCHING_REFS;

  if (!reinitialize_fetcher (pull_data, remote_name_or_baseurl, error))
    goto out;

  pull_data->tmpdir_dfd = pull_data->repo->tmp_dir_fd;
  requested_refs_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  commits_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  if (!ostree_repo_get_remote_option (self,
                                      remote_name_or_baseurl, "metalink",
                                      NULL, &metalink_url_str, error))
    goto out;

  if (!metalink_url_str)
    {
      g_autofree char *baseurl = NULL;

      if (url_override != NULL)
        baseurl = g_strdup (url_override);
      else if (!ostree_repo_remote_get_url (self, remote_name_or_baseurl, &baseurl, error))
        goto out;

      if (g_str_has_prefix (baseurl, "mirrorlist="))
        {
          if (!fetch_mirrorlist (pull_data->fetcher,
                                 baseurl + strlen ("mirrorlist="),
                                 &pull_data->meta_mirrorlist,
                                 cancellable, error))
            goto out;
        }
      else
        {
          g_autoptr(OstreeFetcherURI) baseuri = _ostree_fetcher_uri_parse (baseurl, error);

          if (!baseuri)
            goto out;

          pull_data->meta_mirrorlist =
            g_ptr_array_new_with_free_func ((GDestroyNotify) _ostree_fetcher_uri_free);
          g_ptr_array_add (pull_data->meta_mirrorlist, g_steal_pointer (&baseuri));
        }
    }
  else
    {
      g_autoptr(GBytes) summary_bytes = NULL;
      g_autoptr(OstreeFetcherURI) metalink_uri = _ostree_fetcher_uri_parse (metalink_url_str, error);
      g_autoptr(OstreeFetcherURI) target_uri = NULL;

      if (!metalink_uri)
        goto out;

      metalink = _ostree_metalink_new (pull_data->fetcher, "summary",
                                       OSTREE_MAX_METADATA_SIZE, metalink_uri);

      if (! _ostree_metalink_request_sync (metalink,
                                           &target_uri,
                                           &summary_bytes,
                                           cancellable,
                                           error))
        goto out;

      /* XXX: would be interesting to implement metalink as another source of
       * mirrors here since we use it as such anyway (rather than the "usual"
       * use case of metalink, which is only for a single target filename) */
      {
        g_autofree char *path = _ostree_fetcher_uri_get_path (target_uri);
        g_autofree char *basepath = g_path_get_dirname (path);
        g_autoptr(OstreeFetcherURI) new_target_uri = _ostree_fetcher_uri_new_path (target_uri, basepath);
        pull_data->meta_mirrorlist =
          g_ptr_array_new_with_free_func ((GDestroyNotify) _ostree_fetcher_uri_free);
        g_ptr_array_add (pull_data->meta_mirrorlist, g_steal_pointer (&new_target_uri));
      }

      pull_data->summary = g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                                     summary_bytes, FALSE);
    }

  {
    g_autofree char *contenturl = NULL;

    if (metalink_url_str == NULL && url_override != NULL)
      contenturl = g_strdup (url_override);
    else if (!ostree_repo_get_remote_option (self, remote_name_or_baseurl,
                                             "contenturl", NULL,
                                             &contenturl, error))
      goto out;

    if (contenturl == NULL)
      {
        pull_data->content_mirrorlist =
          g_ptr_array_ref (pull_data->meta_mirrorlist);
      }
    else
      {
        if (g_str_has_prefix (contenturl, "mirrorlist="))
          {
            if (!fetch_mirrorlist (pull_data->fetcher,
                                   contenturl + strlen ("mirrorlist="),
                                   &pull_data->content_mirrorlist,
                                   cancellable, error))
              goto out;
          }
        else
          {
            g_autoptr(OstreeFetcherURI) contenturi = _ostree_fetcher_uri_parse (contenturl, error);

            if (!contenturi)
              goto out;

            pull_data->content_mirrorlist =
              g_ptr_array_new_with_free_func ((GDestroyNotify) _ostree_fetcher_uri_free);
            g_ptr_array_add (pull_data->content_mirrorlist,
                             g_steal_pointer (&contenturi));
          }
      }
  }

  if (!ostree_repo_get_remote_list_option (self,
                                           remote_name_or_baseurl, "branches",
                                           &configured_branches, error))
    goto out;

  /* TODO reindent later */
  { OstreeFetcherURI *first_uri = pull_data->meta_mirrorlist->pdata[0];
    g_autofree char *first_scheme = _ostree_fetcher_uri_get_scheme (first_uri);

  /* NB: we don't support local mirrors in mirrorlists, so if this passes, it
   * means that we're not using mirrorlists (see also fetch_mirrorlist()) */
  if (g_str_equal (first_scheme, "file"))
    {
      g_autofree char *path = _ostree_fetcher_uri_get_path (first_uri);
      g_autoptr(GFile) remote_repo_path = g_file_new_for_path (path);
      pull_data->remote_repo_local = ostree_repo_new (remote_repo_path);
      if (!ostree_repo_open (pull_data->remote_repo_local, cancellable, error))
        goto out;
    }
  else
    {
      if (!load_remote_repo_config (pull_data, &remote_config, cancellable, error))
        goto out;

      if (!ot_keyfile_get_value_with_default (remote_config, "core", "mode", "bare",
                                              &remote_mode_str, error))
        goto out;

      if (!ostree_repo_mode_from_string (remote_mode_str, &pull_data->remote_mode, error))
        goto out;

      if (!ot_keyfile_get_boolean_with_default (remote_config, "core", "tombstone-commits", FALSE,
                                                &pull_data->has_tombstone_commits, error))
        goto out;

      if (pull_data->remote_mode != OSTREE_REPO_MODE_ARCHIVE_Z2)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Can't pull from archives with mode \"%s\"",
                       remote_mode_str);
          goto out;
        }
    }
  }

  /* For local pulls, default to disabling static deltas so that the
   * exact object files are copied.
   */
  if (pull_data->remote_repo_local && !pull_data->require_static_deltas)
    pull_data->disable_static_deltas = TRUE;

  /* We can't use static deltas if pulling into an archive-z2 repo. */
  if (self->mode == OSTREE_REPO_MODE_ARCHIVE_Z2)
    {
      if (pull_data->require_static_deltas)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Can't use static deltas in an archive repo");
          goto out;
        }
      pull_data->disable_static_deltas = TRUE;
    }

  /* It's not efficient to use static deltas if all we want is the commit
   * metadata. */
  if (pull_data->is_commit_only)
    pull_data->disable_static_deltas = TRUE;

  pull_data->static_delta_superblocks = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  {
    g_autoptr(GBytes) bytes_sig = NULL;
    gsize i, n;
    g_autoptr(GVariant) refs = NULL;
    g_autoptr(GVariant) deltas = NULL;
    g_autoptr(GVariant) additional_metadata = NULL;
    gboolean summary_from_cache = FALSE;

    if (!pull_data->summary_data_sig)
      {
        if (!_ostree_fetcher_mirrored_request_to_membuf (pull_data->fetcher,
                                                         pull_data->meta_mirrorlist,
                                                         "summary.sig", FALSE, TRUE,
                                                         &bytes_sig,
                                                         OSTREE_MAX_METADATA_SIZE,
                                                         cancellable, error))
          goto out;
      }

    if (bytes_sig &&
        !pull_data->remote_repo_local &&
        !_ostree_repo_load_cache_summary_if_same_sig (self,
                                                      remote_name_or_baseurl,
                                                      bytes_sig,
                                                      &bytes_summary,
                                                      cancellable,
                                                      error))
      goto out;

    if (bytes_summary)
      summary_from_cache = TRUE;

    if (!pull_data->summary && !bytes_summary)
      {
        if (!_ostree_fetcher_mirrored_request_to_membuf (pull_data->fetcher,
                                                         pull_data->meta_mirrorlist,
                                                         "summary", FALSE, TRUE,
                                                         &bytes_summary,
                                                         OSTREE_MAX_METADATA_SIZE,
                                                         cancellable, error))
          goto out;
      }

    if (!bytes_summary && pull_data->gpg_verify_summary)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "GPG verification enabled, but no summary found (use gpg-verify-summary=false in remote config to disable)");
        goto out;
      }

    if (!bytes_summary && pull_data->require_static_deltas)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Fetch configured to require static deltas, but no summary found");
        goto out;
      }

    if (!bytes_sig && pull_data->gpg_verify_summary)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "GPG verification enabled, but no summary.sig found (use gpg-verify-summary=false in remote config to disable)");
        goto out;
      }

    if (bytes_summary)
      {
        pull_data->summary_data = g_bytes_ref (bytes_summary);
        pull_data->summary = g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, bytes_summary, FALSE);

        if (bytes_sig)
          pull_data->summary_data_sig = g_bytes_ref (bytes_sig);
      }


    if (!summary_from_cache && bytes_summary && bytes_sig)
      {
        if (!pull_data->remote_repo_local &&
            !_ostree_repo_cache_summary (self,
                                         remote_name_or_baseurl,
                                         bytes_summary,
                                         bytes_sig,
                                         cancellable,
                                         error))
          goto out;
      }

    if (pull_data->gpg_verify_summary && bytes_summary && bytes_sig)
      {
        g_autoptr(GVariant) sig_variant = NULL;
        glnx_unref_object OstreeGpgVerifyResult *result = NULL;

        sig_variant = g_variant_new_from_bytes (OSTREE_SUMMARY_SIG_GVARIANT_FORMAT, bytes_sig, FALSE);
        result = _ostree_repo_gpg_verify_with_metadata (self,
                                                        bytes_summary,
                                                        sig_variant,
                                                        pull_data->remote_name,
                                                        NULL,
                                                        NULL,
                                                        cancellable,
                                                        error);
        if (!ostree_gpg_verify_result_require_valid_signature (result, error))
          goto out;
      }

    if (pull_data->summary)
      {
        refs = g_variant_get_child_value (pull_data->summary, 0);
        n = g_variant_n_children (refs);
        for (i = 0; i < n; i++)
          {
            const char *refname;
            g_autoptr(GVariant) ref = g_variant_get_child_value (refs, i);

            g_variant_get_child (ref, 0, "&s", &refname);

            if (!ostree_validate_rev (refname, error))
              goto out;

            if (pull_data->is_mirror && !refs_to_fetch)
              g_hash_table_insert (requested_refs_to_fetch, g_strdup (refname), NULL);
          }

        additional_metadata = g_variant_get_child_value (pull_data->summary, 1);
        deltas = g_variant_lookup_value (additional_metadata, OSTREE_SUMMARY_STATIC_DELTAS, G_VARIANT_TYPE ("a{sv}"));
        n = deltas ? g_variant_n_children (deltas) : 0;
        for (i = 0; i < n; i++)
          {
            const char *delta;
            g_autoptr(GVariant) csum_v = NULL;
            guchar *csum_data = g_malloc (OSTREE_SHA256_DIGEST_LEN);
            g_autoptr(GVariant) ref = g_variant_get_child_value (deltas, i);

            g_variant_get_child (ref, 0, "&s", &delta);
            g_variant_get_child (ref, 1, "v", &csum_v);

            if (!validate_variant_is_csum (csum_v, error))
              goto out;

            memcpy (csum_data, ostree_checksum_bytes_peek (csum_v), 32);
            g_hash_table_insert (pull_data->summary_deltas_checksums,
                                 g_strdup (delta),
                                 csum_data);
          }
      }
  }

  if (pull_data->is_mirror && !refs_to_fetch && !configured_branches)
    {
      if (!bytes_summary)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Fetching all refs was requested in mirror mode, but remote repository does not have a summary");
          goto out;
        }

    } 
  else if (refs_to_fetch != NULL)
    {
      char **strviter = refs_to_fetch;
      char **commitid_strviter = override_commit_ids ? override_commit_ids : NULL;

      while (*strviter)
        {
          const char *branch = *strviter;

          if (ostree_validate_checksum_string (branch, NULL))
            {
              char *key = g_strdup (branch);
              g_hash_table_add (commits_to_fetch, key);
            }
          else
            {
              char *commitid = commitid_strviter ? g_strdup (*commitid_strviter) : NULL;
              g_hash_table_insert (requested_refs_to_fetch, g_strdup (branch), commitid);
            }
          
          strviter++;
          if (commitid_strviter)
            commitid_strviter++;
        }
    }
  else
    {
      char **branches_iter;

      branches_iter = configured_branches;

      if (!(branches_iter && *branches_iter))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No configured branches for remote %s", remote_name_or_baseurl);
          goto out;
        }
      for (;branches_iter && *branches_iter; branches_iter++)
        {
          const char *branch = *branches_iter;
              
          g_hash_table_insert (requested_refs_to_fetch, g_strdup (branch), NULL);
        }
    }

  g_hash_table_iter_init (&hash_iter, requested_refs_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *branch = key;
      const char *override_commitid = value;
      char *contents = NULL;

      /* Support specifying "" for an override commitid */
      if (override_commitid && *override_commitid)
        {
          g_hash_table_replace (requested_refs_to_fetch, g_strdup (branch), g_strdup (override_commitid));
        }
      else    
        {
          if (pull_data->summary)
            {
              gsize commit_size = 0;
              guint64 *malloced_size;

              if (!lookup_commit_checksum_from_summary (pull_data, branch, &contents, &commit_size, error))
                goto out;

              malloced_size = g_new0 (guint64, 1);
              *malloced_size = commit_size;
              g_hash_table_insert (pull_data->expected_commit_sizes, g_strdup (contents), malloced_size);
            }
          else
            {
              if (!fetch_ref_contents (pull_data, branch, &contents, cancellable, error))
                goto out;
            }
          /* Transfer ownership of contents */
          g_hash_table_replace (requested_refs_to_fetch, g_strdup (branch), contents);
        }
    }

  /* Create the state directory here - it's new with the commitpartial code,
   * and may not exist in older repositories.
   */
  if (mkdirat (pull_data->repo->repo_dir_fd, "state", 0777) != 0)
    {
      if (G_UNLIKELY (errno != EEXIST))
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  pull_data->phase = OSTREE_PULL_PHASE_FETCHING_OBJECTS;

  /* Now discard the previous fetcher, as it was bound to a temporary main context
   * for synchronous requests.
   */
  if (!reinitialize_fetcher (pull_data, remote_name_or_baseurl, error))
    goto out;

  pull_data->legacy_transaction_resuming = FALSE;
  if (!inherit_transaction &&
      !ostree_repo_prepare_transaction (pull_data->repo, &pull_data->legacy_transaction_resuming,
                                        cancellable, error))
    goto out;

  if (pull_data->legacy_transaction_resuming)
    g_debug ("resuming legacy transaction");

  /* Initiate requests for explicit commit revisions */
  g_hash_table_iter_init (&hash_iter, commits_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *commit = value;
      if (!initiate_request (pull_data, NULL, commit, error))
        goto out;
    }

  /* Initiate requests for refs */
  g_hash_table_iter_init (&hash_iter, requested_refs_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *ref = key;
      const char *to_revision = value;
      if (!initiate_request (pull_data, ref, to_revision, error))
        goto out;
    }

  if (pull_data->progress)
    {
      /* Setup a custom frequency if set */
      if (update_frequency > 0)
        update_timeout = g_timeout_source_new (pull_data->dry_run ? 0 : update_frequency);
      else
        update_timeout = g_timeout_source_new_seconds (pull_data->dry_run ? 0 : 1);

      g_source_set_priority (update_timeout, G_PRIORITY_HIGH);
      g_source_set_callback (update_timeout, update_progress, pull_data, NULL);
      g_source_attach (update_timeout, pull_data->main_context);
      g_source_unref (update_timeout);
    }

  /* Now await work completion */
  while (!pull_termination_condition (pull_data))
    g_main_context_iteration (pull_data->main_context, TRUE);

  if (pull_data->caught_error)
    goto out;

  if (pull_data->dry_run)
    {
      ret = TRUE;
      goto out;
    }
  
  g_assert_cmpint (pull_data->n_outstanding_metadata_fetches, ==, 0);
  g_assert_cmpint (pull_data->n_outstanding_metadata_write_requests, ==, 0);
  g_assert_cmpint (pull_data->n_outstanding_content_fetches, ==, 0);
  g_assert_cmpint (pull_data->n_outstanding_content_write_requests, ==, 0);

  g_hash_table_iter_init (&hash_iter, requested_refs_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *ref = key;
      const char *checksum = value;
      g_autofree char *remote_ref = NULL;
      g_autofree char *original_rev = NULL;
          
      if (pull_data->remote_name)
        remote_ref = g_strdup_printf ("%s/%s", pull_data->remote_name, ref);
      else
        remote_ref = g_strdup (ref);

      if (!ostree_repo_resolve_rev (pull_data->repo, remote_ref, TRUE, &original_rev, error))
        goto out;
          
      if (original_rev && strcmp (checksum, original_rev) == 0)
        {
        }
      else
        {
          ostree_repo_transaction_set_ref (pull_data->repo, pull_data->is_mirror ? NULL : pull_data->remote_name,
                                          ref, checksum);
        }
    }

  if (pull_data->is_mirror && pull_data->summary_data)
    {
      GLnxFileReplaceFlags replaceflag =
        pull_data->repo->disable_fsync ? GLNX_FILE_REPLACE_NODATASYNC : 0;
      gsize len;
      const guint8 *buf = g_bytes_get_data (pull_data->summary_data, &len);

      if (!glnx_file_replace_contents_at (pull_data->repo->repo_dir_fd, "summary",
                                          buf, len, replaceflag,
                                          cancellable, error))
        goto out;

      if (pull_data->summary_data_sig)
        {
          buf = g_bytes_get_data (pull_data->summary_data_sig, &len);
          if (!glnx_file_replace_contents_at (pull_data->repo->repo_dir_fd, "summary.sig",
                                              buf, len, replaceflag,
                                              cancellable, error))
            goto out;
        }
    }

  if (!inherit_transaction &&
      !ostree_repo_commit_transaction (pull_data->repo, NULL, cancellable, error))
    goto out;

  end_time = g_get_monotonic_time ();

  bytes_transferred = _ostree_fetcher_bytes_transferred (pull_data->fetcher);
  if (bytes_transferred > 0 && pull_data->progress)
    {
      guint shift;
      g_autoptr(GString) buf = g_string_new ("");

      /* Ensure the rest of the progress keys are set appropriately. */
      update_progress (pull_data);

      if (bytes_transferred < 1024)
        shift = 1;
      else
        shift = 1024;

      if (pull_data->n_fetched_deltaparts > 0)
        g_string_append_printf (buf, "%u delta parts, %u loose fetched",
                                pull_data->n_fetched_deltaparts,
                                pull_data->n_fetched_metadata + pull_data->n_fetched_content);
      else
        g_string_append_printf (buf, "%u metadata, %u content objects fetched",
                                pull_data->n_fetched_metadata, pull_data->n_fetched_content);

      g_string_append_printf (buf, "; %" G_GUINT64_FORMAT " %s transferred in %u seconds",
                              (guint64)(bytes_transferred / shift),
                              shift == 1 ? "B" : "KiB",
                              (guint) ((end_time - pull_data->start_time) / G_USEC_PER_SEC));

      ostree_async_progress_set_status (pull_data->progress, buf->str);
    }

  /* iterate over commits fetched and delete any commitpartial files */
  if (pull_data->dirs == NULL && !pull_data->is_commit_only)
    {
      g_hash_table_iter_init (&hash_iter, requested_refs_to_fetch);
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *checksum = value;
          g_autofree char *commitpartial_path = _ostree_get_commitpartial_path (checksum);

          if (!ot_ensure_unlinked_at (pull_data->repo->repo_dir_fd, commitpartial_path, 0))
            goto out;
        }
        g_hash_table_iter_init (&hash_iter, commits_to_fetch);
        while (g_hash_table_iter_next (&hash_iter, &key, &value))
          {
            const char *commit = value;
            g_autofree char *commitpartial_path = _ostree_get_commitpartial_path (commit);

            if (!ot_ensure_unlinked_at (pull_data->repo->repo_dir_fd, commitpartial_path, 0))
              goto out;
          }
    }

  ret = TRUE;
 out:
  /* This is pretty ugly - we have two error locations, because we
   * have a mix of synchronous and async code.  Mixing them gets messy
   * as we need to avoid overwriting errors.
   */
  if (pull_data->cached_async_error && error && !*error)
    g_propagate_error (error, pull_data->cached_async_error);
  else
    g_clear_error (&pull_data->cached_async_error);

  if (!inherit_transaction)
    ostree_repo_abort_transaction (pull_data->repo, cancellable, NULL);
  g_main_context_unref (pull_data->main_context);
  if (update_timeout)
    g_source_destroy (update_timeout);
  g_strfreev (configured_branches);
  g_clear_object (&pull_data->fetcher);
  g_clear_pointer (&pull_data->extra_headers, (GDestroyNotify)g_variant_unref);
  g_clear_object (&pull_data->cancellable);
  g_clear_object (&pull_data->remote_repo_local);
  g_free (pull_data->remote_name);
  g_clear_pointer (&pull_data->meta_mirrorlist, (GDestroyNotify) g_ptr_array_unref);
  g_clear_pointer (&pull_data->content_mirrorlist, (GDestroyNotify) g_ptr_array_unref);
  g_clear_pointer (&pull_data->summary_data, (GDestroyNotify) g_bytes_unref);
  g_clear_pointer (&pull_data->summary_data_sig, (GDestroyNotify) g_bytes_unref);
  g_clear_pointer (&pull_data->summary, (GDestroyNotify) g_variant_unref);
  g_clear_pointer (&pull_data->static_delta_superblocks, (GDestroyNotify) g_ptr_array_unref);
  g_clear_pointer (&pull_data->commit_to_depth, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->expected_commit_sizes, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->scanned_metadata, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->summary_deltas_checksums, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_content, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_fallback_content, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_metadata, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->pending_fetch_content, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->pending_fetch_metadata, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->pending_fetch_deltaparts, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->idle_src, (GDestroyNotify) g_source_destroy);
  g_clear_pointer (&pull_data->dirs, (GDestroyNotify) g_ptr_array_unref);
  g_clear_pointer (&remote_config, (GDestroyNotify) g_key_file_unref);
  return ret;
}

/**
 * ostree_repo_remote_fetch_summary_with_options:
 * @self: Self
 * @name: name of a remote
 * @options: (nullable): A GVariant a{sv} with an extensible set of flags
 * @out_summary: (out) (optional): return location for raw summary data, or
 *               %NULL
 * @out_signatures: (out) (optional): return location for raw summary
 *                  signature data, or %NULL
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Like ostree_repo_remote_fetch_summary(), but supports an extensible set of flags.
 * The following are currently defined:
 *
 * - override-url (s): Fetch summary from this URL if remote specifies no metalink in options
 * - http-headers (a(ss)): Additional headers to add to all HTTP requests
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
ostree_repo_remote_fetch_summary_with_options (OstreeRepo    *self,
                                               const char    *name,
                                               GVariant      *options,
                                               GBytes       **out_summary,
                                               GBytes       **out_signatures,
                                               GCancellable  *cancellable,
                                               GError       **error)
{
  g_autofree char *metalink_url_string = NULL;
  g_autoptr(GBytes) summary = NULL;
  g_autoptr(GBytes) signatures = NULL;
  gboolean ret = FALSE;
  gboolean gpg_verify_summary;

  g_return_val_if_fail (OSTREE_REPO (self), FALSE);
  g_return_val_if_fail (name != NULL, FALSE);

  if (!ostree_repo_get_remote_option (self, name, "metalink", NULL,
                                      &metalink_url_string, error))
    goto out;

  if (!repo_remote_fetch_summary (self,
                                  name,
                                  metalink_url_string,
                                  options,
                                  &summary,
                                  &signatures,
                                  cancellable,
                                  error))
    goto out;

  if (!ostree_repo_remote_get_gpg_verify_summary (self, name, &gpg_verify_summary, error))
    goto out;

  if (gpg_verify_summary && signatures == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "GPG verification enabled, but no summary signatures found (use gpg-verify-summary=false in remote config to disable)");
      goto out;
    }

  /* Verify any summary signatures. */
  if (gpg_verify_summary && summary != NULL && signatures != NULL)
    {
      glnx_unref_object OstreeGpgVerifyResult *result = NULL;

      result = ostree_repo_verify_summary (self,
                                           name,
                                           summary,
                                           signatures,
                                           cancellable,
                                           error);
      if (!ostree_gpg_verify_result_require_valid_signature (result, error))
        goto out;
    }

  if (out_summary != NULL)
    *out_summary = g_steal_pointer (&summary);

  if (out_signatures != NULL)
    *out_signatures = g_steal_pointer (&signatures);

  ret = TRUE;

out:
  return ret;
}

#else /* HAVE_LIBCURL_OR_LIBSOUP */

gboolean
ostree_repo_pull_with_options (OstreeRepo             *self,
                               const char             *remote_name_or_baseurl,
                               GVariant               *options,
                               OstreeAsyncProgress    *progress,
                               GCancellable           *cancellable,
                               GError                **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "This version of ostree was built without libsoup, and cannot fetch over HTTP");
  return FALSE;
}

gboolean
ostree_repo_remote_fetch_summary_with_options (OstreeRepo    *self,
                                               const char    *name,
                                               GVariant      *options,
                                               GBytes       **out_summary,
                                               GBytes       **out_signatures,
                                               GCancellable  *cancellable,
                                               GError       **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "This version of ostree was built without libsoup, and cannot fetch over HTTP");
  return FALSE;
}

#endif /* HAVE_LIBCURL_OR_LIBSOUP */
