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

/* Shared between ostree-fetcher-curl.c and its s3:// support in
 * ostree-fetcher-curl-s3.c. */

#pragma once

#include <curl/curl.h>
#include <stdbool.h>

#include "libglnx.h"
#include "ostree-fetcher.h"

G_BEGIN_DECLS

typedef struct OstreeFetcherS3 OstreeFetcherS3; /* s3:// state of a fetcher */
typedef struct OstreeS3Request OstreeS3Request; /* s3:// state of a request */

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

  OstreeFetcherS3 *s3; /* NULL without HAVE_AWS */
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
  /* Returned and forgotten (_ostree_fetcher_request_finished()). Not
   * g_task_get_completed(), which stays FALSE until the callback has run,
   * and that is deferred to an idle once the task's cancellable is
   * cancelled. */
  gboolean finished;

  gboolean is_s3;      /* the current attempt targets an s3:// mirror */
  OstreeS3Request *s3; /* created by the first such attempt */
} FetcherRequest;

FetcherRequest *_ostree_fetcher_request_new (OstreeFetcher *self, GPtrArray *mirrorlist,
                                             const char *filename, OstreeFetcherRequestFlags flags);
GTask *_ostree_fetcher_request_task_new (OstreeFetcher *self, FetcherRequest *req, int priority,
                                         GCancellable *cancellable, GAsyncReadyCallback callback,
                                         gpointer user_data);
void _ostree_fetcher_request_finished (OstreeFetcher *self, GTask *task);
void _ostree_fetcher_request_fail_later (OstreeFetcher *self, GTask *task, GError *error);
gboolean _ostree_fetcher_request_ensure_tmpfile (FetcherRequest *req, GError **error);
void _ostree_fetcher_request_initiate (FetcherRequest *req, GTask *task);

/* s3:// hooks, called by ostree-fetcher-curl.c; all but the first three
 * only for requests with is_s3 set. */
#ifdef HAVE_AWS

OstreeFetcherS3 *_ostree_fetcher_s3_new (void);
void _ostree_fetcher_s3_free (OstreeFetcherS3 *s3);
void _ostree_s3_request_free (OstreeS3Request *s3);

/* Sign an attempt and add any Range headers. Returns FALSE if the request
 * is now waiting for credentials or has been failed; otherwise *out_url is
 * the URL to fetch. */
gboolean _ostree_fetcher_s3_begin_attempt (FetcherRequest *req, GTask *task, GUri *baseuri,
                                           char **out_url);
/* For the write callback. Returns TRUE if it dealt with the data, with the
 * value to give curl in *out_ret. */
gboolean _ostree_fetcher_s3_write (FetcherRequest *req, GTask *task, const void *ptr,
                                   size_t realsize, size_t *out_ret);
/* For the header callback, with one NUL-terminated header line. */
void _ostree_fetcher_s3_header (FetcherRequest *req, const char *line);
/* Whether the transfer was aborted to fetch the object again, whole. */
gboolean _ostree_fetcher_s3_restarting (FetcherRequest *req);
/* For a non-2xx, non-304 response: either set *out_reinitiate (retry, on
 * this mirror or the next) or complete @task with an error. */
void _ostree_fetcher_s3_complete_error (OstreeFetcher *self, FetcherRequest *req, GTask *task,
                                        const char *eff_url, long response, gboolean retry_all,
                                        gboolean *out_reinitiate);
/* For a 2xx response. Returns FALSE if @task should be completed as an
 * ordinary download; TRUE if it was completed here, or (*out_waiting) must
 * be completed by _ostree_fetcher_s3_complete_after_ranges() once the
 * transfer is removed from curl. */
gboolean _ostree_fetcher_s3_transfer_done (FetcherRequest *req, GTask *task, const char *eff_url,
                                           long response, gboolean *out_waiting);
void _ostree_fetcher_s3_complete_after_ranges (OstreeFetcher *self, GTask *task);
/* Fail every range of @req's object that is still running or about to
 * start. For any request; does nothing unless it has ranges. */
void _ostree_fetcher_s3_cancel_ranges (OstreeFetcher *self, FetcherRequest *req);

#else /* !HAVE_AWS */

static inline OstreeFetcherS3 *
_ostree_fetcher_s3_new (void)
{
  return NULL;
}

static inline void
_ostree_fetcher_s3_free (OstreeFetcherS3 *s3)
{
}

static inline void
_ostree_s3_request_free (OstreeS3Request *s3)
{
}

/* Without AWS support an s3:// request fails before reaching curl, so the
 * other hooks are never called. */
static inline gboolean
_ostree_fetcher_s3_begin_attempt (FetcherRequest *req, GTask *task, GUri *baseuri, char **out_url)
{
  _ostree_fetcher_request_fail_later (req->fetcher, task,
                                      g_error_new (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                                   "this libostree was built without S3 support"));
  return FALSE;
}

static inline gboolean
_ostree_fetcher_s3_write (FetcherRequest *req, GTask *task, const void *ptr, size_t realsize,
                          size_t *out_ret)
{
  g_assert_not_reached ();
  return FALSE;
}

static inline void
_ostree_fetcher_s3_header (FetcherRequest *req, const char *line)
{
  g_assert_not_reached ();
}

static inline gboolean
_ostree_fetcher_s3_restarting (FetcherRequest *req)
{
  g_assert_not_reached ();
  return FALSE;
}

static inline void
_ostree_fetcher_s3_complete_error (OstreeFetcher *self, FetcherRequest *req, GTask *task,
                                   const char *eff_url, long response, gboolean retry_all,
                                   gboolean *out_reinitiate)
{
  g_assert_not_reached ();
}

static inline gboolean
_ostree_fetcher_s3_transfer_done (FetcherRequest *req, GTask *task, const char *eff_url,
                                  long response, gboolean *out_waiting)
{
  g_assert_not_reached ();
  return FALSE;
}

static inline void
_ostree_fetcher_s3_complete_after_ranges (OstreeFetcher *self, GTask *task)
{
  g_assert_not_reached ();
}

static inline void
_ostree_fetcher_s3_cancel_ranges (OstreeFetcher *self, FetcherRequest *req)
{
}

#endif /* HAVE_AWS */

G_END_DECLS
