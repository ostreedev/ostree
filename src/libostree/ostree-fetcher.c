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

#include <gio/gfiledescriptorbased.h>
#include <gio/gunixoutputstream.h>

#include "ostree-fetcher.h"
#ifdef HAVE_LIBSOUP_CLIENT_CERTS
#include "ostree-tls-cert-interaction.h"
#endif
#include "ostree.h"
#include "otutil.h"

typedef enum {
  OSTREE_FETCHER_STATE_PENDING,
  OSTREE_FETCHER_STATE_DOWNLOADING,
  OSTREE_FETCHER_STATE_COMPLETE
} OstreeFetcherState;

typedef struct {
  guint refcount;
  OstreeFetcher *self;
  SoupURI *uri;
  int priority;

  OstreeFetcherState state;

  SoupRequest *request;

  gboolean is_stream;
  GInputStream *request_body;
  char *out_tmpfile;
  GOutputStream *out_stream;

  guint64 max_size;
  guint64 current_size;
  guint64 content_length;

  GCancellable *cancellable;
  GTask *task;
} OstreeFetcherPendingURI;

static int
pending_uri_compare (gconstpointer a,
                     gconstpointer b,
                     gpointer unused)
{
  const OstreeFetcherPendingURI *pending_a = a;
  const OstreeFetcherPendingURI *pending_b = b;

  return (pending_a->priority == pending_b->priority) ? 0 :
         (pending_a->priority < pending_b->priority) ? -1 : 1;
}

static void
pending_uri_free (OstreeFetcherPendingURI *pending)
{
  g_assert (pending->refcount > 0);
  pending->refcount--;
  if (pending->refcount > 0)
    return;

  soup_uri_free (pending->uri);
  g_clear_object (&pending->self);
  g_clear_object (&pending->request);
  g_clear_object (&pending->request_body);
  g_free (pending->out_tmpfile);
  g_clear_object (&pending->out_stream);
  g_clear_object (&pending->cancellable);
  g_free (pending);
}

struct OstreeFetcher
{
  GObject parent_instance;

  int tmpdir_dfd;

  GTlsCertificate *client_cert;

  SoupSession *session;
  SoupRequester *requester;

  GHashTable *sending_messages; /*  SoupMessage */

  GHashTable *output_stream_set; /* set<GOutputStream> */
  
  guint64 total_downloaded;
  guint total_requests;

  /* Queue for libsoup, see bgo#708591 */
  gint outstanding;
  GQueue pending_queue;
  gint max_outstanding;
};

G_DEFINE_TYPE (OstreeFetcher, _ostree_fetcher, G_TYPE_OBJECT)

static void
_ostree_fetcher_finalize (GObject *object)
{
  OstreeFetcher *self;

  self = OSTREE_FETCHER (object);

  g_clear_object (&self->session);
  g_clear_object (&self->client_cert);

  g_hash_table_destroy (self->sending_messages);
  g_hash_table_destroy (self->output_stream_set);

  g_queue_clear (&self->pending_queue);

  G_OBJECT_CLASS (_ostree_fetcher_parent_class)->finalize (object);
}

static void
_ostree_fetcher_class_init (OstreeFetcherClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = _ostree_fetcher_finalize;
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
}

static void
_ostree_fetcher_init (OstreeFetcher *self)
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
      _ostree_fetcher_set_proxy (self, http_proxy);
    }

  if (g_getenv ("OSTREE_DEBUG_HTTP"))
    soup_session_add_feature (self->session, (SoupSessionFeature*)soup_logger_new (SOUP_LOGGER_LOG_BODY, 500));

  self->requester = (SoupRequester *)soup_session_get_feature (self->session, SOUP_TYPE_REQUESTER);
  g_object_get (self->session, "max-conns-per-host", &max_conns, NULL);
  if (max_conns <= 8)
    { 
      // We download a lot of small objects in ostree, so this helps a
      // lot.  Also matches what most modern browsers do.
      max_conns = 8;
      g_object_set (self->session, "max-conns-per-host", max_conns, NULL);
    }

  self->max_outstanding = 3 * max_conns;

  g_signal_connect_object (self->session, "request-started",
                           G_CALLBACK (on_request_started), self, 0);
  g_signal_connect_object (self->session, "request-unqueued",
                           G_CALLBACK (on_request_unqueued), self, 0);
  
  self->sending_messages = g_hash_table_new_full (NULL, NULL, NULL,
                                                  (GDestroyNotify)g_object_unref);
  self->output_stream_set = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_object_unref);
}

