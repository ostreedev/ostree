/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

#include "ostree-fetcher.h"
#include "ostree.h"
#include "otutil.h"
#include "libgsystem.h"

typedef enum {
  OSTREE_FETCHER_STATE_PENDING,
  OSTREE_FETCHER_STATE_DOWNLOADING,
  OSTREE_FETCHER_STATE_COMPLETE
} OstreeFetcherState;

typedef struct {
  guint refcount;
  OstreeFetcher *self;
  SoupURI *uri;

  OstreeFetcherState state;

  SoupRequest *request;

  gboolean is_stream;
  GInputStream *request_body;
  GFile *out_tmpfile;
  GOutputStream *out_stream;

  guint64 content_length;

  GCancellable *cancellable;
  GSimpleAsyncResult *result;
} OstreeFetcherPendingURI;

static void
pending_uri_free (OstreeFetcherPendingURI *pending)
{
  g_assert (pending->refcount > 0);
  pending->refcount--;
  if (pending->refcount > 0)
    return;

  soup_uri_free (pending->uri);
  g_clear_object (&pending->self);
  g_clear_object (&pending->out_tmpfile);
  g_clear_object (&pending->request);
  g_clear_object (&pending->request_body);
  g_clear_object (&pending->out_stream);
  g_clear_object (&pending->cancellable);
  g_free (pending);
}

struct OstreeFetcher
{
  GObject parent_instance;

  GFile *tmpdir;

  SoupSession *session;
  SoupRequester *requester;

  GHashTable *sending_messages; /*  SoupMessage */

  GHashTable *message_to_request; /* SoupMessage -> SoupRequest */
  GHashTable *output_stream_set; /* set<GOutputStream> */
  
  guint64 total_downloaded;
  guint total_requests;

  /* Queue for libsoup, see bgo#708591 */
  gint outstanding;
  GQueue pending_queue;
  gint max_outstanding;
};

G_DEFINE_TYPE (OstreeFetcher, ostree_fetcher, G_TYPE_OBJECT)

static void
ostree_fetcher_finalize (GObject *object)
{
  OstreeFetcher *self;

  self = OSTREE_FETCHER (object);

  g_clear_object (&self->session);
  g_clear_object (&self->tmpdir);

  g_hash_table_destroy (self->sending_messages);
  g_hash_table_destroy (self->message_to_request);
  g_hash_table_destroy (self->output_stream_set);

  g_queue_clear (&self->pending_queue);

  G_OBJECT_CLASS (ostree_fetcher_parent_class)->finalize (object);
}

static void
ostree_fetcher_class_init (OstreeFetcherClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = ostree_fetcher_finalize;
}

static void
on_request_started (SoupSession  *session,
                    SoupMessage  *msg,
                    SoupSocket   *socket,
                    gpointer      user_data)
{
  OstreeFetcher *self = user_data;
  g_hash_table_insert (self->sending_messages, msg, g_object_ref (msg));
}

static void
on_request_unqueued (SoupSession  *session,
                     SoupMessage  *msg,
                     gpointer      user_data)
{
  OstreeFetcher *self = user_data;
  g_hash_table_remove (self->sending_messages, msg);
  g_hash_table_remove (self->message_to_request, msg);
}

static void
ostree_fetcher_init (OstreeFetcher *self)
{
  gint max_conns;
  const char *http_proxy;

  g_queue_init (&self->pending_queue);
  self->session = soup_session_async_new_with_options (SOUP_SESSION_USER_AGENT, "ostree ",
                                                       SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
                                                       SOUP_SESSION_USE_THREAD_CONTEXT, TRUE,
                                                       SOUP_SESSION_ADD_FEATURE_BY_TYPE, SOUP_TYPE_REQUESTER,
                                                       SOUP_SESSION_TIMEOUT, 60,
                                                       SOUP_SESSION_IDLE_TIMEOUT, 60,
                                                       NULL);

  http_proxy = g_getenv ("http_proxy");
  if (http_proxy)
    {
      SoupURI *proxy_uri = soup_uri_new (http_proxy);
      if (!proxy_uri)
        {
          g_warning ("Invalid proxy URI '%s'", http_proxy);
        }
      else
        {
          g_object_set (self->session, SOUP_SESSION_PROXY_URI, proxy_uri, NULL);
          soup_uri_free (proxy_uri);
        }
    }

  self->requester = (SoupRequester *)soup_session_get_feature (self->session, SOUP_TYPE_REQUESTER);
  g_object_get (self->session, "max-conns-per-host", &max_conns, NULL);
  self->max_outstanding = 3 * max_conns;

  g_signal_connect (self->session, "request-started",
                    G_CALLBACK (on_request_started), self);
  g_signal_connect (self->session, "request-unqueued",
                    G_CALLBACK (on_request_unqueued), self);
  
  self->sending_messages = g_hash_table_new_full (NULL, NULL, NULL,
                                                  (GDestroyNotify)g_object_unref);
  self->message_to_request = g_hash_table_new_full (NULL, NULL, (GDestroyNotify)g_object_unref,
                                                    (GDestroyNotify)pending_uri_free);
  self->output_stream_set = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_object_unref);
}

