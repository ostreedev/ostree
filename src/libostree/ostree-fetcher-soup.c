/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <gio/gio.h>
#include <gio/gfiledescriptorbased.h>
#include <gio/gunixoutputstream.h>
#define LIBSOUP_USE_UNSTABLE_REQUEST_API
#include <libsoup/soup.h>
#include <libsoup/soup-requester.h>
#include <libsoup/soup-request-http.h>

#include "libglnx.h"
#include "ostree-fetcher.h"
#include "ostree-fetcher-util.h"
#ifdef HAVE_LIBSOUP_CLIENT_CERTS
#include "ostree-tls-cert-interaction.h"
#endif
#include "ostree-enumtypes.h"
#include "ostree.h"
#include "ostree-repo-private.h"
#include "otutil.h"

typedef enum {
  OSTREE_FETCHER_STATE_PENDING,
  OSTREE_FETCHER_STATE_DOWNLOADING,
  OSTREE_FETCHER_STATE_COMPLETE
} OstreeFetcherState;

typedef struct {
  volatile int ref_count;

  SoupSession *session;  /* not referenced */
  GMainContext *main_context;
  volatile gint running;
  GError *initialization_error; /* Any failure to load the db */

  char *remote_name;
  int base_tmpdir_dfd;

  GVariant *extra_headers;
  gboolean transfer_gzip;

  /* Our active HTTP requests */
  GHashTable *outstanding;

  /* Shared across threads; be sure to lock. */
  GHashTable *output_stream_set;  /* set<GOutputStream> */
  GMutex output_stream_set_lock;

  /* Also protected by output_stream_set_lock. */
  guint64 total_downloaded;

  GError *oob_error;

} ThreadClosure;

typedef struct {
  volatile int ref_count;

  ThreadClosure *thread_closure;
  GPtrArray *mirrorlist; /* list of base URIs */
  char *filename; /* relative name to fetch or NULL */
  guint mirrorlist_idx;

  OstreeFetcherState state;

  SoupRequest *request;

  gboolean is_membuf;
  OstreeFetcherRequestFlags flags;
  GInputStream *request_body;
  GLnxTmpfile tmpf;
  GOutputStream *out_stream;

  guint64 max_size;
  guint64 current_size;
  guint64 content_length;
} OstreeFetcherPendingURI;

/* Used by session_thread_idle_add() */
typedef void (*SessionThreadFunc) (ThreadClosure *thread_closure,
                                   gpointer data);

/* Used by session_thread_idle_add() */
typedef struct {
  ThreadClosure *thread_closure;
  SessionThreadFunc function;
  gpointer data;
  GDestroyNotify notify;
} IdleClosure;

struct OstreeFetcher
{
  GObject parent_instance;

  OstreeFetcherConfigFlags config_flags;

  GThread *session_thread;
  ThreadClosure *thread_closure;
};

enum {
  PROP_0,
  PROP_CONFIG_FLAGS
};

G_DEFINE_TYPE (OstreeFetcher, _ostree_fetcher, G_TYPE_OBJECT)

static ThreadClosure *
thread_closure_ref (ThreadClosure *thread_closure)
{
  int refcount;
  g_return_val_if_fail (thread_closure != NULL, NULL);
  refcount = g_atomic_int_add (&thread_closure->ref_count, 1);
  g_assert (refcount > 0);
  return thread_closure;
}

static void
thread_closure_unref (ThreadClosure *thread_closure)
{
  g_return_if_fail (thread_closure != NULL);

  if (g_atomic_int_dec_and_test (&thread_closure->ref_count))
    {
      /* The session thread should have cleared this by now. */
      g_assert (thread_closure->session == NULL);

      g_clear_pointer (&thread_closure->main_context, g_main_context_unref);

      g_clear_pointer (&thread_closure->extra_headers, (GDestroyNotify)g_variant_unref);

      g_clear_pointer (&thread_closure->output_stream_set, g_hash_table_unref);
      g_mutex_clear (&thread_closure->output_stream_set_lock);

      g_clear_pointer (&thread_closure->oob_error, g_error_free);

      g_free (thread_closure->remote_name);

      g_slice_free (ThreadClosure, thread_closure);
    }
}

static void
idle_closure_free (IdleClosure *idle_closure)
{
  g_clear_pointer (&idle_closure->thread_closure, thread_closure_unref);

  if (idle_closure->notify != NULL)
    idle_closure->notify (idle_closure->data);

  g_slice_free (IdleClosure, idle_closure);
}

