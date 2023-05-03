/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 * Copyright (C) 2022 Igalia S.L.
 * Copyright (C) 2023 Endless OS Foundation, LLC
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
 *
 * Author: Colin Walters <walters@verbum.org>
 * Author: Daniel Kolesa <dkolesa@igalia.com>
 * Author: Dan Nicholson <dbn@endlessos.org>
 */

#include "config.h"

#include <gio/gio.h>
#include <gio/gunixoutputstream.h>
#include <libsoup/soup.h>

#include "libglnx.h"
#include "ostree-enumtypes.h"
#include "ostree-fetcher-util.h"
#include "ostree-fetcher.h"
#include "ostree-repo-private.h"
#include "ostree-tls-cert-interaction-private.h"

typedef struct
{
  GPtrArray *mirrorlist; /* list of base URIs */
  char *filename;        /* relative name to fetch or NULL */
  guint mirrorlist_idx;

  SoupMessage *message;
  struct OstreeFetcher *fetcher;
  GMainContext *mainctx;
  SoupSession *session;
  GFile *file;

  gboolean is_membuf;
  OstreeFetcherRequestFlags flags;
  char *if_none_match;       /* request ETag */
  guint64 if_modified_since; /* seconds since the epoch */
  GInputStream *response_body;
  GLnxTmpfile tmpf;
  GOutputStream *out_stream;
  gboolean out_not_modified; /* TRUE if the server gave a HTTP 304 Not Modified response, which we
                                don’t propagate as an error */
  char *out_etag;            /* response ETag */
  guint64 out_last_modified; /* response Last-Modified, seconds since the epoch */

  guint64 max_size;
  guint64 current_size;
  goffset content_length;
} FetcherRequest;

struct OstreeFetcher
{
  GObject parent_instance;

  OstreeFetcherConfigFlags config_flags;
  char *remote_name;
  int tmpdir_dfd;

  GHashTable *sessions; /* (element-type GMainContext SoupSession ) */
  GProxyResolver *proxy_resolver;
  SoupCookieJar *cookie_jar;
  OstreeTlsCertInteraction *client_cert;
  GTlsDatabase *tls_database;
  GVariant *extra_headers;
  char *user_agent;

  guint64 bytes_transferred;
};

enum
{
  PROP_0,
  PROP_CONFIG_FLAGS
};

G_DEFINE_TYPE (OstreeFetcher, _ostree_fetcher, G_TYPE_OBJECT)

static void
fetcher_request_free (FetcherRequest *request)
{
  g_debug ("Freeing request for %s", request->filename);
  g_clear_pointer (&request->mirrorlist, g_ptr_array_unref);
  g_clear_pointer (&request->filename, g_free);
  g_clear_object (&request->message);
  g_clear_pointer (&request->mainctx, g_main_context_unref);
  g_clear_object (&request->session);
  g_clear_object (&request->file);
  g_clear_pointer (&request->if_none_match, g_free);
  g_clear_object (&request->response_body);
  glnx_tmpfile_clear (&request->tmpf);
  g_clear_object (&request->out_stream);
  g_clear_pointer (&request->out_etag, g_free);
  g_free (request);
}

static void on_request_sent (GObject *object, GAsyncResult *result, gpointer user_data);

static gboolean
_message_accept_cert_loose (SoupMessage *msg, GTlsCertificate *tls_peer_certificate,
                            GTlsCertificateFlags tls_peer_errors, gpointer data)
{
  return TRUE;
}