OstreeFetcher *
ostree_fetcher_new (GFile                    *tmpdir,
                    OstreeFetcherConfigFlags  flags)
{
  OstreeFetcher *self = (OstreeFetcher*)g_object_new (OSTREE_TYPE_FETCHER, NULL);

  self->tmpdir = g_object_ref (tmpdir);
  if ((flags & OSTREE_FETCHER_FLAGS_TLS_PERMISSIVE) > 0)
    g_object_set ((GObject*)self->session, "ssl-strict", FALSE, NULL);
 
  return self;
}

static void
on_request_sent (GObject        *object, GAsyncResult   *result, gpointer        user_data);

static void
ostree_fetcher_process_pending_queue (OstreeFetcher *self)
{

  while (g_queue_peek_head (&self->pending_queue) != NULL &&
         self->outstanding < self->max_outstanding)
    {
      OstreeFetcherPendingURI *next = g_queue_pop_head (&self->pending_queue);
      self->outstanding++;
      soup_request_send_async (next->request, next->cancellable,
                               on_request_sent, next);
    }
}

static void
ostree_fetcher_queue_pending_uri (OstreeFetcher *self,
                                  OstreeFetcherPendingURI *pending)
{
  g_assert (!pending->is_stream);

  g_queue_push_tail (&self->pending_queue, pending);

  ostree_fetcher_process_pending_queue (self);
}

static void
on_splice_complete (GObject        *object,
                    GAsyncResult   *result,
                    gpointer        user_data) 
{
  OstreeFetcherPendingURI *pending = user_data;
  gs_unref_object GFileInfo *file_info = NULL;
  goffset filesize;
  GError *local_error = NULL;

  if (pending->out_stream)
    g_hash_table_remove (pending->self->output_stream_set, pending->out_stream);

  pending->state = OSTREE_FETCHER_STATE_COMPLETE;
  file_info = g_file_query_info (pending->out_tmpfile, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 pending->cancellable, &local_error);
  if (!file_info)
    goto out;

  /* Now that we've finished downloading, continue with other queued
   * requests.
   */
  pending->self->outstanding--;
  ostree_fetcher_process_pending_queue (pending->self);

  filesize = g_file_info_get_size (file_info);
  if (filesize < pending->content_length)
    {
      g_set_error (&local_error, G_IO_ERROR, G_IO_ERROR_FAILED, "Download incomplete");
      goto out;
    }
  else
    {
      pending->self->total_downloaded += g_file_info_get_size (file_info);
    }

 out:
  (void) g_input_stream_close (pending->request_body, NULL, NULL);
  if (local_error)
    g_simple_async_result_take_error (pending->result, local_error);
  g_simple_async_result_complete (pending->result);
  g_object_unref (pending->result);
}