static OstreeFetcherPendingURI *
pending_uri_ref (OstreeFetcherPendingURI *pending)
{
  gint refcount;
  g_return_val_if_fail (pending != NULL, NULL);
  refcount = g_atomic_int_add (&pending->ref_count, 1);
  g_assert (refcount > 0);
  return pending;
}

static void
pending_uri_unref (OstreeFetcherPendingURI *pending)
{
  if (!g_atomic_int_dec_and_test (&pending->ref_count))
    return;

  g_clear_pointer (&pending->thread_closure, thread_closure_unref);

  g_clear_pointer (&pending->mirrorlist, g_ptr_array_unref);
  g_free (pending->filename);
  g_clear_object (&pending->request);
  g_clear_object (&pending->request_body);
  glnx_tmpfile_clear (&pending->tmpf);
  g_clear_object (&pending->out_stream);
  g_free (pending);
}

static gboolean
session_thread_idle_dispatch (gpointer data)
{
  IdleClosure *idle_closure = data;

  idle_closure->function (idle_closure->thread_closure,
                          idle_closure->data);

  return G_SOURCE_REMOVE;
}

static void
session_thread_idle_add (ThreadClosure *thread_closure,
                         SessionThreadFunc function,
                         gpointer data,
                         GDestroyNotify notify)
{
  IdleClosure *idle_closure;

  g_return_if_fail (thread_closure != NULL);
  g_return_if_fail (function != NULL);

  idle_closure = g_slice_new (IdleClosure);
  idle_closure->thread_closure = thread_closure_ref (thread_closure);
  idle_closure->function = function;
  idle_closure->data = data;
  idle_closure->notify = notify;

  g_main_context_invoke_full (thread_closure->main_context,
                              G_PRIORITY_DEFAULT,
                              session_thread_idle_dispatch,
                              idle_closure,  /* takes ownership */
                              (GDestroyNotify) idle_closure_free);
}

static void
session_thread_add_logger (ThreadClosure *thread_closure,
                           gpointer data)
{
  glnx_unref_object SoupLogger *logger = NULL;

  logger = soup_logger_new (SOUP_LOGGER_LOG_BODY, 500);
  soup_session_add_feature (thread_closure->session,
                            SOUP_SESSION_FEATURE (logger));
}

static void
session_thread_config_flags (ThreadClosure *thread_closure,
                             gpointer data)
{
  OstreeFetcherConfigFlags config_flags;

  config_flags = GPOINTER_TO_UINT (data);

  if ((config_flags & OSTREE_FETCHER_FLAGS_TLS_PERMISSIVE) > 0)
    {
      g_object_set (thread_closure->session,
                    SOUP_SESSION_SSL_STRICT,
                    FALSE, NULL);
    }
}

static void
on_authenticate (SoupSession *session, SoupMessage *msg, SoupAuth *auth,
                 gboolean retrying, gpointer user_data)
{
  ThreadClosure *thread_closure = user_data;

  if (msg->status_code == SOUP_STATUS_PROXY_UNAUTHORIZED)
    {
      SoupURI *uri = NULL;
      g_object_get (session, SOUP_SESSION_PROXY_URI, &uri, NULL);
      if (retrying)
        {
          g_autofree char *s = soup_uri_to_string (uri, FALSE);
          g_set_error (&thread_closure->oob_error,
                       G_IO_ERROR, G_IO_ERROR_PROXY_AUTH_FAILED,
                       "Invalid username or password for proxy '%s'", s);
        }
      else
        soup_auth_authenticate (auth, soup_uri_get_user (uri),
                                      soup_uri_get_password (uri));
    }
}

static void
session_thread_set_proxy_cb (ThreadClosure *thread_closure,
                             gpointer data)
{
  SoupURI *proxy_uri = data;

  g_object_set (thread_closure->session,
                SOUP_SESSION_PROXY_URI,
                proxy_uri, NULL);

  /* libsoup won't necessarily pass any embedded username and password to proxy
   * requests, so we have to be ready to handle 407 and handle them ourselves.
   * See also: https://bugzilla.gnome.org/show_bug.cgi?id=772932
   * */
  if (soup_uri_get_user (proxy_uri) &&
      soup_uri_get_password (proxy_uri))
    {
      g_signal_connect (thread_closure->session, "authenticate",
                        G_CALLBACK (on_authenticate), thread_closure);
    }
}

static void
session_thread_set_cookie_jar_cb (ThreadClosure *thread_closure,
                                  gpointer data)
{
  SoupCookieJar *jar = data;

  soup_session_add_feature (thread_closure->session,
                            SOUP_SESSION_FEATURE (jar));
}

