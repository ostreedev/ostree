/*
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
 * Copyright (C) 2022 Igalia S.L.
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <gio/gfiledescriptorbased.h>
#include <gio/gunixoutputstream.h>

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-journal.h>
#endif

#include "ostree-fetcher-util.h"
#include "otutil.h"

typedef struct
{
  GBytes *result_buf;
  gboolean result_not_modified;
  char *result_etag;
  guint64 result_last_modified; /* second since the epoch */
  gboolean done;
  GMainContext *main_context; /* (owned) */
  GError **error;
} FetchUriSyncData;

static void
fetch_uri_sync_on_complete (GObject *object, GAsyncResult *result, gpointer user_data)
{
  FetchUriSyncData *data = user_data;

  (void)_ostree_fetcher_request_to_membuf_finish (
      (OstreeFetcher *)object, result, &data->result_buf, &data->result_not_modified,
      &data->result_etag, &data->result_last_modified, data->error);
  data->done = TRUE;
  g_main_context_wakeup (data->main_context);
}

static gboolean
_ostree_fetcher_mirrored_request_to_membuf_once (
    OstreeFetcher *fetcher, GPtrArray *mirrorlist, const char *filename,
    OstreeFetcherRequestFlags flags, const char *if_none_match, guint64 if_modified_since,
    GBytes **out_contents, gboolean *out_not_modified, char **out_etag, guint64 *out_last_modified,
    guint64 max_size, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr (GMainContext) mainctx = NULL;
  FetchUriSyncData data;
  g_assert (error != NULL);

  memset (&data, 0, sizeof (data));

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  mainctx = g_main_context_new ();
  g_main_context_push_thread_default (mainctx);

  data.main_context = g_main_context_ref (mainctx);
  data.done = FALSE;
  data.error = error;

  _ostree_fetcher_request_to_membuf (fetcher, mirrorlist, filename, flags, if_none_match,
                                     if_modified_since, max_size, OSTREE_FETCHER_DEFAULT_PRIORITY,
                                     cancellable, fetch_uri_sync_on_complete, &data);
  while (!data.done)
    g_main_context_iteration (mainctx, TRUE);

  if (!data.result_buf)
    {
      if (flags & OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT)
        {
          if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (error);
              ret = TRUE;
              *out_contents = NULL;
              if (out_not_modified != NULL)
                *out_not_modified = FALSE;
              if (out_etag != NULL)
                *out_etag = NULL;
              if (out_last_modified != NULL)
                *out_last_modified = 0;
            }
        }
      goto out;
    }

  ret = TRUE;
  *out_contents = g_steal_pointer (&data.result_buf);
  if (out_not_modified != NULL)
    *out_not_modified = data.result_not_modified;
  if (out_etag != NULL)
    *out_etag = g_steal_pointer (&data.result_etag);
  if (out_last_modified != NULL)
    *out_last_modified = data.result_last_modified;
out:
  if (mainctx)
    g_main_context_pop_thread_default (mainctx);
  g_clear_pointer (&data.result_buf, g_bytes_unref);
  g_clear_pointer (&data.result_etag, g_free);
  g_clear_pointer (&data.main_context, g_main_context_unref);
  return ret;
}

gboolean
_ostree_fetcher_mirrored_request_to_membuf (OstreeFetcher *fetcher, GPtrArray *mirrorlist,
                                            const char *filename, OstreeFetcherRequestFlags flags,
                                            const char *if_none_match, guint64 if_modified_since,
                                            guint n_network_retries, GBytes **out_contents,
                                            gboolean *out_not_modified, char **out_etag,
                                            guint64 *out_last_modified, guint64 max_size,
                                            GCancellable *cancellable, GError **error)
{
  g_autoptr (GError) local_error = NULL;
  guint n_retries_remaining = n_network_retries;

  do
    {
      g_clear_error (&local_error);
      if (_ostree_fetcher_mirrored_request_to_membuf_once (
              fetcher, mirrorlist, filename, flags, if_none_match, if_modified_since, out_contents,
              out_not_modified, out_etag, out_last_modified, max_size, cancellable, &local_error))
        return TRUE;
    }
  while (_ostree_fetcher_should_retry_request (local_error, n_retries_remaining--));

  g_assert (local_error != NULL);
  g_propagate_error (error, g_steal_pointer (&local_error));
  return FALSE;
}