static void
on_request_sent (GObject        *object,
                 GAsyncResult   *result,
                 gpointer        user_data) 
{
  OstreeFetcherPendingURI *pending = user_data;
  GError *local_error = NULL;
  gs_unref_object SoupMessage *msg = NULL;
  GOutputStreamSpliceFlags flags = G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET;

  pending->state = OSTREE_FETCHER_STATE_COMPLETE;
  pending->request_body = soup_request_send_finish ((SoupRequest*) object,
                                                   result, &local_error);

  if (!pending->request_body)
    goto out;
  
  if (SOUP_IS_REQUEST_HTTP (object))
    {
      msg = soup_request_http_get_message ((SoupRequestHTTP*) object);
      if (msg->status_code == SOUP_STATUS_REQUESTED_RANGE_NOT_SATISFIABLE)
        {
          // We already have the whole file, so just use it.
          pending->state = OSTREE_FETCHER_STATE_COMPLETE;
          (void) g_input_stream_close (pending->request_body, NULL, NULL);
          g_simple_async_result_complete (pending->result);
          g_object_unref (pending->result);
          return;
        }
      else if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
        {
          GIOErrorEnum code;
          switch (msg->status_code)
            {
            case 404:
            case 410:
              code = G_IO_ERROR_NOT_FOUND;
              break;
            default:
              code = G_IO_ERROR_FAILED;
            }
          g_set_error (&local_error, G_IO_ERROR, code,
                       "Server returned status %u: %s",
                       msg->status_code, soup_status_get_phrase (msg->status_code));
          goto out;
        }
    }

  pending->state = OSTREE_FETCHER_STATE_DOWNLOADING;
  
  pending->content_length = soup_request_get_content_length (pending->request);

  if (!pending->is_stream)
    {
      pending->out_stream = G_OUTPUT_STREAM (g_file_append_to (pending->out_tmpfile, G_FILE_CREATE_NONE,
                                                               pending->cancellable, &local_error));
      if (!pending->out_stream)
        goto out;
      g_hash_table_add (pending->self->output_stream_set, g_object_ref (pending->out_stream));
      g_output_stream_splice_async (pending->out_stream, pending->request_body, flags, G_PRIORITY_DEFAULT,
                                    pending->cancellable, on_splice_complete, pending);
      
    }
  else
    {
      g_simple_async_result_complete (pending->result);
    }
  
 out:
  if (local_error)
    {
      g_simple_async_result_take_error (pending->result, local_error);
      g_simple_async_result_complete (pending->result);
    }
}

static OstreeFetcherPendingURI *
ostree_fetcher_request_uri_internal (OstreeFetcher         *self,
                                     SoupURI               *uri,
                                     gboolean               is_stream,
                                     GCancellable          *cancellable,
                                     GAsyncReadyCallback    callback,
                                     gpointer               user_data,
                                     gpointer               source_tag)
{
  OstreeFetcherPendingURI *pending;
  GError *local_error = NULL;

  pending = g_new0 (OstreeFetcherPendingURI, 1);
  pending->refcount = 1;
  pending->self = g_object_ref (self);
  pending->uri = soup_uri_copy (uri);
  pending->is_stream = is_stream;
  if (!is_stream)
    {
      gs_free char *uristring = soup_uri_to_string (uri, FALSE);
      gs_free char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, uristring, strlen (uristring));
      pending->out_tmpfile = g_file_get_child (self->tmpdir, hash);
    }
  pending->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  pending->request = soup_requester_request_uri (self->requester, uri, &local_error);
  pending->result = g_simple_async_result_new ((GObject*) self,
                                               callback,
                                               user_data,
                                               source_tag);
  g_simple_async_result_set_op_res_gpointer (pending->result,
                                             pending,
                                             (GDestroyNotify) pending_uri_free);
  
  g_assert_no_error (local_error);
  
  pending->refcount++;

  return pending;
}

void
ostree_fetcher_request_uri_with_partial_async (OstreeFetcher         *self,
                                               SoupURI               *uri,
                                               GCancellable          *cancellable,
                                               GAsyncReadyCallback    callback,
                                               gpointer               user_data)
{
  OstreeFetcherPendingURI *pending;
  gs_unref_object GFileInfo *file_info = NULL;
  GError *local_error = NULL;

  self->total_requests++;

  pending = ostree_fetcher_request_uri_internal (self, uri, FALSE, cancellable,
                                                 callback, user_data,
                                                 ostree_fetcher_request_uri_with_partial_async);

  if (!ot_gfile_query_info_allow_noent (pending->out_tmpfile, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        &file_info, cancellable, &local_error))
    goto out;

  if (SOUP_IS_REQUEST_HTTP (pending->request))
    {
      SoupMessage *msg;

      msg = soup_request_http_get_message ((SoupRequestHTTP*) pending->request);
      if (file_info && g_file_info_get_size (file_info) > 0)
        soup_message_headers_set_range (msg->request_headers, g_file_info_get_size (file_info), -1);
      g_hash_table_insert (self->message_to_request,
                           soup_request_http_get_message ((SoupRequestHTTP*)pending->request),
                           pending);
    }

  ostree_fetcher_queue_pending_uri (self, pending);

 out:
  if (local_error != NULL)
    {
      g_simple_async_result_take_error (pending->result, local_error);
      g_simple_async_result_complete (pending->result);
    }
}