static void
session_thread_set_headers_cb (ThreadClosure *thread_closure,
                               gpointer data)
{
  GVariant *headers = data;

  g_clear_pointer (&thread_closure->extra_headers, (GDestroyNotify)g_variant_unref);
  thread_closure->extra_headers = g_variant_ref (headers);
}

#ifdef HAVE_LIBSOUP_CLIENT_CERTS
static void
session_thread_set_tls_interaction_cb (ThreadClosure *thread_closure,
                                       gpointer data)
{
  const char *cert_and_key_path = data; /* str\0str\0 in one malloc buf */
  const char *cert_path = cert_and_key_path;
  const char *key_path = cert_and_key_path + strlen (cert_and_key_path) + 1;
  g_autoptr(OstreeTlsCertInteraction) interaction = NULL;

  /* The GTlsInteraction instance must be created in the
   * session thread so it uses the correct GMainContext. */
  interaction = _ostree_tls_cert_interaction_new (cert_path, key_path);

  g_object_set (thread_closure->session,
                SOUP_SESSION_TLS_INTERACTION,
                interaction, NULL);
}
#endif

static void
session_thread_set_tls_database_cb (ThreadClosure *thread_closure,
                                    gpointer data)
{
  const char *db_path = data;

  if (db_path != NULL)
    {
      glnx_unref_object GTlsDatabase *tlsdb = NULL;

      g_clear_error (&thread_closure->initialization_error);
      tlsdb = g_tls_file_database_new (db_path, &thread_closure->initialization_error);

      if (tlsdb)
        g_object_set (thread_closure->session,
                      SOUP_SESSION_TLS_DATABASE,
                      tlsdb, NULL);
    }
  else
    {
      g_object_set (thread_closure->session,
                    SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE,
                    TRUE, NULL);
    }
}

static void
on_request_sent (GObject        *object, GAsyncResult   *result, gpointer        user_data);

static void
start_pending_request (ThreadClosure *thread_closure,
                       GTask         *task)
{

  OstreeFetcherPendingURI *pending;
  GCancellable *cancellable;

  pending = g_task_get_task_data (task);
  cancellable = g_task_get_cancellable (task);

  g_hash_table_add (thread_closure->outstanding, pending_uri_ref (pending));
  soup_request_send_async (pending->request,
                           cancellable,
                           on_request_sent,
                           g_object_ref (task));
}

static void
create_pending_soup_request (OstreeFetcherPendingURI  *pending,
                             GError                  **error)
{
  OstreeFetcherURI *next_mirror = NULL;
  g_autoptr(OstreeFetcherURI) uri = NULL;

  g_assert (pending->mirrorlist);
  g_assert (pending->mirrorlist_idx < pending->mirrorlist->len);

  next_mirror = g_ptr_array_index (pending->mirrorlist, pending->mirrorlist_idx);
  if (pending->filename)
    uri = _ostree_fetcher_uri_new_subpath (next_mirror, pending->filename);

  g_clear_object (&pending->request);

  pending->request = soup_session_request_uri (pending->thread_closure->session,
                                               (SoupURI*)(uri ? uri : next_mirror), error);
}

static void
session_thread_request_uri (ThreadClosure *thread_closure,
                            gpointer data)
{
  GTask *task = G_TASK (data);
  OstreeFetcherPendingURI *pending;
  GCancellable *cancellable;
  GError *local_error = NULL;

  pending = g_task_get_task_data (task);
  cancellable = g_task_get_cancellable (task);

  /* If we caught an error in init, re-throw it for every request */
  if (thread_closure->initialization_error)
    {
      g_task_return_error (task, g_error_copy (thread_closure->initialization_error));
      return;
    }

  create_pending_soup_request (pending, &local_error);
  if (local_error != NULL)
    {
      g_task_return_error (task, local_error);
      return;
    }

  if (SOUP_IS_REQUEST_HTTP (pending->request) && thread_closure->extra_headers)
    {
      glnx_unref_object SoupMessage *msg = soup_request_http_get_message ((SoupRequestHTTP*) pending->request);
      g_autoptr(GVariantIter) viter = g_variant_iter_new (thread_closure->extra_headers);
      const char *key;
      const char *value;

      while (g_variant_iter_next (viter, "(&s&s)", &key, &value))
        soup_message_headers_append (msg->request_headers, key, value);
    }

  if (pending->is_membuf)
    {
      soup_request_send_async (pending->request,
                               cancellable,
                               on_request_sent,
                               g_object_ref (task));
    }
  else
    {
     start_pending_request (thread_closure, task);
    }
}

