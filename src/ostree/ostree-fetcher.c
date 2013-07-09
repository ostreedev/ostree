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

  GFile *tmpfile;
  GInputStream *request_body;
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
  g_clear_object (&pending->tmpfile);
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
  
  guint64 total_downloaded;
  guint total_requests;
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
  self->session = soup_session_async_new_with_options (SOUP_SESSION_USER_AGENT, "ostree ",
                                                       SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
                                                       SOUP_SESSION_USE_THREAD_CONTEXT, TRUE,
                                                       SOUP_SESSION_ADD_FEATURE_BY_TYPE, SOUP_TYPE_REQUESTER,
                                                       NULL);
  self->requester = (SoupRequester *)soup_session_get_feature (self->session, SOUP_TYPE_REQUESTER);

  g_signal_connect (self->session, "request-started",
                    G_CALLBACK (on_request_started), self);
  g_signal_connect (self->session, "request-unqueued",
                    G_CALLBACK (on_request_unqueued), self);
  
  self->sending_messages = g_hash_table_new_full (NULL, NULL, NULL,
                                                  (GDestroyNotify)g_object_unref);
  self->message_to_request = g_hash_table_new_full (NULL, NULL, (GDestroyNotify)g_object_unref,
                                                    (GDestroyNotify)pending_uri_free);
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
on_splice_complete (GObject        *object,
                    GAsyncResult   *result,
                    gpointer        user_data) 
{
  OstreeFetcherPendingURI *pending = user_data;
  gs_unref_object GFileInfo *file_info = NULL;

  pending->state = OSTREE_FETCHER_STATE_COMPLETE;
  file_info = g_file_query_info (pending->tmpfile, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 NULL, NULL);
  if (file_info)
    pending->self->total_downloaded += g_file_info_get_size (file_info);

  (void) g_input_stream_close (pending->request_body, NULL, NULL);

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

  pending->request_body = soup_request_send_finish ((SoupRequest*) object,
                                                   result, &local_error);

  if (!pending->request_body)
    {
      pending->state = OSTREE_FETCHER_STATE_COMPLETE;
      g_simple_async_result_take_error (pending->result, local_error);
      g_simple_async_result_complete (pending->result);
      return;
    }
  
  if (SOUP_IS_REQUEST_HTTP (object))
    {
      msg = soup_request_http_get_message ((SoupRequestHTTP*) object);
      if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
        {
          g_set_error (&local_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Server returned status %u: %s",
                       msg->status_code, soup_status_get_phrase (msg->status_code));
          g_simple_async_result_take_error (pending->result, local_error);
          g_simple_async_result_complete (pending->result);
          return;
        }
    }

  pending->state = OSTREE_FETCHER_STATE_DOWNLOADING;
  
  pending->content_length = soup_request_get_content_length (pending->request);
  
  /* TODO - make this async */
  if (!ostree_create_temp_regular_file (pending->self->tmpdir,
                                        NULL, NULL,
                                        &pending->tmpfile,
                                        &pending->out_stream,
                                        NULL, &local_error))
    {
      g_simple_async_result_take_error (pending->result, local_error);
      g_simple_async_result_complete (pending->result);
      return;
    }
  
  g_output_stream_splice_async (pending->out_stream, pending->request_body, flags, G_PRIORITY_DEFAULT,
                                pending->cancellable, on_splice_complete, pending);
}

void
ostree_fetcher_request_uri_async (OstreeFetcher         *self,
                                  SoupURI               *uri,
                                  GCancellable          *cancellable,
                                  GAsyncReadyCallback    callback,
                                  gpointer               user_data)
{
  OstreeFetcherPendingURI *pending;
  GError *local_error = NULL;

  self->total_requests++;

  pending = g_new0 (OstreeFetcherPendingURI, 1);
  pending->refcount = 1;
  pending->self = g_object_ref (self);
  pending->uri = soup_uri_copy (uri);
  pending->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  pending->request = soup_requester_request_uri (self->requester, uri, &local_error);
  g_assert_no_error (local_error);

  pending->refcount++;
  if (SOUP_IS_REQUEST_HTTP (pending->request))
    {
      g_hash_table_insert (self->message_to_request,
                           soup_request_http_get_message ((SoupRequestHTTP*)pending->request),
                           pending);
    }

  pending->result = g_simple_async_result_new ((GObject*) self,
                                               callback, user_data,
                                               ostree_fetcher_request_uri_async);
  g_simple_async_result_set_op_res_gpointer (pending->result, pending,
                                             (GDestroyNotify) pending_uri_free);

  soup_request_send_async (pending->request, cancellable,
                           on_request_sent, pending);
}

GFile *
ostree_fetcher_request_uri_finish (OstreeFetcher         *self,
                                   GAsyncResult          *result,
                                   GError               **error)
{
  GSimpleAsyncResult *simple;
  OstreeFetcherPendingURI *pending;

  g_return_val_if_fail (g_simple_async_result_is_valid (result, (GObject*)self, ostree_fetcher_request_uri_async), FALSE);

  simple = G_SIMPLE_ASYNC_RESULT (result);
  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;
  pending = g_simple_async_result_get_op_res_gpointer (simple);

  return g_object_ref (pending->tmpfile);
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

          if (active->tmpfile)
            {
              gs_unref_object GFileInfo *file_info = NULL;

              file_info = g_file_query_info (active->tmpfile, OSTREE_GIO_FAST_QUERYINFO,
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
  return self->total_downloaded;
}

guint
ostree_fetcher_get_n_requests (OstreeFetcher       *self)
{
  return self->total_requests;
}
