/*
 * Copyright © 2017 Endless Mobile, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
 */

#pragma once

#include "ostree-repo-private.h"
#include "ostree-fetcher-util.h"
#include "ostree-remote-private.h"

G_BEGIN_DECLS

typedef enum {
  OSTREE_FETCHER_SECURITY_STATE_CA_PINNED,
  OSTREE_FETCHER_SECURITY_STATE_TLS,
  OSTREE_FETCHER_SECURITY_STATE_INSECURE,
} OstreeFetcherSecurityState;

typedef struct {
  OstreeRepo   *repo;
  int           tmpdir_dfd;
  OstreeRepoPullFlags flags;
  char          *remote_name;
  char          *remote_refspec_name;
  OstreeRepoMode remote_mode;
  OstreeFetcher *fetcher;
  OstreeFetcherSecurityState fetcher_security_state;

  GPtrArray     *meta_mirrorlist;    /* List of base URIs for fetching metadata */
  GPtrArray     *content_mirrorlist; /* List of base URIs for fetching content */
  OstreeRepo   *remote_repo_local;
  GPtrArray    *localcache_repos; /* Array<OstreeRepo> */

  GMainContext    *main_context;
  GCancellable *cancellable;
  OstreeAsyncProgress *progress;

  GVariant         *extra_headers;
  char             *append_user_agent;

  gboolean      dry_run;
  gboolean      dry_run_emitted_progress;
  gboolean      legacy_transaction_resuming;
  guint         n_network_retries;
  enum {
    OSTREE_PULL_PHASE_FETCHING_REFS,
    OSTREE_PULL_PHASE_FETCHING_OBJECTS
  }             phase;
  gint          n_scanned_metadata;

  gboolean          gpg_verify;
  gboolean          gpg_verify_summary;
  gboolean          sign_verify;
  gboolean          sign_verify_summary;
  gboolean          require_static_deltas;
  gboolean          disable_static_deltas;
  gboolean          has_tombstone_commits;

  GBytes           *summary_data;
  GBytes           *summary_data_sig;
  GVariant         *summary;
  GHashTable       *summary_deltas_checksums;
  GHashTable       *ref_original_commits; /* Maps checksum to commit, used by timestamp checks */
  GHashTable       *verified_commits; /* Set<checksum> of commits that have been verified */
  GHashTable       *ref_keyring_map; /* Maps OstreeCollectionRef to keyring remote name */
  GPtrArray        *static_delta_superblocks;
  GHashTable       *expected_commit_sizes; /* Maps commit checksum to known size */
  GHashTable       *commit_to_depth; /* Maps commit checksum maximum depth */
  GHashTable       *scanned_metadata; /* Maps object name to itself */
  GHashTable       *fetched_detached_metadata; /* Map<checksum,GVariant> */
  GHashTable       *requested_metadata; /* Maps object name to itself */
  GHashTable       *requested_content; /* Maps checksum to itself */
  GHashTable       *requested_fallback_content; /* Maps checksum to itself */
  GHashTable       *pending_fetch_metadata; /* Map<ObjectName,FetchObjectData> */
  GHashTable       *pending_fetch_content; /* Map<checksum,FetchObjectData> */
  GHashTable       *pending_fetch_delta_superblocks; /* Set<FetchDeltaSuperData> */
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
  /* Objects imported via hardlink/reflink/copying or  --localcache-repo*/
  guint             n_imported_metadata;
  guint             n_imported_content;

  gboolean          timestamp_check; /* Verify commit timestamps */
  int               maxdepth;
  guint64           max_metadata_size;
  guint64           start_time;

  gboolean          is_mirror;
  gboolean          trusted_http_direct;
  gboolean          is_commit_only;
  OstreeRepoImportFlags importflags;

  GPtrArray        *signapi_verifiers;

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

GPtrArray *
_signapi_verifiers_for_remote (OstreeRepo *repo,
                               const char *remote_name,
                               GError    **error);

gboolean
_sign_verify_for_remote (GPtrArray *signers,
                         GBytes *signed_data,
                         GVariant *metadata,
                         GError **error);

gboolean
_verify_unwritten_commit (OtPullData                 *pull_data,
                          const char                 *checksum,
                          GVariant                   *commit,
                          GVariant                   *detached_metadata,
                          const OstreeCollectionRef  *ref,
                          GCancellable               *cancellable,
                          GError                    **error);

gboolean
_process_gpg_verify_result (OtPullData            *pull_data,
                            const char            *checksum,
                            OstreeGpgVerifyResult *result,
                            GError               **error);

G_END_DECLS