static gpointer
ostree_fetcher_session_thread (gpointer data)
{
  ThreadClosure *closure = data;
  g_autoptr(GMainContext) mainctx = g_main_context_ref (closure->main_context);

  /* This becomes the GMainContext that SoupSession schedules async
   * callbacks and emits signals from.  Make it the thread-default
   * context for this thread before creating the session. */
  g_main_context_push_thread_default (mainctx);

  /* We retain ownership of the SoupSession reference. */
  closure->session = soup_session_async_new_with_options (SOUP_SESSION_USER_AGENT, OSTREE_FETCHER_USERAGENT_STRING,
                                                          SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
                                                          SOUP_SESSION_USE_THREAD_CONTEXT, TRUE,
                                                          SOUP_SESSION_ADD_FEATURE_BY_TYPE, SOUP_TYPE_REQUESTER,
                                                          SOUP_SESSION_TIMEOUT, 60,
                                                          SOUP_SESSION_IDLE_TIMEOUT, 60,
                                                          NULL);

  if (closure->transfer_gzip)
    soup_session_add_feature_by_type (closure->session, SOUP_TYPE_CONTENT_DECODER);

  /* XXX: Now that we have mirrorlist support, we could make this even smarter
   * by spreading requests across mirrors. */
  gint max_conns;
  g_object_get (closure->session, "max-conns-per-host", &max_conns, NULL);
  if (max_conns < _OSTREE_MAX_OUTSTANDING_FETCHER_REQUESTS)
    {
      /* We download a lot of small objects in ostree, so this
       * helps a lot.  Also matches what most modern browsers do.
       *
       * Note since https://github.com/ostreedev/ostree/commit/f4d1334e19ce3ab2f8872b1e28da52044f559401
       * we don't do queuing in this libsoup backend, but we still
       * want to override libsoup's currently conservative
       * #define SOUP_SESSION_MAX_CONNS_PER_HOST_DEFAULT 2 (as of 2018-02-14).
       */
      max_conns = _OSTREE_MAX_OUTSTANDING_FETCHER_REQUESTS;
      g_object_set (closure->session,
                    "max-conns-per-host",
                    max_conns, NULL);
    }

  /* This model ensures we don't hit a race using g_main_loop_quit();
   * see also what pull_termination_condition() in ostree-repo-pull.c
   * is doing.
   */
  while (g_atomic_int_get (&closure->running))
    g_main_context_iteration (closure->main_context, TRUE);

  /* Since the ThreadClosure may be finalized from any thread we
   * unreference all data related to the SoupSession ourself to ensure
   * it's freed in the same thread where it was created. */
  g_clear_pointer (&closure->outstanding, g_hash_table_unref);
  g_clear_pointer (&closure->session, g_object_unref);

  thread_closure_unref (closure);

  /* Do this last, since libsoup uses g_main_current_source() which
   * relies on it.
   */
  g_main_context_pop_thread_default (mainctx);

  return NULL;
}

static void
_ostree_fetcher_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
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
_ostree_fetcher_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
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

  /* Terminate the session thread. */
  g_atomic_int_set (&self->thread_closure->running, 0);
  g_main_context_wakeup (self->thread_closure->main_context);
  if (self->session_thread)
    {
      /* We need to explicitly synchronize to clean up TLS */
      if (self->session_thread != g_thread_self ())
        g_thread_join (self->session_thread);
      else
        g_clear_pointer (&self->session_thread, g_thread_unref);
    }
  g_clear_pointer (&self->thread_closure, thread_closure_unref);

  G_OBJECT_CLASS (_ostree_fetcher_parent_class)->finalize (object);
}