GFile *
ostree_fetcher_request_uri_with_partial_finish (OstreeFetcher         *self,
                                                GAsyncResult          *result,
                                                GError               **error)
{
  GSimpleAsyncResult *simple;
  OstreeFetcherPendingURI *pending;

  g_return_val_if_fail (g_simple_async_result_is_valid (result, (GObject*)self, ostree_fetcher_request_uri_with_partial_async), FALSE);

  simple = G_SIMPLE_ASYNC_RESULT (result);
  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;
  pending = g_simple_async_result_get_op_res_gpointer (simple);

  return g_object_ref (pending->out_tmpfile);
}

void
ostree_fetcher_stream_uri_async (OstreeFetcher         *self,
                                 SoupURI               *uri,
                                 GCancellable          *cancellable,
                                 GAsyncReadyCallback    callback,
                                 gpointer               user_data)
{
  OstreeFetcherPendingURI *pending;

  self->total_requests++;

  pending = ostree_fetcher_request_uri_internal (self, uri, TRUE, cancellable,
                                                 callback, user_data,
                                                 ostree_fetcher_stream_uri_async);

  if (SOUP_IS_REQUEST_HTTP (pending->request))
    {
      g_hash_table_insert (self->message_to_request,
                           soup_request_http_get_message ((SoupRequestHTTP*)pending->request),
                           pending);
    }
  
  soup_request_send_async (pending->request, cancellable,
                           on_request_sent, pending);
}

GInputStream *
ostree_fetcher_stream_uri_finish (OstreeFetcher         *self,
                                  GAsyncResult          *result,
                                  GError               **error)
{
  GSimpleAsyncResult *simple;
  OstreeFetcherPendingURI *pending;

  g_return_val_if_fail (g_simple_async_result_is_valid (result, (GObject*)self, ostree_fetcher_stream_uri_async), FALSE);

  simple = G_SIMPLE_ASYNC_RESULT (result);
  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;
  pending = g_simple_async_result_get_op_res_gpointer (simple);

  return g_object_ref (pending->request_body);
}

static char *
format_size_pair (guint64 start,
                  guint64 max)
{
  if (max < 1024)
    return g_strdup_printf ("%lu/%lu bytes", 
                            (gulong) start,
                            (gulong) max);
  else
    return g_strdup_printf ("%.1f/%.1f KiB", ((double) start) / 1024,
                            ((double) max) / 1024);
}

char *
ostree_fetcher_query_state_text (OstreeFetcher              *self)
{
  guint n_active;

  n_active = g_hash_table_size (self->sending_messages);
  if (n_active > 0)
    {
      GHashTableIter hash_iter;
      gpointer key, value;
      GString *buf;

      buf = g_string_new ("");

      g_string_append_printf (buf, "%u requests", n_active);

      g_hash_table_iter_init (&hash_iter, self->sending_messages);
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          OstreeFetcherPendingURI *active; 

          active = g_hash_table_lookup (self->message_to_request, key);
          g_assert (active != NULL);

          if (active->out_tmpfile)
            {
              gs_unref_object GFileInfo *file_info = NULL;

              file_info = g_file_query_info (active->out_tmpfile, OSTREE_GIO_FAST_QUERYINFO,
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                             NULL, NULL);
              if (file_info)
                {
                  gs_free char *size = format_size_pair (g_file_info_get_size (file_info),
                                                         active->content_length);
                  g_string_append_printf (buf, " [%s]", size);
                }
            }
          else
            {
              g_string_append_printf (buf, " [Requesting]");
            }
        }

      return g_string_free (buf, FALSE);
    }
  else
    return g_strdup_printf ("Idle");
}

guint64
ostree_fetcher_bytes_transferred (OstreeFetcher       *self)
{
  guint64 ret = self->total_downloaded;
  GHashTableIter hiter;
  gpointer key, value;

  g_hash_table_iter_init (&hiter, self->output_stream_set);
  while (g_hash_table_iter_next (&hiter, &key, &value))
    {
      GFileOutputStream *stream = key;
      GFileInfo *finfo;

      finfo = g_file_output_stream_query_info (stream, "standard::size",
                                               NULL, NULL);
      if (finfo)
        {
          ret += g_file_info_get_size (finfo);
          g_object_unref (finfo);
        }
    }
  
  return ret;
}

guint
ostree_fetcher_get_n_requests (OstreeFetcher       *self)
{
  return self->total_requests;
}