static void
create_request_message (FetcherRequest *request)
{
  g_assert (request->mirrorlist);
  g_assert (request->mirrorlist_idx < request->mirrorlist->len);

  OstreeFetcherURI *next_mirror = g_ptr_array_index (request->mirrorlist, request->mirrorlist_idx);
  g_autoptr (OstreeFetcherURI) uri = NULL;
  if (request->filename)
    uri = _ostree_fetcher_uri_new_subpath (next_mirror, request->filename);
  if (!uri)
    uri = (OstreeFetcherURI *)g_uri_ref ((GUri *)next_mirror);

  g_clear_object (&request->message);
  g_clear_object (&request->file);
  g_clear_object (&request->response_body);

  GUri *guri = (GUri *)uri;

  /* file:// URI is handle via GFile */
  if (!strcmp (g_uri_get_scheme (guri), "file"))
    {
      g_autofree char *str = g_uri_to_string (guri);
      request->file = g_file_new_for_uri (str);
      return;
    }

  request->message = soup_message_new_from_uri ("GET", guri);

  if (request->if_none_match != NULL)
    soup_message_headers_append (soup_message_get_request_headers (request->message),
                                 "If-None-Match", request->if_none_match);

  if (request->if_modified_since > 0)
    {
      g_autoptr (GDateTime) date_time = g_date_time_new_from_unix_utc (request->if_modified_since);
      g_autofree char *mod_date = g_date_time_format (date_time, "%a, %d %b %Y %H:%M:%S %Z");
      soup_message_headers_append (soup_message_get_request_headers (request->message),
                                   "If-Modified-Since", mod_date);
    }

  if ((request->fetcher->config_flags & OSTREE_FETCHER_FLAGS_TLS_PERMISSIVE) != 0)
    g_signal_connect (request->message, "accept-certificate",
                      G_CALLBACK (_message_accept_cert_loose), NULL);

  if (request->fetcher->extra_headers)
    {
      g_autoptr (GVariantIter) viter = g_variant_iter_new (request->fetcher->extra_headers);
      const char *key;
      const char *value;

      while (g_variant_iter_next (viter, "(&s&s)", &key, &value))
        soup_message_headers_append (soup_message_get_request_headers (request->message), key,
                                     value);
    }
}

static void
initiate_task_request (GTask *task)
{
  FetcherRequest *request = g_task_get_task_data (task);
  create_request_message (request);

  GCancellable *cancellable = g_task_get_cancellable (task);
  int priority = g_task_get_priority (task);
  if (request->file)
    g_file_read_async (request->file, priority, cancellable, on_request_sent, task);
  else
    {
      g_autofree char *uri = g_uri_to_string (soup_message_get_uri (request->message));
      const char *dest = request->is_membuf ? "memory" : "tmpfile";
      g_debug ("Requesting %s to %s for session %p in main context %p", uri, dest, request->session,
               request->mainctx);
      soup_session_send_async (request->session, request->message, priority, cancellable,
                               on_request_sent, task);
    }
}