static void
_ostree_fetcher_constructed (GObject *object)
{
  OstreeFetcher *self = OSTREE_FETCHER (object);
  g_autoptr(GMainContext) main_context = NULL;
  const char *http_proxy;

  main_context = g_main_context_new ();

  self->thread_closure = g_slice_new0 (ThreadClosure);
  self->thread_closure->ref_count = 1;
  self->thread_closure->main_context = g_main_context_ref (main_context);
  self->thread_closure->running = 1;
  self->thread_closure->transfer_gzip = (self->config_flags & OSTREE_FETCHER_FLAGS_TRANSFER_GZIP) != 0;

  self->thread_closure->outstanding = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)pending_uri_unref);
  self->thread_closure->output_stream_set = g_hash_table_new_full (NULL, NULL,
                                                                   (GDestroyNotify) NULL,
                                                                   (GDestroyNotify) g_object_unref);
  g_mutex_init (&self->thread_closure->output_stream_set_lock);

  if (g_getenv ("OSTREE_DEBUG_HTTP"))
    {
      session_thread_idle_add (self->thread_closure,
                               session_thread_add_logger,
                               NULL, (GDestroyNotify) NULL);
    }

  if (self->config_flags != 0)
    {
      session_thread_idle_add (self->thread_closure,
                               session_thread_config_flags,
                               GUINT_TO_POINTER (self->config_flags),
                               (GDestroyNotify) NULL);
    }

  http_proxy = g_getenv ("http_proxy");
  if (http_proxy != NULL)
    _ostree_fetcher_set_proxy (self, http_proxy);

  /* FIXME Maybe implement GInitableIface and use g_thread_try_new()
   *       so we can try to handle thread creation errors gracefully? */
  self->session_thread = g_thread_new ("fetcher-session-thread",
                                       ostree_fetcher_session_thread,
                                       thread_closure_ref (self->thread_closure));

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

  g_object_class_install_property (gobject_class,
                                   PROP_CONFIG_FLAGS,
                                   g_param_spec_flags ("config-flags",
                                                       "",
                                                       "",
                                                       OSTREE_TYPE_FETCHER_CONFIG_FLAGS,
                                                       OSTREE_FETCHER_FLAGS_NONE,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_STATIC_STRINGS));
}

static void
_ostree_fetcher_init (OstreeFetcher *self)
{
}

OstreeFetcher *
_ostree_fetcher_new (int                      tmpdir_dfd,
                     const char              *remote_name,
                     OstreeFetcherConfigFlags flags)
{
  OstreeFetcher *self;

  self = g_object_new (OSTREE_TYPE_FETCHER, "config-flags", flags, NULL);
  self->thread_closure->remote_name = g_strdup (remote_name);
  self->thread_closure->base_tmpdir_dfd = tmpdir_dfd;

  return self;
}

int
_ostree_fetcher_get_dfd (OstreeFetcher *fetcher)
{
  return fetcher->thread_closure->base_tmpdir_dfd;
}

void
_ostree_fetcher_set_proxy (OstreeFetcher *self,
                           const char    *http_proxy)
{
  SoupURI *proxy_uri;

  g_return_if_fail (OSTREE_IS_FETCHER (self));
  g_return_if_fail (http_proxy != NULL);

  proxy_uri = soup_uri_new (http_proxy);

  if (!proxy_uri)
    {
      g_warning ("Invalid proxy URI '%s'", http_proxy);
    }
  else
    {
      session_thread_idle_add (self->thread_closure,
                               session_thread_set_proxy_cb,
                               proxy_uri,  /* takes ownership */
                               (GDestroyNotify) soup_uri_free);
    }
}

void
_ostree_fetcher_set_cookie_jar (OstreeFetcher *self,
                                const char    *jar_path)
{
  SoupCookieJar *jar;

  g_return_if_fail (OSTREE_IS_FETCHER (self));
  g_return_if_fail (jar_path != NULL);

  jar = soup_cookie_jar_text_new (jar_path, TRUE);

  session_thread_idle_add (self->thread_closure,
                           session_thread_set_cookie_jar_cb,
                           jar,  /* takes ownership */
                           (GDestroyNotify) g_object_unref);
}

void
_ostree_fetcher_set_client_cert (OstreeFetcher   *self,
                                 const char      *cert_path,
                                 const char      *key_path)
{
  g_autoptr(GString) buf = NULL;
  g_return_if_fail (OSTREE_IS_FETCHER (self));

  if (cert_path)
    {
      buf = g_string_new (cert_path);
      g_string_append_c (buf, '\0');
      g_string_append (buf, key_path);
    }

#ifdef HAVE_LIBSOUP_CLIENT_CERTS
  session_thread_idle_add (self->thread_closure,
                           session_thread_set_tls_interaction_cb,
                           g_string_free (g_steal_pointer (&buf), FALSE),
                           (GDestroyNotify) g_free);
#else
  g_warning ("This version of OSTree is compiled without client side certificate support");
#endif
}

void
_ostree_fetcher_set_tls_database (OstreeFetcher *self,
                                  const char    *tlsdb_path)
{
  g_return_if_fail (OSTREE_IS_FETCHER (self));

  session_thread_idle_add (self->thread_closure,
                           session_thread_set_tls_database_cb,
                           g_strdup (tlsdb_path),
                           (GDestroyNotify) g_free);
}