OstreeFetcher *
_ostree_fetcher_new (int                      tmpdir_dfd,
                    OstreeFetcherConfigFlags  flags)
{
  OstreeFetcher *self = (OstreeFetcher*)g_object_new (OSTREE_TYPE_FETCHER, NULL);

  self->tmpdir_dfd = tmpdir_dfd;
  if ((flags & OSTREE_FETCHER_FLAGS_TLS_PERMISSIVE) > 0)
    g_object_set ((GObject*)self->session, "ssl-strict", FALSE, NULL);
 
  return self;
}

int
_ostree_fetcher_get_dfd (OstreeFetcher *fetcher)
{
  return fetcher->tmpdir_dfd;
}

void
_ostree_fetcher_set_proxy (OstreeFetcher *self,
                           const char    *http_proxy)
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

void
_ostree_fetcher_set_client_cert (OstreeFetcher *fetcher,
                                GTlsCertificate *cert)
{
  g_clear_object (&fetcher->client_cert);
  fetcher->client_cert = g_object_ref (cert);
  if (fetcher->client_cert)
    {
#ifdef HAVE_LIBSOUP_CLIENT_CERTS
      g_autoptr(GTlsInteraction) interaction =
        (GTlsInteraction*)_ostree_tls_cert_interaction_new (fetcher->client_cert);
      g_object_set (fetcher->session, "tls-interaction", interaction, NULL);
#else
      g_warning ("This version of OSTree is compiled without client side certificate support");
#endif
    }
}

void
_ostree_fetcher_set_tls_database (OstreeFetcher *self,
                                  GTlsDatabase  *db)
{
  if (db)
    g_object_set ((GObject*)self->session, "tls-database", db, NULL);
  else
    g_object_set ((GObject*)self->session, "ssl-use-system-ca-file", TRUE, NULL);
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

static gboolean
finish_stream (OstreeFetcherPendingURI *pending,
               GCancellable            *cancellable,
               GError                 **error)
{
  gboolean ret = FALSE;
  struct stat stbuf;

  /* Close it here since we do an async fstat(), where we don't want
   * to hit a bad fd.
   */
  if (pending->out_stream)
    {
      if (!g_output_stream_close (pending->out_stream, pending->cancellable, error))
        goto out;
      g_hash_table_remove (pending->self->output_stream_set, pending->out_stream);
    }

  pending->state = OSTREE_FETCHER_STATE_COMPLETE;
  if (fstatat (pending->self->tmpdir_dfd, pending->out_tmpfile, &stbuf, AT_SYMLINK_NOFOLLOW) != 0)
    {
      gs_set_error_from_errno (error, errno);
      goto out;
    }

  /* Now that we've finished downloading, continue with other queued
   * requests.
   */
  pending->self->outstanding--;
  ostree_fetcher_process_pending_queue (pending->self);

  if (stbuf.st_size < pending->content_length)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Download incomplete");
      goto out;
    }
  else
    {
      pending->self->total_downloaded += stbuf.st_size;
    }

  ret = TRUE;
 out:
  (void) g_input_stream_close (pending->request_body, NULL, NULL);
  return ret;
}

static void
on_stream_read (GObject        *object,
                GAsyncResult   *result,
                gpointer        user_data);

