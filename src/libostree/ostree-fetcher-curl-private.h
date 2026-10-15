/*
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

/* Internals of the curl fetcher, for code that extends it from another file. */

#pragma once

#include <curl/curl.h>
#include <stdbool.h>

#include "libglnx.h"
#include "ostree-fetcher.h"

G_BEGIN_DECLS

struct OstreeFetcher
{
  GObject parent_instance;

  OstreeFetcherConfigFlags config_flags;
  char *remote_name;
  char *tls_ca_db_path;
  char *tls_client_cert_path;
  char *tls_client_key_path;
  char *cookie_jar_path;
  char *proxy;
  struct curl_slist *extra_headers;
  int tmpdir_dfd;
  bool force_anonymous;
  bool finalizing; // Set if we're in the process of teardown
  char *custom_user_agent;
  guint32 opt_low_speed_limit;
  guint32 opt_low_speed_time;
  gboolean opt_retry_all;
  guint32 opt_max_outstanding_fetcher_requests;

  GMainContext *mainctx;
  CURLM *multi;
  GSource *timer_event;
  int curl_running;
  GHashTable *outstanding_requests; /* Set<GTask> */
  GHashTable *sockets;              /* Set<SockInfo> */

  guint64 bytes_transferred;
};

/* Information associated with a request */
typedef struct FetcherRequest
{
  guint refcount;
  GPtrArray *mirrorlist;
  guint idx;

  char *filename;
  guint64 current_size;
  guint64 max_size;
  OstreeFetcherRequestFlags flags;
  struct curl_slist *req_headers;
  char *if_none_match;       /* request ETag */
  guint64 if_modified_since; /* seconds since the epoch */
  gboolean is_membuf;
  GError *caught_write_error;
  GLnxTmpfile tmpf;
  GString *output_buf;
  gboolean out_not_modified; /* TRUE if the server gave a HTTP 304 Not Modified response, which we
                                don’t propagate as an error */
  char *out_etag;            /* response ETag */
  guint64 out_last_modified; /* response Last-Modified, seconds since the epoch */

  CURL *easy;
  char error[CURL_ERROR_SIZE];

  OstreeFetcher *fetcher;
} FetcherRequest;

FetcherRequest *_ostree_fetcher_request_new (OstreeFetcher *self, GPtrArray *mirrorlist,
                                             const char *filename, OstreeFetcherRequestFlags flags);
GTask *_ostree_fetcher_request_task_new (OstreeFetcher *self, FetcherRequest *req, int priority,
                                         GCancellable *cancellable, GAsyncReadyCallback callback,
                                         gpointer user_data);
void _ostree_fetcher_request_finished (OstreeFetcher *self, GTask *task);
gboolean _ostree_fetcher_request_ensure_tmpfile (FetcherRequest *req, GError **error);
void _ostree_fetcher_request_initiate (FetcherRequest *req, GTask *task);

G_END_DECLS