void
_ostree_fetcher_set_extra_headers (OstreeFetcher *self,
                                   GVariant      *extra_headers)
{
  session_thread_idle_add (self->thread_closure,
                           session_thread_set_headers_cb,
                           g_variant_ref (extra_headers),
                           (GDestroyNotify) g_variant_unref);
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
      if ((pending->flags & OSTREE_FETCHER_REQUEST_NUL_TERMINATION) > 0)
        {
          const guint8 nulchar = 0;
          gsize bytes_written;

          if (!g_output_stream_write_all (pending->out_stream, &nulchar, 1, &bytes_written,
                                          cancellable, error))
            goto out;
        }

      if (!g_output_stream_close (pending->out_stream, cancellable, error))
        goto out;

      g_mutex_lock (&pending->thread_closure->output_stream_set_lock);
      g_hash_table_remove (pending->thread_closure->output_stream_set,
                           pending->out_stream);
      g_mutex_unlock (&pending->thread_closure->output_stream_set_lock);
    }

  if (!pending->is_membuf)
    {
      if (!glnx_fstat (pending->tmpf.fd, &stbuf, error))
        goto out;
    }

  pending->state = OSTREE_FETCHER_STATE_COMPLETE;

  if (!pending->is_membuf)
    {
      if (stbuf.st_size < pending->content_length)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Download incomplete");
          goto out;
        }
      else
        {
          g_mutex_lock (&pending->thread_closure->output_stream_set_lock);
          pending->thread_closure->total_downloaded += stbuf.st_size;
          g_mutex_unlock (&pending->thread_closure->output_stream_set_lock);
        }
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
remove_pending (OstreeFetcherPendingURI *pending)
{
  /* Hold a temporary ref to ensure the reference to
   * pending->thread_closure is valid.
   */
  pending_uri_ref (pending);
  g_hash_table_remove (pending->thread_closure->outstanding, pending);
  pending_uri_unref (pending);
}

static void
on_out_splice_complete (GObject        *object,
                        GAsyncResult   *result,
                        gpointer        user_data) 
{
  GTask *task = G_TASK (user_data);
  OstreeFetcherPendingURI *pending;
  GCancellable *cancellable;
  gssize bytes_written;
  GError *local_error = NULL;

  pending = g_task_get_task_data (task);
  cancellable = g_task_get_cancellable (task);

  bytes_written = g_output_stream_splice_finish ((GOutputStream *)object,
                                                 result,
                                                 &local_error);
  if (bytes_written < 0)
    goto out;

  g_input_stream_read_bytes_async (pending->request_body,
                                   8192, G_PRIORITY_DEFAULT,
                                   cancellable,
                                   on_stream_read,
                                   g_object_ref (task));

 out:
  if (local_error)
    {
      g_task_return_error (task, local_error);
      remove_pending (pending);
    }

  g_object_unref (task);
}

static void
on_stream_read (GObject        *object,
                GAsyncResult   *result,
                gpointer        user_data) 
{
  GTask *task = G_TASK (user_data);
  OstreeFetcherPendingURI *pending;
  GCancellable *cancellable;
  g_autoptr(GBytes) bytes = NULL;
  gsize bytes_read;
  GError *local_error = NULL;

  pending = g_task_get_task_data (task);
  cancellable = g_task_get_cancellable (task);

  /* Only open the output stream on demand to ensure we use as
   * few file descriptors as possible.
   */
  if (!pending->out_stream)
    {
      if (!pending->is_membuf)
        {
          if (!_ostree_fetcher_tmpf_from_flags (pending->flags, pending->thread_closure->base_tmpdir_dfd,
                                                &pending->tmpf, &local_error))
            goto out;
          pending->out_stream = g_unix_output_stream_new (pending->tmpf.fd, FALSE);
        }
      else
        {
          pending->out_stream = g_memory_output_stream_new_resizable ();
        }

      g_mutex_lock (&pending->thread_closure->output_stream_set_lock);
      g_hash_table_add (pending->thread_closure->output_stream_set,
                        g_object_ref (pending->out_stream));
      g_mutex_unlock (&pending->thread_closure->output_stream_set_lock);
    }

  /* Get a GBytes buffer */
  bytes = g_input_stream_read_bytes_finish ((GInputStream*)object, result, &local_error);
  if (!bytes)
    goto out;
  bytes_read = g_bytes_get_size (bytes);

  /* Was this the end of the stream? */
  if (bytes_read == 0)
    {
      if (!finish_stream (pending, cancellable, &local_error))
        goto out;
      if (pending->is_membuf)
        {
          g_task_return_pointer (task,
                                 g_memory_output_stream_steal_as_bytes ((GMemoryOutputStream*)pending->out_stream),
                                 (GDestroyNotify) g_bytes_unref);
        }
      else
        {
          if (lseek (pending->tmpf.fd, 0, SEEK_SET) < 0)
            {
              glnx_set_error_from_errno (&local_error);
              g_task_return_error (task, g_steal_pointer (&local_error));
            }
          else
            g_task_return_boolean (task, TRUE);
        }
      remove_pending (pending);
    }
  else
    {
      /* Verify max size */
      if (pending->max_size > 0)
        {
          if (bytes_read > pending->max_size ||
              (bytes_read + pending->current_size) > pending->max_size)
            {
              g_autofree char *uristr =
                soup_uri_to_string (soup_request_get_uri (pending->request), FALSE);
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
                                      cancellable,
                                      on_out_splice_complete,
                                      g_object_ref (task));
      }
    }

 out:
  if (local_error)
    {
      g_task_return_error (task, local_error);
      remove_pending (pending);
    }

  g_object_unref (task);
}