static void
_ostree_fetcher_set_property (GObject *object, guint prop_id, const GValue *value,
                              GParamSpec *pspec)
{
  OstreeFetcher *self = OSTREE_FETCHER (object);

  switch (prop_id)
    {
    case PROP_CONFIG_FLAGS:
      self->config_flags = g_value_get_flags (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
_ostree_fetcher_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  OstreeFetcher *self = OSTREE_FETCHER (object);

  switch (prop_id)
    {
    case PROP_CONFIG_FLAGS:
      g_value_set_flags (value, self->config_flags);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
_ostree_fetcher_finalize (GObject *object)
{
  OstreeFetcher *self = OSTREE_FETCHER (object);

  g_clear_pointer (&self->remote_name, g_free);
  g_clear_pointer (&self->sessions, g_hash_table_unref);
  g_clear_object (&self->proxy_resolver);
  g_clear_object (&self->cookie_jar);
  g_clear_object (&self->client_cert);
  g_clear_object (&self->tls_database);
  g_clear_pointer (&self->extra_headers, g_variant_unref);
  g_clear_pointer (&self->user_agent, g_free);

  G_OBJECT_CLASS (_ostree_fetcher_parent_class)->finalize (object);
}

static void
_ostree_fetcher_constructed (GObject *object)
{
  OstreeFetcher *self = OSTREE_FETCHER (object);

  const char *http_proxy = g_getenv ("http_proxy");
  if (http_proxy != NULL && http_proxy[0] != '\0')
    _ostree_fetcher_set_proxy (self, http_proxy);

  G_OBJECT_CLASS (_ostree_fetcher_parent_class)->constructed (object);
}

static void
_ostree_fetcher_class_init (OstreeFetcherClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->set_property = _ostree_fetcher_set_property;
  gobject_class->get_property = _ostree_fetcher_get_property;
  gobject_class->finalize = _ostree_fetcher_finalize;
  gobject_class->constructed = _ostree_fetcher_constructed;

  g_object_class_install_property (
      gobject_class, PROP_CONFIG_FLAGS,
      g_param_spec_flags ("config-flags", "", "", OSTREE_TYPE_FETCHER_CONFIG_FLAGS,
                          OSTREE_FETCHER_FLAGS_NONE,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
_ostree_fetcher_init (OstreeFetcher *self)
{
  self->sessions = g_hash_table_new (g_direct_hash, g_direct_equal);
}

OstreeFetcher *
_ostree_fetcher_new (int tmpdir_dfd, const char *remote_name, OstreeFetcherConfigFlags flags)
{
  OstreeFetcher *self = g_object_new (OSTREE_TYPE_FETCHER, "config-flags", flags, NULL);
  self->remote_name = g_strdup (remote_name);
  self->tmpdir_dfd = tmpdir_dfd;

  return self;
}

int
_ostree_fetcher_get_dfd (OstreeFetcher *self)
{
  return self->tmpdir_dfd;
}

void
_ostree_fetcher_set_proxy (OstreeFetcher *self, const char *http_proxy)
{
  g_return_if_fail (OSTREE_IS_FETCHER (self));
  g_return_if_fail (http_proxy != NULL && http_proxy[0] != '\0');

  /* validate first */
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GUri) guri = g_uri_parse (http_proxy, G_URI_FLAGS_NONE, &local_error);
  if (guri == NULL)
    {
      g_warning ("Invalid proxy URI '%s': %s", http_proxy, local_error->message);
      return;
    }

  g_clear_object (&self->proxy_resolver);
  self->proxy_resolver = g_simple_proxy_resolver_new (http_proxy, NULL);
}

void
_ostree_fetcher_set_cookie_jar (OstreeFetcher *self, const char *jar_path)
{
  g_return_if_fail (OSTREE_IS_FETCHER (self));
  g_return_if_fail (jar_path != NULL);

  g_clear_object (&self->cookie_jar);
  self->cookie_jar = soup_cookie_jar_text_new (jar_path, TRUE);
}

void
_ostree_fetcher_set_client_cert (OstreeFetcher *self, const char *cert_path, const char *key_path)
{
  g_return_if_fail (OSTREE_IS_FETCHER (self));

  g_clear_object (&self->client_cert);
  self->client_cert = _ostree_tls_cert_interaction_new (cert_path, key_path);
}

void
_ostree_fetcher_set_tls_database (OstreeFetcher *self, const char *tlsdb_path)
{
  g_return_if_fail (OSTREE_IS_FETCHER (self));

  g_autoptr (GError) local_error = NULL;
  GTlsDatabase *tlsdb = g_tls_file_database_new (tlsdb_path, &local_error);
  if (tlsdb == NULL)
    {
      g_warning ("Invalid TLS database '%s': %s", tlsdb_path, local_error->message);
      return;
    }

  g_clear_object (&self->tls_database);
  self->tls_database = tlsdb;
}

void
_ostree_fetcher_set_extra_headers (OstreeFetcher *self, GVariant *extra_headers)
{
  g_return_if_fail (OSTREE_IS_FETCHER (self));

  g_clear_pointer (&self->extra_headers, g_variant_unref);
  self->extra_headers = g_variant_ref (extra_headers);
}

void
_ostree_fetcher_set_extra_user_agent (OstreeFetcher *self, const char *extra_user_agent)
{
  g_return_if_fail (OSTREE_IS_FETCHER (self));

  g_clear_pointer (&self->user_agent, g_free);
  if (extra_user_agent != NULL)
    self->user_agent = g_strdup_printf ("%s %s", OSTREE_FETCHER_USERAGENT_STRING, extra_user_agent);
}

static gboolean
finish_stream (FetcherRequest *request, GCancellable *cancellable, GError **error)
{
  /* Close it here since we do an async fstat(), where we don't want
   * to hit a bad fd.
   */
  if (request->out_stream)
    {
      if ((request->flags & OSTREE_FETCHER_REQUEST_NUL_TERMINATION) > 0)
        {
          const guint8 nulchar = 0;

          if (!g_output_stream_write_all (request->out_stream, &nulchar, 1, NULL, cancellable,
                                          error))
            return FALSE;
        }

      if (!g_output_stream_close (request->out_stream, cancellable, error))
        return FALSE;
    }

  if (!request->is_membuf)
    {
      struct stat stbuf;

      if (!glnx_fstat (request->tmpf.fd, &stbuf, error))
        return FALSE;

      if (request->content_length >= 0 && stbuf.st_size < request->content_length)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Download incomplete");
          return FALSE;
        }
    }

  return TRUE;
}

static void on_stream_read (GObject *object, GAsyncResult *result, gpointer user_data);

static void
on_out_splice_complete (GObject *object, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (GTask) task = G_TASK (user_data);
  GError *local_error = NULL;

  gssize bytes_written
      = g_output_stream_splice_finish ((GOutputStream *)object, result, &local_error);
  if (bytes_written < 0)
    {
      g_task_return_error (task, local_error);
      return;
    }

  FetcherRequest *request = g_task_get_task_data (task);
  request->fetcher->bytes_transferred += bytes_written;

  GCancellable *cancellable = g_task_get_cancellable (task);
  g_input_stream_read_bytes_async (request->response_body, 8192, G_PRIORITY_DEFAULT, cancellable,
                                   on_stream_read, g_object_ref (task));
}

static void
on_stream_read (GObject *object, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (GTask) task = G_TASK (user_data);
  FetcherRequest *request = g_task_get_task_data (task);
  GError *local_error = NULL;

  /* Only open the output stream on demand to ensure we use as
   * few file descriptors as possible.
   */
  if (!request->out_stream)
    {
      if (!request->is_membuf)
        {
          if (!_ostree_fetcher_tmpf_from_flags (request->flags, request->fetcher->tmpdir_dfd,
                                                &request->tmpf, &local_error))
            {
              g_task_return_error (task, local_error);
              return;
            }
          request->out_stream = g_unix_output_stream_new (request->tmpf.fd, FALSE);
        }
      else
        {
          request->out_stream = g_memory_output_stream_new_resizable ();
        }
    }

  /* Get a GBytes buffer */
  g_autoptr (GBytes) bytes
      = g_input_stream_read_bytes_finish ((GInputStream *)object, result, &local_error);
  if (!bytes)
    {
      g_task_return_error (task, local_error);
      return;
    }

  /* Was this the end of the stream? */
  GCancellable *cancellable = g_task_get_cancellable (task);
  gsize bytes_read = g_bytes_get_size (bytes);
  if (bytes_read == 0)
    {
      if (!finish_stream (request, cancellable, &local_error))
        {
          g_task_return_error (task, local_error);
          return;
        }
      if (request->is_membuf)
        {
          GBytes *mem_bytes
              = g_memory_output_stream_steal_as_bytes ((GMemoryOutputStream *)request->out_stream);
          g_task_return_pointer (task, mem_bytes, (GDestroyNotify)g_bytes_unref);
        }
      else
        {
          if (lseek (request->tmpf.fd, 0, SEEK_SET) < 0)
            {
              glnx_set_error_from_errno (&local_error);
              g_task_return_error (task, g_steal_pointer (&local_error));
              return;
            }

          g_task_return_boolean (task, TRUE);
        }
    }
  else
    {
      /* Verify max size */
      if (request->max_size > 0)
        {
          if (bytes_read > request->max_size
              || (bytes_read + request->current_size) > request->max_size)
            {
              g_autofree char *uristr = NULL;

              if (request->file)
                uristr = g_file_get_uri (request->file);
              else
                uristr = g_uri_to_string (soup_message_get_uri (request->message));

              local_error
                  = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "URI %s exceeded maximum size of %" G_GUINT64_FORMAT " bytes",
                                 uristr, request->max_size);
              g_task_return_error (task, local_error);
              return;
            }
        }

      request->current_size += bytes_read;

      /* We do this instead of _write_bytes_async() as that's not
       * guaranteed to do a complete write.
       */
      {
        g_autoptr (GInputStream) membuf = g_memory_input_stream_new_from_bytes (bytes);
        g_output_stream_splice_async (request->out_stream, membuf,
                                      G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE, G_PRIORITY_DEFAULT,
                                      cancellable, on_out_splice_complete, g_object_ref (task));
      }
    }
}

static void
on_request_sent (GObject *object, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (GTask) task = G_TASK (user_data);
  FetcherRequest *request = g_task_get_task_data (task);
  GError *local_error = NULL;

  if (request->file)
    request->response_body
        = (GInputStream *)g_file_read_finish ((GFile *)object, result, &local_error);
  else
    request->response_body = soup_session_send_finish ((SoupSession *)object, result, &local_error);

  if (!request->response_body)
    {
      g_task_return_error (task, local_error);
      return;
    }

  if (request->message)
    {
      SoupStatus status = soup_message_get_status (request->message);
      if (status == SOUP_STATUS_NOT_MODIFIED
          && (request->if_none_match != NULL || request->if_modified_since > 0))
        {
          /* Version on the server is unchanged from the version we have cached locally;
           * report this as an out-argument, a zero-length response buffer, and no error */
          request->out_not_modified = TRUE;
        }
      else if (!SOUP_STATUS_IS_SUCCESSFUL (status))
        {
          /* is there another mirror we can try? */
          if (request->mirrorlist_idx + 1 < request->mirrorlist->len)
            {
              request->mirrorlist_idx++;
              initiate_task_request (g_object_ref (task));
              return;
            }
          else
            {
              g_autofree char *uristring
                  = g_uri_to_string (soup_message_get_uri (request->message));
              GIOErrorEnum code = _ostree_fetcher_http_status_code_to_io_error (status);
              {
                g_autofree char *errmsg = g_strdup_printf ("Server returned status %u: %s", status,
                                                           soup_status_get_phrase (status));
                local_error = g_error_new_literal (G_IO_ERROR, code, errmsg);
              }

              if (request->mirrorlist->len > 1)
                g_prefix_error (&local_error, "All %u mirrors failed. Last error was: ",
                                request->mirrorlist->len);
              if (request->fetcher->remote_name
                  && !((request->flags & OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT) > 0
                       && code == G_IO_ERROR_NOT_FOUND))
                _ostree_fetcher_journal_failure (request->fetcher->remote_name, uristring,
                                                 local_error->message);

              g_task_return_error (task, local_error);
              return;
            }
        }

      /* Grab cache properties from the response */
      request->out_etag = g_strdup (soup_message_headers_get_one (
          soup_message_get_response_headers (request->message), "ETag"));
      request->out_last_modified = 0;

      const char *last_modified_str = soup_message_headers_get_one (
          soup_message_get_response_headers (request->message), "Last-Modified");
      if (last_modified_str != NULL)
        {
          GDateTime *soup_date = soup_date_time_new_from_http_string (last_modified_str);
          if (soup_date != NULL)
            {
              request->out_last_modified = g_date_time_to_unix (soup_date);
              g_date_time_unref (soup_date);
            }
        }
    }

  if (request->file)
    {
      GFileInfo *info = g_file_query_info (
          request->file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
          0, NULL, NULL);
      if (info)
        {
          request->content_length = g_file_info_get_size (info);
          g_object_unref (info);
        }
      else
        request->content_length = -1;
    }
  else
    {
      SoupMessageHeaders *headers = soup_message_get_response_headers (request->message);
      if (soup_message_headers_get_list (headers, "Content-Encoding") == NULL)
        request->content_length = soup_message_headers_get_content_length (headers);
      else
        request->content_length = -1;
    }

  GCancellable *cancellable = g_task_get_cancellable (task);
  g_input_stream_read_bytes_async (request->response_body, 8192, G_PRIORITY_DEFAULT, cancellable,
                                   on_stream_read, g_object_ref (task));
}

static SoupSession *
create_soup_session (OstreeFetcher *self)
{
  const char *user_agent = self->user_agent ?: OSTREE_FETCHER_USERAGENT_STRING;
  SoupSession *session = soup_session_new_with_options (
      "user-agent", user_agent, "timeout", 60, "idle-timeout", 60, "max-conns-per-host",
      _OSTREE_MAX_OUTSTANDING_FETCHER_REQUESTS, NULL);

  /* SoupContentDecoder is included in the session by default. Remove it
   * if gzip compression isn't in use.
   */
  if ((self->config_flags & OSTREE_FETCHER_FLAGS_TRANSFER_GZIP) == 0)
    soup_session_remove_feature_by_type (session, SOUP_TYPE_CONTENT_DECODER);

  if (g_getenv ("OSTREE_DEBUG_HTTP"))
    {
      glnx_unref_object SoupLogger *logger = soup_logger_new (SOUP_LOGGER_LOG_BODY);
      soup_session_add_feature (session, SOUP_SESSION_FEATURE (logger));
    }

  if (self->proxy_resolver != NULL)
    soup_session_set_proxy_resolver (session, self->proxy_resolver);
  if (self->cookie_jar != NULL)
    soup_session_add_feature (session, SOUP_SESSION_FEATURE (self->cookie_jar));
  if (self->client_cert != NULL)
    soup_session_set_tls_interaction (session, (GTlsInteraction *)self->client_cert);
  if (self->tls_database != NULL)
    soup_session_set_tls_database (session, self->tls_database);

  return session;
}

static gboolean
match_value (gpointer key, gpointer value, gpointer user_data)
{
  return value == user_data;
}

static void
on_session_finalized (gpointer data, GObject *object)
{
  GHashTable *sessions = data;
  g_debug ("Removing session %p from sessions hash table", object);
  (void)g_hash_table_foreach_remove (sessions, match_value, object);
}

static void
_ostree_fetcher_request_async (OstreeFetcher *self, GPtrArray *mirrorlist, const char *filename,
                               OstreeFetcherRequestFlags flags, const char *if_none_match,
                               guint64 if_modified_since, gboolean is_membuf, guint64 max_size,
                               int priority, GCancellable *cancellable,
                               GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail (OSTREE_IS_FETCHER (self));
  g_return_if_fail (mirrorlist != NULL);
  g_return_if_fail (mirrorlist->len > 0);

  FetcherRequest *request = g_new0 (FetcherRequest, 1);
  request->mirrorlist = g_ptr_array_ref (mirrorlist);
  request->filename = g_strdup (filename);
  request->flags = flags;
  request->if_none_match = g_strdup (if_none_match);
  request->if_modified_since = if_modified_since;
  request->max_size = max_size;
  request->is_membuf = is_membuf;
  request->fetcher = self;
  request->mainctx = g_main_context_ref_thread_default ();

  /* Ideally each fetcher would have a single soup session. However, each
   * session needs to be used from a single main context and the fetcher
   * doesn't have that limitation. Instead, a mapping from main context to
   * session is kept in the fetcher.
   */
  g_debug ("Looking up session for main context %p", request->mainctx);
  request->session = (SoupSession *)g_hash_table_lookup (self->sessions, request->mainctx);
  if (request->session != NULL)
    {
      g_debug ("Using existing session %p", request->session);
      g_object_ref (request->session);
    }
  else
    {
      request->session = create_soup_session (self);
      g_debug ("Created new session %p", request->session);
      g_hash_table_insert (self->sessions, request->mainctx, request->session);

      /* Add a weak ref to the session so that when it's finalized it can be
       * removed from the hash table.
       */
      g_object_weak_ref (G_OBJECT (request->session), on_session_finalized, self->sessions);
    }

  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, _ostree_fetcher_request_async);
  g_task_set_task_data (task, request, (GDestroyNotify)fetcher_request_free);

  /* We'll use the GTask priority for our own priority queue. */
  g_task_set_priority (task, priority);

  initiate_task_request (g_object_ref (task));
}

void
_ostree_fetcher_request_to_tmpfile (OstreeFetcher *self, GPtrArray *mirrorlist,
                                    const char *filename, OstreeFetcherRequestFlags flags,
                                    const char *if_none_match, guint64 if_modified_since,
                                    guint64 max_size, int priority, GCancellable *cancellable,
                                    GAsyncReadyCallback callback, gpointer user_data)
{
  _ostree_fetcher_request_async (self, mirrorlist, filename, flags, if_none_match,
                                 if_modified_since, FALSE, max_size, priority, cancellable,
                                 callback, user_data);
}

gboolean
_ostree_fetcher_request_to_tmpfile_finish (OstreeFetcher *self, GAsyncResult *result,
                                           GLnxTmpfile *out_tmpf, gboolean *out_not_modified,
                                           char **out_etag, guint64 *out_last_modified,
                                           GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, _ostree_fetcher_request_async), FALSE);

  GTask *task = (GTask *)result;
  gpointer ret = g_task_propagate_pointer (task, error);
  if (!ret)
    return FALSE;

  FetcherRequest *request = g_task_get_task_data (task);
  g_assert (!request->is_membuf);
  *out_tmpf = request->tmpf;
  request->tmpf.initialized = FALSE; /* Transfer ownership */

  if (out_not_modified != NULL)
    *out_not_modified = request->out_not_modified;
  if (out_etag != NULL)
    *out_etag = g_steal_pointer (&request->out_etag);
  if (out_last_modified != NULL)
    *out_last_modified = request->out_last_modified;

  return TRUE;
}

void
_ostree_fetcher_request_to_membuf (OstreeFetcher *self, GPtrArray *mirrorlist, const char *filename,
                                   OstreeFetcherRequestFlags flags, const char *if_none_match,
                                   guint64 if_modified_since, guint64 max_size, int priority,
                                   GCancellable *cancellable, GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  _ostree_fetcher_request_async (self, mirrorlist, filename, flags, if_none_match,
                                 if_modified_since, TRUE, max_size, priority, cancellable, callback,
                                 user_data);
}

gboolean
_ostree_fetcher_request_to_membuf_finish (OstreeFetcher *self, GAsyncResult *result,
                                          GBytes **out_buf, gboolean *out_not_modified,
                                          char **out_etag, guint64 *out_last_modified,
                                          GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, _ostree_fetcher_request_async), FALSE);

  GTask *task = (GTask *)result;
  gpointer ret = g_task_propagate_pointer (task, error);
  if (!ret)
    return FALSE;

  FetcherRequest *request = g_task_get_task_data (task);
  g_assert (request->is_membuf);
  g_assert (out_buf);
  *out_buf = ret;

  if (out_not_modified != NULL)
    *out_not_modified = request->out_not_modified;
  if (out_etag != NULL)
    *out_etag = g_steal_pointer (&request->out_etag);
  if (out_last_modified != NULL)
    *out_last_modified = request->out_last_modified;

  return TRUE;
}

guint64
_ostree_fetcher_bytes_transferred (OstreeFetcher *self)
{
  return self->bytes_transferred;
}