/* Helper for callers who just want to fetch single one-off URIs */
gboolean
_ostree_fetcher_request_uri_to_membuf (OstreeFetcher *fetcher, OstreeFetcherURI *uri,
                                       OstreeFetcherRequestFlags flags, const char *if_none_match,
                                       guint64 if_modified_since, guint n_network_retries,
                                       GBytes **out_contents, gboolean *out_not_modified,
                                       char **out_etag, guint64 *out_last_modified,
                                       guint64 max_size, GCancellable *cancellable, GError **error)
{
  g_autoptr (GPtrArray) mirrorlist = g_ptr_array_new ();
  g_ptr_array_add (mirrorlist, uri); /* no transfer */
  return _ostree_fetcher_mirrored_request_to_membuf (
      fetcher, mirrorlist, NULL, flags, if_none_match, if_modified_since, n_network_retries,
      out_contents, out_not_modified, out_etag, out_last_modified, max_size, cancellable, error);
}

#define OSTREE_HTTP_FAILURE_ID \
  SD_ID128_MAKE (f0, 2b, ce, 89, a5, 4e, 4e, fa, b3, a9, 4a, 79, 7d, 26, 20, 4a)

void
_ostree_fetcher_journal_failure (const char *remote_name, const char *url, const char *msg)
{
  /* Sanity - we don't want to log this when doing local/file pulls */
  if (!remote_name)
    return;
  ot_journal_send ("MESSAGE=libostree HTTP error from remote %s for <%s>: %s", remote_name, url,
                   msg, "MESSAGE_ID=" SD_ID128_FORMAT_STR,
                   SD_ID128_FORMAT_VAL (OSTREE_HTTP_FAILURE_ID), "OSTREE_REMOTE=%s", remote_name,
                   "OSTREE_URL=%s", url, "PRIORITY=%i", LOG_ERR, NULL);
}

/* Check whether a particular operation should be retried. This is entirely
 * based on how it failed (if at all) last time, and whether the operation has
 * some retries left. The retry count is set when the operation is first
 * created, and must be decremented by the caller. (@n_retries_remaining == 0)
 * will always return %FALSE from this function.
 *
 * FIXME: In future, we may decide to use transient failures like this as a hint
 * to prioritise other mirrors for a particular pull operation (for example). */
gboolean
_ostree_fetcher_should_retry_request (const GError *error, guint n_retries_remaining)
{
  if (error == NULL)
    g_debug ("%s: error: unset, n_retries_remaining: %u", G_STRFUNC, n_retries_remaining);
  else
    g_debug ("%s: error: %u:%u %s, n_retries_remaining: %u", G_STRFUNC, error->domain, error->code,
             error->message, n_retries_remaining);

  if (error == NULL || n_retries_remaining == 0)
    return FALSE;

  /* Return TRUE for transient errors. */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)
      || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND)
      || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE)
      || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT)
      || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_BUSY) ||
#if !GLIB_CHECK_VERSION(2, 44, 0)
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE) ||
#else
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED) ||
#endif
      g_error_matches (error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND)
      || g_error_matches (error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_TEMPORARY_FAILURE))
    {
      g_debug ("Should retry request (remaining: %u retries), due to transient error: %s",
               n_retries_remaining, error->message);
      return TRUE;
    }

  return FALSE;
}

/* Convert a HTTP status code representing an error from libsoup or libcurl to
 * a #GIOErrorEnum. This will return %G_IO_ERROR_FAILED if the status code is
 * unknown or otherwise unhandled. */
GIOErrorEnum
_ostree_fetcher_http_status_code_to_io_error (guint status_code)
{
  switch (status_code)
    {
    case 403: /* SOUP_STATUS_FORBIDDEN */
    case 404: /* SOUP_STATUS_NOT_FOUND */
    case 410: /* SOUP_STATUS_GONE */
      return G_IO_ERROR_NOT_FOUND;
    case 408: /* SOUP_STATUS_REQUEST_TIMEOUT */
      return G_IO_ERROR_TIMED_OUT;
    case 500: /* SOUP_STATUS_INTERNAL_SERVER_ERROR */
      return G_IO_ERROR_BUSY;
    default:
      return G_IO_ERROR_FAILED;
    }
}