static void
on_request_sent (GObject        *object,
                 GAsyncResult   *result,
                 gpointer        user_data) 
{
  GTask *task = G_TASK (user_data);
  /* Hold a ref to the pending across this function, since we remove
   * it from the hash early in some cases, not in others. */
  OstreeFetcherPendingURI *pending = pending_uri_ref (g_task_get_task_data (task));
  GCancellable *cancellable = g_task_get_cancellable (task);
  GError *local_error = NULL;
  glnx_unref_object SoupMessage *msg = NULL;

  pending->state = OSTREE_FETCHER_STATE_COMPLETE;
  pending->request_body = soup_request_send_finish ((SoupRequest*) object,
                                                   result, &local_error);

  if (!pending->request_body)
    goto out;
  g_assert_no_error (local_error);

  if (SOUP_IS_REQUEST_HTTP (object))
    {
      msg = soup_request_http_get_message ((SoupRequestHTTP*) object);
      if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
        {
          /* is there another mirror we can try? */
          if (pending->mirrorlist_idx + 1 < pending->mirrorlist->len)
            {
              pending->mirrorlist_idx++;
              create_pending_soup_request (pending, &local_error);
              if (local_error != NULL)
                goto out;

              (void) g_input_stream_close (pending->request_body, NULL, NULL);

              start_pending_request (pending->thread_closure, task);
            }
          else
            {
              g_autofree char *uristring =
                soup_uri_to_string (soup_request_get_uri (pending->request), FALSE);

              GIOErrorEnum code;
              switch (msg->status_code)
                {
                case 404:
                case 403:
                case 410:
                  code = G_IO_ERROR_NOT_FOUND;
                  break;
                default:
                  code = G_IO_ERROR_FAILED;
                }

              {
                g_autofree char *errmsg =
                  g_strdup_printf ("Server returned status %u: %s",
                                   msg->status_code,
                                   soup_status_get_phrase (msg->status_code));

                /* Let's make OOB errors be the final one since they're probably
                 * the cause for the error here. */
                if (pending->thread_closure->oob_error)
                  {
                    local_error =
                      g_error_copy (pending->thread_closure->oob_error);
                    g_prefix_error (&local_error, "%s: ", errmsg);
                  }
                else
                  local_error = g_error_new_literal (G_IO_ERROR, code, errmsg);
              }

              if (pending->mirrorlist->len > 1)
                g_prefix_error (&local_error,
                                "All %u mirrors failed. Last error was: ",
                                pending->mirrorlist->len);
              if (pending->thread_closure->remote_name &&
                  !((pending->flags & OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT) > 0 &&
                    code == G_IO_ERROR_NOT_FOUND))
                _ostree_fetcher_journal_failure (pending->thread_closure->remote_name,
                                                 uristring, local_error->message);

            }
          goto out;
        }
    }

  pending->state = OSTREE_FETCHER_STATE_DOWNLOADING;
  
  pending->content_length = soup_request_get_content_length (pending->request);

  g_input_stream_read_bytes_async (pending->request_body,
                                   8192, G_PRIORITY_DEFAULT,
                                   cancellable,
                                   on_stream_read,
                                   g_object_ref (task));

 out:
  if (local_error)
    {
      if (pending->request_body)
        (void) g_input_stream_close (pending->request_body, NULL, NULL);
      g_task_return_error (task, local_error);
      remove_pending (pending);
    }

  pending_uri_unref (pending);
  g_object_unref (task);
}

