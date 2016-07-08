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

#include "libglnx.h"
#include "ostree-fetcher.h"
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
  GMainLoop *main_loop;

  int tmpdir_dfd;
  char *tmpdir_name;
  GLnxLockFile tmpdir_lock;
  int base_tmpdir_dfd;

  int max_outstanding;

  /* Queue for libsoup, see bgo#708591 */
  GQueue pending_queue;
  GHashTable *outstanding;

  /* Shared across threads; be sure to lock. */
  GHashTable *output_stream_set;  /* set<GOutputStream> */
  GMutex output_stream_set_lock;

  /* Also protected by output_stream_set_lock. */
  guint64 total_downloaded;
} ThreadClosure;

static void
session_thread_process_pending_queue (ThreadClosure *thread_closure);

typedef struct {
  volatile int ref_count;

  ThreadClosure *thread_closure;
  SoupURI *uri;

  OstreeFetcherState state;

  SoupRequest *request;

  gboolean is_stream;
  GInputStream *request_body;
  char *out_tmpfile;
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
  g_return_val_if_fail (thread_closure != NULL, NULL);
  g_return_val_if_fail (thread_closure->ref_count > 0, NULL);

  g_atomic_int_inc (&thread_closure->ref_count);

  return thread_closure;
}

static void
thread_closure_unref (ThreadClosure *thread_closure)
{
  g_return_if_fail (thread_closure != NULL);
  g_return_if_fail (thread_closure->ref_count > 0);

  if (g_atomic_int_dec_and_test (&thread_closure->ref_count))
    {
      /* The session thread should have cleared this by now. */
      g_assert (thread_closure->session == NULL);

      g_clear_pointer (&thread_closure->main_context, g_main_context_unref);
      g_clear_pointer (&thread_closure->main_loop, g_main_loop_unref);

      if (thread_closure->tmpdir_dfd != -1)
        close (thread_closure->tmpdir_dfd);

      /* Note: We don't remove the tmpdir here, because that would cause
         us to not reuse it on resume. This happens because we use two
         fetchers for each pull, so finalizing the first one would remove
         all the files to be resumed from the previous second one */

      g_free (thread_closure->tmpdir_name);
      glnx_release_lock_file (&thread_closure->tmpdir_lock);

      g_clear_pointer (&thread_closure->output_stream_set, g_hash_table_unref);
      g_mutex_clear (&thread_closure->output_stream_set_lock);

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

static int
pending_task_compare (gconstpointer a,
                      gconstpointer b,
                      gpointer unused)
{
  gint priority_a = g_task_get_priority (G_TASK (a));
  gint priority_b = g_task_get_priority (G_TASK (b));

  return (priority_a == priority_b) ? 0 :
         (priority_a < priority_b) ? -1 : 1;
}

static OstreeFetcherPendingURI *
pending_uri_ref (OstreeFetcherPendingURI *pending)
{
  g_return_val_if_fail (pending != NULL, NULL);
  g_return_val_if_fail (pending->ref_count > 0, NULL);

  g_atomic_int_inc (&pending->ref_count);

  return pending;
}

static void
pending_uri_unref (OstreeFetcherPendingURI *pending)
{
  if (!g_atomic_int_dec_and_test (&pending->ref_count))
    return;

  g_clear_pointer (&pending->thread_closure, thread_closure_unref);

  soup_uri_free (pending->uri);
  g_clear_object (&pending->request);
  g_clear_object (&pending->request_body);
  g_free (pending->out_tmpfile);
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
session_thread_set_proxy_cb (ThreadClosure *thread_closure,
                             gpointer data)
{
  SoupURI *proxy_uri = data;

  g_object_set (thread_closure->session,
                SOUP_SESSION_PROXY_URI,
                proxy_uri, NULL);
}

#ifdef HAVE_LIBSOUP_CLIENT_CERTS
static void
session_thread_set_tls_interaction_cb (ThreadClosure *thread_closure,
                                       gpointer data)
{
  GTlsCertificate *cert = data;
  glnx_unref_object OstreeTlsCertInteraction *interaction = NULL;

  /* The GTlsInteraction instance must be created in the
   * session thread so it uses the correct GMainContext. */
  interaction = _ostree_tls_cert_interaction_new (cert);

  g_object_set (thread_closure->session,
                SOUP_SESSION_TLS_INTERACTION,
                interaction, NULL);
}
#endif

static void
session_thread_set_tls_database_cb (ThreadClosure *thread_closure,
                                    gpointer data)
{
  GTlsDatabase *database = data;

  if (database != NULL)
    {
      g_object_set (thread_closure->session,
                    SOUP_SESSION_TLS_DATABASE,
                    database, NULL);
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
session_thread_process_pending_queue (ThreadClosure *thread_closure)
{

  while (g_queue_peek_head (&thread_closure->pending_queue) != NULL &&
         g_hash_table_size (thread_closure->outstanding) < thread_closure->max_outstanding)
    {
      GTask *task;
      OstreeFetcherPendingURI *pending;
      GCancellable *cancellable;

      task = g_queue_pop_head (&thread_closure->pending_queue);

      pending = g_task_get_task_data (task);
      cancellable = g_task_get_cancellable (task);

      g_hash_table_add (thread_closure->outstanding, pending_uri_ref (pending));

      soup_request_send_async (pending->request,
                               cancellable,
                               on_request_sent,
                               g_object_ref (task));

      g_object_unref (task);
    }
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

  pending->request = soup_session_request_uri (thread_closure->session,
                                               pending->uri,
                                               &local_error);

  if (local_error != NULL)
    {
      g_task_return_error (task, local_error);
      return;
    }

  if (pending->is_stream)
    {
      soup_request_send_async (pending->request,
                               cancellable,
                               on_request_sent,
                               g_object_ref (task));
    }
  else
    {
      g_autofree char *uristring = soup_uri_to_string (pending->uri, FALSE);
      g_autofree char *tmpfile = NULL;
      struct stat stbuf;
      gboolean exists;

      /* The tmp directory is lazily created for each fetcher instance,
       * since it may require superuser permissions and some instances
       * only need _ostree_fetcher_request_uri_to_membuf() which keeps
       * everything in memory buffers. */
      if (thread_closure->tmpdir_name == NULL)
        {
          if (!_ostree_repo_allocate_tmpdir (thread_closure->base_tmpdir_dfd,
                                             OSTREE_REPO_TMPDIR_FETCHER,
                                             &thread_closure->tmpdir_name,
                                             &thread_closure->tmpdir_dfd,
                                             &thread_closure->tmpdir_lock,
                                             NULL,
                                             cancellable,
                                             &local_error))
            {
              g_task_return_error (task, local_error);
              return;
            }
        }

      tmpfile = g_compute_checksum_for_string (G_CHECKSUM_SHA256, uristring, strlen (uristring));

      if (fstatat (thread_closure->tmpdir_dfd, tmpfile, &stbuf, AT_SYMLINK_NOFOLLOW) == 0)
        exists = TRUE;
      else
        {
          if (errno == ENOENT)
            exists = FALSE;
          else
            {
              glnx_set_error_from_errno (&local_error);
              g_task_return_error (task, local_error);
              return;
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

      g_queue_insert_sorted (&thread_closure->pending_queue,
                             g_object_ref (task),
                             pending_task_compare, NULL);
      session_thread_process_pending_queue (thread_closure);
    }
}

static gpointer
ostree_fetcher_session_thread (gpointer data)
{
  ThreadClosure *closure = data;
  g_autoptr(GMainContext) mainctx = g_main_context_ref (closure->main_context);
  gint max_conns;

  /* This becomes the GMainContext that SoupSession schedules async
   * callbacks and emits signals from.  Make it the thread-default
   * context for this thread before creating the session. */
  g_main_context_push_thread_default (mainctx);

  /* We retain ownership of the SoupSession reference. */
  closure->session = soup_session_async_new_with_options (SOUP_SESSION_USER_AGENT, "ostree ",
                                                          SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
                                                          SOUP_SESSION_USE_THREAD_CONTEXT, TRUE,
                                                          SOUP_SESSION_ADD_FEATURE_BY_TYPE, SOUP_TYPE_REQUESTER,
                                                          SOUP_SESSION_TIMEOUT, 60,
                                                          SOUP_SESSION_IDLE_TIMEOUT, 60,
                                                          NULL);

  g_object_get (closure->session, "max-conns-per-host", &max_conns, NULL);
  if (max_conns < 8)
    {
      /* We download a lot of small objects in ostree, so this
       * helps a lot.  Also matches what most modern browsers do. */
      max_conns = 8;
      g_object_set (closure->session,
                    "max-conns-per-host",
                    max_conns, NULL);
    }
  closure->max_outstanding = 3 * max_conns;

  g_main_loop_run (closure->main_loop);

  /* Since the ThreadClosure may be finalized from any thread we
   * unreference all data related to the SoupSession ourself to ensure
   * it's freed in the same thread where it was created. */
  g_clear_pointer (&closure->outstanding, g_hash_table_unref);
  while (!g_queue_is_empty (&closure->pending_queue))
    g_object_unref (g_queue_pop_head (&closure->pending_queue));
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
  g_main_loop_quit (self->thread_closure->main_loop);
  g_clear_pointer (&self->session_thread, g_thread_unref);
  g_clear_pointer (&self->thread_closure, thread_closure_unref);

  G_OBJECT_CLASS (_ostree_fetcher_parent_class)->finalize (object);
}

static void
_ostree_fetcher_constructed (GObject *object)
{
  OstreeFetcher *self = OSTREE_FETCHER (object);
  g_autoptr(GMainContext) main_context = NULL;
  GLnxLockFile empty_lockfile = GLNX_LOCK_FILE_INIT;
  const char *http_proxy;

  main_context = g_main_context_new ();

  self->thread_closure = g_slice_new0 (ThreadClosure);
  self->thread_closure->ref_count = 1;
  self->thread_closure->main_context = g_main_context_ref (main_context);
  self->thread_closure->main_loop = g_main_loop_new (main_context, FALSE);
  self->thread_closure->tmpdir_dfd = -1;
  self->thread_closure->tmpdir_lock = empty_lockfile;

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
                     OstreeFetcherConfigFlags flags)
{
  OstreeFetcher *self;

  self = g_object_new (OSTREE_TYPE_FETCHER, "config-flags", flags, NULL);

  self->thread_closure->base_tmpdir_dfd = tmpdir_dfd;

  return self;
}

int
_ostree_fetcher_get_dfd (OstreeFetcher *fetcher)
{
  return fetcher->thread_closure->tmpdir_dfd;
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
_ostree_fetcher_set_client_cert (OstreeFetcher   *self,
                                 GTlsCertificate *cert)
{
  g_return_if_fail (OSTREE_IS_FETCHER (self));
  g_return_if_fail (G_IS_TLS_CERTIFICATE (cert));

#ifdef HAVE_LIBSOUP_CLIENT_CERTS
  session_thread_idle_add (self->thread_closure,
                           session_thread_set_tls_interaction_cb,
                           g_object_ref (cert),
                           (GDestroyNotify) g_object_unref);
#else
  g_warning ("This version of OSTree is compiled without client side certificate support");
#endif
}

void
_ostree_fetcher_set_tls_database (OstreeFetcher *self,
                                  GTlsDatabase  *db)
{
  g_return_if_fail (OSTREE_IS_FETCHER (self));
  g_return_if_fail (db == NULL || G_IS_TLS_DATABASE (db));

  if (db != NULL)
    {
      session_thread_idle_add (self->thread_closure,
                               session_thread_set_tls_database_cb,
                               g_object_ref (db),
                               (GDestroyNotify) g_object_unref);
    }
  else
    {
      session_thread_idle_add (self->thread_closure,
                               session_thread_set_tls_database_cb,
                               NULL, (GDestroyNotify) NULL);
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
      if (!g_output_stream_close (pending->out_stream, cancellable, error))
        goto out;

      g_mutex_lock (&pending->thread_closure->output_stream_set_lock);
      g_hash_table_remove (pending->thread_closure->output_stream_set,
                           pending->out_stream);
      g_mutex_unlock (&pending->thread_closure->output_stream_set_lock);
    }

  pending->state = OSTREE_FETCHER_STATE_COMPLETE;
  if (fstatat (pending->thread_closure->tmpdir_dfd,
               pending->out_tmpfile,
               &stbuf, AT_SYMLINK_NOFOLLOW) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  /* Now that we've finished downloading, continue with other queued
   * requests.
   */
  session_thread_process_pending_queue (pending->thread_closure);

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
remove_pending_rerun_queue (OstreeFetcherPendingURI *pending)
{
  /* Hold a temporary ref to ensure the reference to
   * pending->thread_closure is valid.
   */
  pending_uri_ref (pending);
  g_hash_table_remove (pending->thread_closure->outstanding, pending);
  session_thread_process_pending_queue (pending->thread_closure);
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
      remove_pending_rerun_queue (pending);
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

  bytes = g_input_stream_read_bytes_finish ((GInputStream*)object, result, &local_error);
  if (!bytes)
    goto out;

  bytes_read = g_bytes_get_size (bytes);
  if (bytes_read == 0)
    {
      if (!finish_stream (pending, cancellable, &local_error))
        goto out;
      g_task_return_pointer (task,
                             g_strdup (pending->out_tmpfile),
                             (GDestroyNotify) g_free);
      remove_pending_rerun_queue (pending);
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
                                      cancellable,
                                      on_out_splice_complete,
                                      g_object_ref (task));
      }
    }

 out:
  if (local_error)
    {
      g_task_return_error (task, local_error);
      remove_pending_rerun_queue (pending);
    }

  g_object_unref (task);
}

static void
on_request_sent (GObject        *object,
                 GAsyncResult   *result,
                 gpointer        user_data) 
{
  GTask *task = G_TASK (user_data);
  OstreeFetcherPendingURI *pending;
  GCancellable *cancellable;
  GError *local_error = NULL;
  glnx_unref_object SoupMessage *msg = NULL;

  pending = g_task_get_task_data (task);
  cancellable = g_task_get_cancellable (task);

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
              g_task_return_pointer (task,
                                     g_object_ref (pending->request_body),
                                     (GDestroyNotify) g_object_unref);
            }
          else
            {
              g_task_return_pointer (task,
                                     g_strdup (pending->out_tmpfile),
                                     (GDestroyNotify) g_free);
            }
          remove_pending_rerun_queue (pending);
          goto out;
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

      fd = openat (pending->thread_closure->tmpdir_dfd,
                   pending->out_tmpfile, oflags, 0666);
      if (fd == -1)
        {
          glnx_set_error_from_errno (&local_error);
          goto out;
        }
      pending->out_stream = g_unix_output_stream_new (fd, TRUE);

      g_mutex_lock (&pending->thread_closure->output_stream_set_lock);
      g_hash_table_add (pending->thread_closure->output_stream_set,
                        g_object_ref (pending->out_stream));
      g_mutex_unlock (&pending->thread_closure->output_stream_set_lock);

      g_input_stream_read_bytes_async (pending->request_body,
                                       8192, G_PRIORITY_DEFAULT,
                                       cancellable,
                                       on_stream_read,
                                       g_object_ref (task));
    }
  else
    {
      g_task_return_pointer (task,
                             g_object_ref (pending->request_body),
                             (GDestroyNotify) g_object_unref);
      remove_pending_rerun_queue (pending);
    }
  
 out:
  if (local_error)
    {
      if (pending->request_body)
        (void) g_input_stream_close (pending->request_body, NULL, NULL);
      g_task_return_error (task, local_error);
      remove_pending_rerun_queue (pending);
    }

  g_object_unref (task);
}

static void
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
  g_autoptr(GTask) task = NULL;
  OstreeFetcherPendingURI *pending;

  g_return_if_fail (OSTREE_IS_FETCHER (self));
  g_return_if_fail (uri != NULL);

  /* SoupRequest is created in session thread. */
  pending = g_new0 (OstreeFetcherPendingURI, 1);
  pending->ref_count = 1;
  pending->thread_closure = thread_closure_ref (self->thread_closure);
  pending->uri = soup_uri_copy (uri);
  pending->max_size = max_size;
  pending->is_stream = is_stream;

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, source_tag);
  g_task_set_task_data (task, pending, (GDestroyNotify) pending_uri_unref);

  /* We'll use the GTask priority for our own priority queue. */
  g_task_set_priority (task, priority);

  session_thread_idle_add (self->thread_closure,
                           session_thread_request_uri,
                           g_object_ref (task),
                           (GDestroyNotify) g_object_unref);
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
  GHashTableIter hiter;
  gpointer key, value;
  guint64 ret;

  g_return_val_if_fail (OSTREE_IS_FETCHER (self), 0);

  g_mutex_lock (&self->thread_closure->output_stream_set_lock);

  ret = self->thread_closure->total_downloaded;

  g_hash_table_iter_init (&hiter, self->thread_closure->output_stream_set);
  while (g_hash_table_iter_next (&hiter, &key, &value))
    {
      GFileOutputStream *stream = key;
      struct stat stbuf;
      
      if (G_IS_FILE_DESCRIPTOR_BASED (stream))
        {
          if (glnx_stream_fstat ((GFileDescriptorBased*)stream, &stbuf, NULL))
            ret += stbuf.st_size;
        }
    }

  g_mutex_unlock (&self->thread_closure->output_stream_set_lock);

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

  mainctx = g_main_context_new ();
  g_main_context_push_thread_default (mainctx);

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
  if (mainctx)
    g_main_context_pop_thread_default (mainctx);
  g_clear_object (&(data.result_stream));
  return ret;
}