static void
on_out_splice_complete (GObject        *object,
                        GAsyncResult   *result,
                        gpointer        user_data) 
{
  OstreeFetcherPendingURI *pending = user_data;
  gssize bytes_written;
  GError *local_error = NULL;

  bytes_written = g_output_stream_splice_finish ((GOutputStream *)object,
                                                 result,
                                                 &local_error);
  if (bytes_written < 0)
    goto out;

  g_input_stream_read_bytes_async (pending->request_body, 8192, G_PRIORITY_DEFAULT,
                                   pending->cancellable, on_stream_read, pending);

 out:
  if (local_error)
    g_task_return_error (pending->task, local_error);
}

static void
on_stream_read (GObject        *object,
                GAsyncResult   *result,
                gpointer        user_data) 
{
  OstreeFetcherPendingURI *pending = user_data;
  g_autoptr(GBytes) bytes = NULL;
  gsize bytes_read;
  GError *local_error = NULL;

  bytes = g_input_stream_read_bytes_finish ((GInputStream*)object, result, &local_error);
  if (!bytes)
    goto out;

  bytes_read = g_bytes_get_size (bytes);
  if (bytes_read == 0)
    {
      if (!finish_stream (pending, pending->cancellable, &local_error))
        goto out;
      g_task_return_pointer (pending->task,
                             g_strdup (pending->out_tmpfile),
                             (GDestroyNotify) g_free);
      g_object_unref (pending->task);
    }
  else
    {
      if (pending->max_size > 0)
        {
          if (bytes_read > pending->max_size ||
              (bytes_read + pending->current_size) > pending->max_size)
            {
              g_autofree char *uristr = soup_uri_to_string (pending->uri, FALSE);
              local_error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                         "URI %s exceeded maximum size of %" G_GUINT64_FORMAT " bytes",
                                         uristr, pending->max_size);
              goto out;
            }
        }
      
      pending->current_size += bytes_read;

      /* We do this instead of _write_bytes_async() as that's not
       * guaranteed to do a complete write.
       */
      {
        g_autoptr(GInputStream) membuf =
          g_memory_input_stream_new_from_bytes (bytes);
        g_output_stream_splice_async (pending->out_stream, membuf,
                                      G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                      G_PRIORITY_DEFAULT,
                                      pending->cancellable,
                                      on_out_splice_complete,
                                      pending);
      }
    }

 out:
  if (local_error)
    {
      g_task_return_error (pending->task, local_error);
      g_object_unref (pending->task);
    }
}

static void
on_request_sent (GObject        *object,
                 GAsyncResult   *result,
                 gpointer        user_data) 
{
  OstreeFetcherPendingURI *pending = user_data;
  GError *local_error = NULL;
  glnx_unref_object SoupMessage *msg = NULL;

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
          if (pending->is_stream)
            {
              g_task_return_pointer (pending->task,
                                     g_object_ref (pending->request_body),
                                     (GDestroyNotify) g_object_unref);
            }
          else
            {
              g_task_return_pointer (pending->task,
                                     g_strdup (pending->out_tmpfile),
                                     (GDestroyNotify) g_free);
            }
          g_object_unref (pending->task);
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
          local_error = g_error_new (G_IO_ERROR, code,
                                     "Server returned status %u: %s",
                                     msg->status_code,
                                     soup_status_get_phrase (msg->status_code));
          goto out;
        }
    }

  pending->state = OSTREE_FETCHER_STATE_DOWNLOADING;
  
  pending->content_length = soup_request_get_content_length (pending->request);

  if (!pending->is_stream)
    {
      int oflags = O_CREAT | O_WRONLY | O_CLOEXEC;
      int fd;

      /* If we got partial content, we can append; if the server
       * ignored our range request, we need to truncate.
       */
      if (msg && msg->status_code == SOUP_STATUS_PARTIAL_CONTENT)
        oflags |= O_APPEND;
      else
        oflags |= O_TRUNC;

      fd = openat (pending->self->tmpdir_dfd, pending->out_tmpfile, oflags, 0666);
      if (fd == -1)
        {
          gs_set_error_from_errno (&local_error, errno);
          goto out;
        }
      pending->out_stream = g_unix_output_stream_new (fd, TRUE);
      g_hash_table_add (pending->self->output_stream_set, g_object_ref (pending->out_stream));
      g_input_stream_read_bytes_async (pending->request_body, 8192, G_PRIORITY_DEFAULT,
                                       pending->cancellable, on_stream_read, pending);
      
    }
  else
    {
      g_task_return_pointer (pending->task,
                             g_object_ref (pending->request_body),
                             (GDestroyNotify) g_object_unref);
      g_object_unref (pending->task);
    }
  
 out:
  if (local_error)
    {
      if (pending->request_body)
        (void) g_input_stream_close (pending->request_body, NULL, NULL);
      g_task_return_error (pending->task, local_error);
      g_object_unref (pending->task);
    }
}