static void
_ostree_fetcher_request_async (OstreeFetcher         *self,
                               GPtrArray             *mirrorlist,
                               const char            *filename,
                               OstreeFetcherRequestFlags flags,
                               gboolean               is_membuf,
                               guint64                max_size,
                               int                    priority,
                               GCancellable          *cancellable,
                               GAsyncReadyCallback    callback,
                               gpointer               user_data)
{
  g_autoptr(GTask) task = NULL;
  OstreeFetcherPendingURI *pending;

  g_return_if_fail (OSTREE_IS_FETCHER (self));
  g_return_if_fail (mirrorlist != NULL);
  g_return_if_fail (mirrorlist->len > 0);

  /* SoupRequest is created in session thread. */
  pending = g_new0 (OstreeFetcherPendingURI, 1);
  pending->ref_count = 1;
  pending->thread_closure = thread_closure_ref (self->thread_closure);
  pending->mirrorlist = g_ptr_array_ref (mirrorlist);
  pending->filename = g_strdup (filename);
  pending->flags = flags;
  pending->max_size = max_size;
  pending->is_membuf = is_membuf;

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, _ostree_fetcher_request_async);
  g_task_set_task_data (task, pending, (GDestroyNotify) pending_uri_unref);

  /* We'll use the GTask priority for our own priority queue. */
  g_task_set_priority (task, priority);

  session_thread_idle_add (self->thread_closure,
                           session_thread_request_uri,
                           g_object_ref (task),
                           (GDestroyNotify) g_object_unref);
}

void
_ostree_fetcher_request_to_tmpfile (OstreeFetcher         *self,
                                    GPtrArray             *mirrorlist,
                                    const char            *filename,
                                    OstreeFetcherRequestFlags flags,
                                    guint64                max_size,
                                    int                    priority,
                                    GCancellable          *cancellable,
                                    GAsyncReadyCallback    callback,
                                    gpointer               user_data)
{
  _ostree_fetcher_request_async (self, mirrorlist, filename, flags, FALSE,
                                 max_size, priority, cancellable,
                                 callback, user_data);
}

gboolean
_ostree_fetcher_request_to_tmpfile_finish (OstreeFetcher *self,
                                           GAsyncResult  *result,
                                           GLnxTmpfile   *out_tmpf,
                                           GError       **error)
{
  GTask *task;
  OstreeFetcherPendingURI *pending;
  gpointer ret;

  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, _ostree_fetcher_request_async), FALSE);

  task = (GTask*)result;
  pending = g_task_get_task_data (task);

  ret = g_task_propagate_pointer (task, error);
  if (!ret)
    return FALSE;

  g_assert (!pending->is_membuf);
  *out_tmpf = pending->tmpf;
  pending->tmpf.initialized = FALSE; /* Transfer ownership */

  return TRUE;
}

void
_ostree_fetcher_request_to_membuf (OstreeFetcher         *self,
                                   GPtrArray             *mirrorlist,
                                   const char            *filename,
                                   OstreeFetcherRequestFlags flags,
                                   guint64                max_size,
                                   int                    priority,
                                   GCancellable          *cancellable,
                                   GAsyncReadyCallback    callback,
                                   gpointer               user_data)
{
  _ostree_fetcher_request_async (self, mirrorlist, filename, flags, TRUE,
                                 max_size, priority, cancellable,
                                 callback, user_data);
}

gboolean
_ostree_fetcher_request_to_membuf_finish (OstreeFetcher *self,
                                          GAsyncResult  *result,
                                          GBytes       **out_buf,
                                          GError       **error)
{
  GTask *task;
  OstreeFetcherPendingURI *pending;
  gpointer ret;

  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, _ostree_fetcher_request_async), FALSE);

  task = (GTask*)result;
  pending = g_task_get_task_data (task);

  ret = g_task_propagate_pointer (task, error);
  if (!ret)
    return FALSE;

  g_assert (pending->is_membuf);
  g_assert (out_buf);
  *out_buf = ret;

  return TRUE;
}


guint64
_ostree_fetcher_bytes_transferred (OstreeFetcher       *self)
{
  g_return_val_if_fail (OSTREE_IS_FETCHER (self), 0);

  g_mutex_lock (&self->thread_closure->output_stream_set_lock);

  guint64 ret = self->thread_closure->total_downloaded;

  GLNX_HASH_TABLE_FOREACH (self->thread_closure->output_stream_set,
                           GFileOutputStream*, stream)
    {
      if (G_IS_FILE_DESCRIPTOR_BASED (stream))
        {
          int fd = g_file_descriptor_based_get_fd ((GFileDescriptorBased*)stream);
          struct stat stbuf;

          if (glnx_fstat (fd, &stbuf, NULL))
            ret += stbuf.st_size;
        }
    }

  g_mutex_unlock (&self->thread_closure->output_stream_set_lock);

  return ret;
}