static OstreeFetcherPendingURI *
ostree_fetcher_request_uri_internal (OstreeFetcher         *self,
                                     SoupURI               *uri,
                                     gboolean               is_stream,
                                     guint64                max_size,
                                     int                    priority,
                                     GCancellable          *cancellable,
                                     GAsyncReadyCallback    callback,
                                     gpointer               user_data,
                                     gpointer               source_tag)
{
  OstreeFetcherPendingURI *pending = g_new0 (OstreeFetcherPendingURI, 1);
  GError *local_error = NULL;

  pending->refcount = 1;
  pending->request = soup_requester_request_uri (self->requester, uri, &local_error);

  pending->self = g_object_ref (self);
  pending->uri = soup_uri_copy (uri);
  pending->priority = priority;
  pending->max_size = max_size;
  pending->is_stream = is_stream;
  pending->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  pending->task = g_task_new (self,
                              cancellable,
                              callback, user_data);

  g_task_set_source_tag (pending->task, source_tag);
  g_task_set_task_data (pending->task, pending, (GDestroyNotify) pending_uri_free);

  if (is_stream)
    {
      soup_request_send_async (pending->request, cancellable,
                               on_request_sent, pending);
    }
  else
    {
      g_autofree char *uristring = soup_uri_to_string (uri, FALSE);
      g_autofree char *tmpfile = NULL;
      struct stat stbuf;
      gboolean exists;

      tmpfile = g_compute_checksum_for_string (G_CHECKSUM_SHA256, uristring, strlen (uristring));

      if (fstatat (self->tmpdir_dfd, tmpfile, &stbuf, AT_SYMLINK_NOFOLLOW) == 0)
        exists = TRUE;
      else
        {
          if (errno == ENOENT)
            exists = FALSE;
          else
            {
              gs_set_error_from_errno (&local_error, errno);
              goto fail;
            }
        }

      if (SOUP_IS_REQUEST_HTTP (pending->request))
        {
          glnx_unref_object SoupMessage *msg = NULL;
          msg = soup_request_http_get_message ((SoupRequestHTTP*) pending->request);
          if (exists && stbuf.st_size > 0)
            soup_message_headers_set_range (msg->request_headers, stbuf.st_size, -1);
        }
      pending->out_tmpfile = tmpfile;
      tmpfile = NULL; /* Transfer ownership */

      g_queue_insert_sorted (&self->pending_queue, pending, pending_uri_compare, NULL);
      ostree_fetcher_process_pending_queue (self);
    }

  g_assert_no_error (local_error);

  self->total_requests++;

  pending->refcount++;

  return pending;

 fail:
  pending_uri_free (pending);
  return NULL;
}

void
_ostree_fetcher_request_uri_with_partial_async (OstreeFetcher         *self,
                                               SoupURI               *uri,
                                               guint64                max_size,
                                               int                    priority,
                                               GCancellable          *cancellable,
                                               GAsyncReadyCallback    callback,
                                               gpointer               user_data)
{
  ostree_fetcher_request_uri_internal (self, uri, FALSE, max_size, priority, cancellable,
                                       callback, user_data,
                                       _ostree_fetcher_request_uri_with_partial_async);
}

char *
_ostree_fetcher_request_uri_with_partial_finish (OstreeFetcher         *self,
                                                GAsyncResult          *result,
                                                GError               **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (result,
                        _ostree_fetcher_request_uri_with_partial_async), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
ostree_fetcher_stream_uri_async (OstreeFetcher         *self,
                                 SoupURI               *uri,
                                 guint64                max_size,
                                 int                    priority,
                                 GCancellable          *cancellable,
                                 GAsyncReadyCallback    callback,
                                 gpointer               user_data)
{
  ostree_fetcher_request_uri_internal (self, uri, TRUE, max_size, priority, cancellable,
                                       callback, user_data,
                                       ostree_fetcher_stream_uri_async);
}

static GInputStream *
ostree_fetcher_stream_uri_finish (OstreeFetcher         *self,
                                  GAsyncResult          *result,
                                  GError               **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (result,
                        ostree_fetcher_stream_uri_async), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

guint64
_ostree_fetcher_bytes_transferred (OstreeFetcher       *self)
{
  guint64 ret = self->total_downloaded;
  GHashTableIter hiter;
  gpointer key, value;

  g_hash_table_iter_init (&hiter, self->output_stream_set);
  while (g_hash_table_iter_next (&hiter, &key, &value))
    {
      GFileOutputStream *stream = key;
      struct stat stbuf;
      
      if (G_IS_FILE_DESCRIPTOR_BASED (stream))
        {
          if (gs_stream_fstat ((GFileDescriptorBased*)stream, &stbuf, NULL, NULL))
            ret += stbuf.st_size;
        }
    }
  
  return ret;
}

typedef struct
{
  GInputStream   *result_stream;
  gboolean         done;
  GError         **error;
}
FetchUriSyncData;

static void
fetch_uri_sync_on_complete (GObject        *object,
                            GAsyncResult   *result,
                            gpointer        user_data)
{
  FetchUriSyncData *data = user_data;

  data->result_stream = ostree_fetcher_stream_uri_finish ((OstreeFetcher*)object,
                                                          result, data->error);
  data->done = TRUE;
}

/* Synchronously request a URI - will iterate the thread-default main
 * context for historical reasons.  If you don't want that, push a
 * temporary one.
 */
gboolean
_ostree_fetcher_request_uri_to_membuf (OstreeFetcher  *fetcher,
                                       SoupURI        *uri,
                                       gboolean        add_nul,
                                       gboolean        allow_noent,
                                       GBytes         **out_contents,
                                       guint64        max_size,
                                       GCancellable   *cancellable,
                                       GError         **error)
{
  gboolean ret = FALSE;
  const guint8 nulchar = 0;
  g_autofree char *ret_contents = NULL;
  g_autoptr(GMemoryOutputStream) buf = NULL;
  g_autoptr(GMainContext) mainctx = NULL;
  FetchUriSyncData data;
  g_assert (error != NULL);

  data.result_stream = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  mainctx = g_main_context_ref_thread_default ();

  data.done = FALSE;
  data.error = error;

  ostree_fetcher_stream_uri_async (fetcher, uri,
                                   max_size,
                                   OSTREE_FETCHER_DEFAULT_PRIORITY,
                                   cancellable,
                                   fetch_uri_sync_on_complete, &data);
  while (!data.done)
    g_main_context_iteration (mainctx, TRUE);

  if (!data.result_stream)
    {
      if (allow_noent)
        {
          if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (error);
              ret = TRUE;
              *out_contents = NULL;
            }
        }
      goto out;
    }

  buf = (GMemoryOutputStream*)g_memory_output_stream_new (NULL, 0, g_realloc, g_free);
  if (g_output_stream_splice ((GOutputStream*)buf, data.result_stream,
                              G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                              cancellable, error) < 0)
    goto out;

  if (add_nul)
    {
      if (!g_output_stream_write ((GOutputStream*)buf, &nulchar, 1, cancellable, error))
        goto out;
    }

  if (!g_output_stream_close ((GOutputStream*)buf, cancellable, error))
    goto out;

  ret = TRUE;
  *out_contents = g_memory_output_stream_steal_as_bytes (buf);
 out:
  g_clear_object (&(data.result_stream));
  return ret;
}
