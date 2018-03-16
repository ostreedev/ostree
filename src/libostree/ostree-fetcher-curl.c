/*
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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

#include "config.h"

#include <gio/gfiledescriptorbased.h>
#include <gio/gunixoutputstream.h>
#include <glib-unix.h>
#include <curl/curl.h>

/* These macros came from 7.43.0, but we want to check
 * for versions a bit earlier than that (to work on CentOS 7),
 * so define them here if we're using an older version.
 */
#ifndef CURL_VERSION_BITS
#define CURL_VERSION_BITS(x,y,z) ((x)<<16|(y)<<8|z)
#endif
#ifndef CURL_AT_LEAST_VERSION
#define CURL_AT_LEAST_VERSION(x,y,z) (LIBCURL_VERSION_NUM >= CURL_VERSION_BITS(x, y, z))
#endif

/* Cargo culted from https://github.com/curl/curl/blob/curl-7_53_0/docs/examples/http2-download.c */
#ifndef CURLPIPE_MULTIPLEX
/* This little trick will just make sure that we don't enable pipelining for
   libcurls old enough to not have this symbol. It is _not_ defined to zero in
   a recent libcurl header. */
#define CURLPIPE_MULTIPLEX 0
#endif

#include "ostree-fetcher.h"
#include "ostree-fetcher-util.h"
#include "ostree-enumtypes.h"
#include "ostree-repo-private.h"
#include "otutil.h"

#include "ostree-soup-uri.h"

typedef struct FetcherRequest FetcherRequest;
typedef struct SockInfo SockInfo;

static int sock_cb (CURL *e, curl_socket_t s, int what, void *cbp, void *sockp);
static gboolean timer_cb (gpointer data);
static void sock_unref (SockInfo *f);
static int update_timeout_cb (CURLM *multi, long timeout_ms, void *userp);
static void request_unref (FetcherRequest *req);
static void initiate_next_curl_request (FetcherRequest *req, GTask *task);
static void destroy_and_unref_source (GSource *source);

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
  char *custom_user_agent;

  GMainContext *mainctx;
  CURLM *multi;
  GSource *timer_event;
  int curl_running;
  GHashTable *outstanding_requests; /* Set<GTask> */
  GHashTable *sockets; /* Set<SockInfo> */

  guint64 bytes_transferred;
};

/* Information associated with a request */
struct FetcherRequest {
  guint refcount;
  GPtrArray *mirrorlist;
  guint idx;

  char *filename;
  guint64 current_size;
  guint64 max_size;
  OstreeFetcherRequestFlags flags;
  gboolean is_membuf;
  GError *caught_write_error;
  GLnxTmpfile tmpf;
  GString *output_buf;

  CURL *easy;
  char error[CURL_ERROR_SIZE];

  OstreeFetcher *fetcher;
};

/* Information associated with a specific socket */
struct SockInfo {
  guint refcount;
  curl_socket_t sockfd;
  int action;
  long timeout;
  GSource *ch;
  OstreeFetcher *fetcher;
};

enum {
  PROP_0,
  PROP_CONFIG_FLAGS
};

G_DEFINE_TYPE (OstreeFetcher, _ostree_fetcher, G_TYPE_OBJECT)

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

  curl_multi_cleanup (self->multi);
  g_free (self->remote_name);
  g_free (self->cookie_jar_path);
  g_free (self->proxy);
  g_assert_cmpint (g_hash_table_size (self->outstanding_requests), ==, 0);
  g_clear_pointer (&self->extra_headers, (GDestroyNotify)curl_slist_free_all);
  g_hash_table_unref (self->outstanding_requests);
  g_hash_table_unref (self->sockets);
  g_clear_pointer (&self->timer_event, (GDestroyNotify)destroy_and_unref_source);
  if (self->mainctx)
    g_main_context_unref (self->mainctx);
  g_clear_pointer (&self->custom_user_agent, (GDestroyNotify)g_free);

  G_OBJECT_CLASS (_ostree_fetcher_parent_class)->finalize (object);
}

static void
_ostree_fetcher_constructed (GObject *object)
{
  // OstreeFetcher *self = OSTREE_FETCHER (object);

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
  self->multi = curl_multi_init();
  self->outstanding_requests = g_hash_table_new_full (NULL, NULL, (GDestroyNotify)g_object_unref, NULL);
  self->sockets = g_hash_table_new_full (NULL, NULL, (GDestroyNotify)sock_unref, NULL);
  curl_multi_setopt (self->multi, CURLMOPT_SOCKETFUNCTION, sock_cb);
  curl_multi_setopt (self->multi, CURLMOPT_SOCKETDATA, self);
  curl_multi_setopt (self->multi, CURLMOPT_TIMERFUNCTION, update_timeout_cb);
  curl_multi_setopt (self->multi, CURLMOPT_TIMERDATA, self);
#if CURL_AT_LEAST_VERSION(7, 30, 0)
  /* Let's do something reasonable here. */
  curl_multi_setopt (self->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, 8);
#endif
  /* This version mirrors the version at which we're enabling HTTP2 support.
   * See also https://github.com/curl/curl/blob/curl-7_53_0/docs/examples/http2-download.c
   */
#if CURL_AT_LEAST_VERSION(7, 51, 0)
  curl_multi_setopt (self->multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
#endif
}


OstreeFetcher *
_ostree_fetcher_new (int                      tmpdir_dfd,
                     const char              *remote_name,
                     OstreeFetcherConfigFlags flags)
{
  OstreeFetcher *fetcher = g_object_new (OSTREE_TYPE_FETCHER, "config-flags", flags, NULL);
  fetcher->remote_name = g_strdup (remote_name);
  fetcher->tmpdir_dfd = tmpdir_dfd;
  return fetcher;
}

static void
destroy_and_unref_source (GSource *source)
{
  g_source_destroy (source);
  g_source_unref (source);
}

static char *
request_get_uri (FetcherRequest *req, guint idx)
{
  SoupURI *baseuri = req->mirrorlist->pdata[idx];
  if (!req->filename)
    return soup_uri_to_string (baseuri, FALSE);
  { g_autofree char *uristr = soup_uri_to_string (baseuri, FALSE);
    return g_build_filename (uristr, req->filename, NULL);
  }
}

static gboolean
ensure_tmpfile (FetcherRequest *req, GError **error)
{
  if (!req->tmpf.initialized)
    {
      if (!_ostree_fetcher_tmpf_from_flags (req->flags, req->fetcher->tmpdir_dfd,
                                            &req->tmpf, error))
        return FALSE;
    }
  return TRUE;
}

/* Check for completed transfers, and remove their easy handles */
static void
check_multi_info (OstreeFetcher *fetcher)
{
  CURLMsg *msg;
  int msgs_left;

  while ((msg = curl_multi_info_read (fetcher->multi, &msgs_left)) != NULL)
    {
      long response;
      CURL *easy = msg->easy_handle;
      CURLcode curlres = msg->data.result;
      GTask *task;
      FetcherRequest *req;
      const char *eff_url;
      gboolean is_file;
      gboolean continued_request = FALSE;

      if (msg->msg != CURLMSG_DONE)
        continue;

      curl_easy_getinfo (easy, CURLINFO_PRIVATE, &task);
      curl_easy_getinfo (easy, CURLINFO_EFFECTIVE_URL, &eff_url);
      /* We should have limited the protocols; this is what
       * curl's tool_operate.c does.
       */
      is_file = g_str_has_prefix (eff_url, "file:");
      g_assert (is_file || g_str_has_prefix (eff_url, "http"));

      req = g_task_get_task_data (task);

      if (req->caught_write_error)
        g_task_return_error (task, g_steal_pointer (&req->caught_write_error));
      else if (curlres != CURLE_OK)
        {
          if (is_file && curlres == CURLE_FILE_COULDNT_READ_FILE)
            {
              /* Handle file not found */
              g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                       "%s",
                                         curl_easy_strerror (curlres));
            }
          else
            {
              g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "[%u] %s",
                                       curlres,
                                       curl_easy_strerror (curlres));
              if (req->fetcher->remote_name)
                _ostree_fetcher_journal_failure (req->fetcher->remote_name,
                                                 eff_url, curl_easy_strerror (curlres));
            }
        }
      else
        {
          curl_easy_getinfo (easy, CURLINFO_RESPONSE_CODE, &response);
          if (!is_file && !(response >= 200 && response < 300))
            {
              GIOErrorEnum giocode;

              /* TODO - share with soup */
              switch (response)
                {
                case 404:
                case 403:
                case 410:
                  giocode = G_IO_ERROR_NOT_FOUND;
                  break;
                default:
                  giocode = G_IO_ERROR_FAILED;
                }

              if (req->idx + 1 == req->mirrorlist->len)
                {
                  g_autofree char *msg = g_strdup_printf ("Server returned HTTP %lu", response);
                  g_task_return_new_error (task, G_IO_ERROR, giocode,
                                           "%s", msg);
                  if (req->fetcher->remote_name &&
                      !((req->flags & OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT) > 0 &&
                        giocode == G_IO_ERROR_NOT_FOUND))
                    _ostree_fetcher_journal_failure (req->fetcher->remote_name,
                                                     eff_url, msg);

                }
              else
                {
                  continued_request = TRUE;
                }
            }
          else if (req->is_membuf)
            {
              GBytes *ret;
              if ((req->flags & OSTREE_FETCHER_REQUEST_NUL_TERMINATION) > 0)
                g_string_append_c (req->output_buf, '\0');
              ret = g_string_free_to_bytes (req->output_buf);
              req->output_buf = NULL;
              g_task_return_pointer (task, ret, (GDestroyNotify)g_bytes_unref);
            }
          else
            {
              g_autoptr(GError) local_error = NULL;
              GError **error = &local_error;

              if (!ensure_tmpfile (req, error))
                {
                  g_task_return_error (task, g_steal_pointer (&local_error));
                }
              else if (lseek (req->tmpf.fd, 0, SEEK_SET) < 0)
                {
                  glnx_set_error_from_errno (error);
                  g_task_return_error (task, g_steal_pointer (&local_error));
                }
              else
                {
                  /* We return the tmpfile in the _finish wrapper */
                  g_task_return_boolean (task, TRUE);
                }
            }
        }

      curl_multi_remove_handle (fetcher->multi, easy);
      if (continued_request)
        {
          req->idx++;
          initiate_next_curl_request (req, task);
        }
      else
        {
          g_hash_table_remove (fetcher->outstanding_requests, task);
          if (g_hash_table_size (fetcher->outstanding_requests) == 0)
            {
              g_clear_pointer (&fetcher->mainctx, g_main_context_unref);
            }
        }
    }
}

/* Called by glib when our timeout expires */
static gboolean
timer_cb (gpointer data)
{
  OstreeFetcher *fetcher = data;
  GSource *orig_src = fetcher->timer_event;

  (void)curl_multi_socket_action (fetcher->multi, CURL_SOCKET_TIMEOUT, 0, &fetcher->curl_running);
  check_multi_info (fetcher);
  if (fetcher->timer_event == orig_src)
    fetcher->timer_event = NULL;

  return FALSE;
}

/* Update the event timer after curl_multi library calls */
static int
update_timeout_cb (CURLM *multi, long timeout_ms, void *userp)
{
  OstreeFetcher *fetcher = userp;

  g_clear_pointer (&fetcher->timer_event, (GDestroyNotify)destroy_and_unref_source);

  if (timeout_ms != -1)
    {
      fetcher->timer_event = g_timeout_source_new (timeout_ms);
      g_source_set_callback (fetcher->timer_event, timer_cb, fetcher, NULL);
      g_source_attach (fetcher->timer_event, fetcher->mainctx);
    }

  return 0;
}

/* Called by glib when we get action on a multi socket */
static gboolean
event_cb (int fd, GIOCondition condition, gpointer data)
{
  OstreeFetcher *fetcher = data;

  int action =
    (condition & G_IO_IN ? CURL_CSELECT_IN : 0) |
    (condition & G_IO_OUT ? CURL_CSELECT_OUT : 0);

  (void)curl_multi_socket_action (fetcher->multi, fd, action, &fetcher->curl_running);
  check_multi_info (fetcher);
  if (fetcher->curl_running > 0)
    {
      return TRUE;
    }
  else
    {
      return FALSE;
    }
}

/* Clean up the SockInfo structure */
static void
sock_unref (SockInfo *f)
{
  if (!f)
    return;
  if (--f->refcount != 0)
    return;
  g_clear_pointer (&f->ch, (GDestroyNotify)destroy_and_unref_source);
  g_free (f);
}

/* Assign information to a SockInfo structure */
static void
setsock (SockInfo*f, curl_socket_t s, int act, OstreeFetcher *fetcher)
{
  GIOCondition kind =
     (act&CURL_POLL_IN?G_IO_IN:0)|(act&CURL_POLL_OUT?G_IO_OUT:0);

  f->sockfd = s;
  f->action = act;
  g_clear_pointer (&f->ch, (GDestroyNotify)destroy_and_unref_source);
  /* TODO - investigate new g_source_modify_unix_fd() so changing the poll
   * flags involves less allocation.
   */
  f->ch = g_unix_fd_source_new (f->sockfd, kind);
  g_source_set_callback (f->ch, (GSourceFunc) event_cb, fetcher, NULL);
  g_source_attach (f->ch, fetcher->mainctx);
}

/* Initialize a new SockInfo structure */
static void
addsock (curl_socket_t s, CURL *easy, int action, OstreeFetcher *fetcher)
{
  SockInfo *fdp = g_new0 (SockInfo, 1);

  fdp->refcount = 1;
  fdp->fetcher = fetcher;
  setsock (fdp, s, action, fetcher);
  curl_multi_assign (fetcher->multi, s, fdp);
  g_hash_table_add (fetcher->sockets, fdp);
}

/* CURLMOPT_SOCKETFUNCTION */
static int
sock_cb (CURL *easy, curl_socket_t s, int what, void *cbp, void *sockp)
{
  OstreeFetcher *fetcher = cbp;
  SockInfo *fdp = (SockInfo*) sockp;

  if (what == CURL_POLL_REMOVE)
    {
      if (!g_hash_table_remove (fetcher->sockets, fdp))
        g_assert_not_reached ();
    }
  else
    {
      if (!fdp)
        {
          addsock (s, easy, what, fetcher);
        }
      else
        {
          setsock (fdp, s, what, fetcher);
        }
    }
  return 0;
}

/* CURLOPT_WRITEFUNCTION */
static size_t
write_cb (void *ptr, size_t size, size_t nmemb, void *data)
{
  const size_t realsize = size * nmemb;
  GTask *task = data;
  FetcherRequest *req;

  req = g_task_get_task_data (task);

  if (req->caught_write_error)
    return -1;

  if (req->max_size > 0)
    {
      if (realsize > req->max_size ||
          (realsize + req->current_size) > req->max_size)
        {
          const char *eff_url;
          curl_easy_getinfo (req->easy, CURLINFO_EFFECTIVE_URL, &eff_url);
          req->caught_write_error =  g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                  "URI %s exceeded maximum size of %" G_GUINT64_FORMAT " bytes",
                                                  eff_url, req->max_size);
          return -1;
        }
    }

  if (req->is_membuf)
    g_string_append_len (req->output_buf, ptr, realsize);
  else
    {
      if (!ensure_tmpfile (req, &req->caught_write_error))
        return -1;
      g_assert (req->tmpf.fd >= 0);
      if (glnx_loop_write (req->tmpf.fd, ptr, realsize) < 0)
        {
          glnx_set_error_from_errno (&req->caught_write_error);
          return -1;
        }
    }

  req->current_size += realsize;
  req->fetcher->bytes_transferred += realsize;

  return realsize;
}

/* CURLOPT_PROGRESSFUNCTION */
static int
prog_cb (void *p, double dltotal, double dlnow, double ult, double uln)
{
  GTask *task = p;
  FetcherRequest *req;
  char *eff_url;
  req = g_task_get_task_data (task);
  curl_easy_getinfo (req->easy, CURLINFO_EFFECTIVE_URL, &eff_url);
  g_printerr ("Progress: %s (%g/%g)\n", eff_url, dlnow, dltotal);
  return 0;
}

static void
request_unref (FetcherRequest *req)
{
  if (--req->refcount != 0)
    return;

  g_ptr_array_unref (req->mirrorlist);
  g_free (req->filename);
  g_clear_error (&req->caught_write_error);
  glnx_tmpfile_clear (&req->tmpf);
  if (req->output_buf)
    g_string_free (req->output_buf, TRUE);
  curl_easy_cleanup (req->easy);

  g_free (req);
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
  g_free (self->proxy);
  self->proxy = g_strdup (http_proxy);
}

void
_ostree_fetcher_set_cookie_jar (OstreeFetcher *self,
                                const char    *jar_path)
{
  g_free (self->cookie_jar_path);
  self->cookie_jar_path = g_strdup (jar_path);
}

void
_ostree_fetcher_set_client_cert (OstreeFetcher   *self,
                                 const char      *cert_path,
                                 const char      *key_path)
{
  g_assert ((cert_path == NULL && key_path == NULL)
            || (cert_path != NULL && key_path != NULL));
  g_free (self->tls_client_cert_path);
  self->tls_client_cert_path = g_strdup (cert_path);
  g_free (self->tls_client_key_path);
  self->tls_client_key_path = g_strdup (key_path);
}

void
_ostree_fetcher_set_tls_database (OstreeFetcher *self,
                                  const char    *dbpath)
{
  g_free (self->tls_ca_db_path);
  self->tls_ca_db_path = g_strdup (dbpath);
}

void
_ostree_fetcher_set_extra_headers (OstreeFetcher *self,
                                   GVariant      *extra_headers)
{
  GVariantIter viter;
  const char *key;
  const char *value;

  g_clear_pointer (&self->extra_headers, (GDestroyNotify)curl_slist_free_all);

  g_variant_iter_init (&viter, extra_headers);
  while (g_variant_iter_loop (&viter, "(&s&s)", &key, &value))
    {
      g_autofree char *header = g_strdup_printf ("%s: %s", key, value);
      self->extra_headers = curl_slist_append (self->extra_headers, header);
    }
}

void
_ostree_fetcher_set_extra_user_agent (OstreeFetcher *self,
                                      const char    *extra_user_agent)
{
  g_clear_pointer (&self->custom_user_agent, (GDestroyNotify)g_free);
  if (extra_user_agent)
    {
      self->custom_user_agent =
        g_strdup_printf ("%s %s", OSTREE_FETCHER_USERAGENT_STRING, extra_user_agent);
    }
}

/* Re-bind all of the outstanding curl items to our new main context */
static void
adopt_steal_mainctx (OstreeFetcher *self,
                     GMainContext *mainctx)
{
  g_assert (self->mainctx == NULL);
  self->mainctx = mainctx; /* Transfer */

  if (self->timer_event != NULL)
    {
      guint64 readytime = g_source_get_ready_time (self->timer_event);
      guint64 curtime = g_source_get_time (self->timer_event);
      guint64 timeout_micros = curtime - readytime;
      if (curtime < readytime)
        timeout_micros = 0;
      update_timeout_cb (self->multi, timeout_micros / 1000, self);
    }

  GLNX_HASH_TABLE_FOREACH (self->sockets, SockInfo*, fdp)
    setsock (fdp, fdp->sockfd, fdp->action, self);
}

static void
initiate_next_curl_request (FetcherRequest *req,
                            GTask *task)
{
  CURLcode rc;
  OstreeFetcher *self = req->fetcher;

  if (req->easy)
    curl_easy_cleanup (req->easy);
  req->easy = curl_easy_init ();
  g_assert (req->easy);

  g_assert_cmpint (req->idx, <, req->mirrorlist->len);

  { g_autofree char *uri = request_get_uri (req, req->idx);
    curl_easy_setopt (req->easy, CURLOPT_URL, uri);
  }

  curl_easy_setopt (req->easy, CURLOPT_USERAGENT,
                    self->custom_user_agent ?: OSTREE_FETCHER_USERAGENT_STRING);
  if (self->extra_headers)
    curl_easy_setopt (req->easy, CURLOPT_HTTPHEADER, self->extra_headers);

  if (self->cookie_jar_path)
    {
      rc = curl_easy_setopt (req->easy, CURLOPT_COOKIEFILE, self->cookie_jar_path);
      g_assert_cmpint (rc, ==, CURLM_OK);
      rc = curl_easy_setopt (req->easy, CURLOPT_COOKIELIST, "RELOAD");
      g_assert_cmpint (rc, ==, CURLM_OK);
    }

  if (self->proxy)
    {
      rc = curl_easy_setopt (req->easy, CURLOPT_PROXY, self->proxy);
      g_assert_cmpint (rc, ==, CURLM_OK);
    }

  if (self->tls_ca_db_path)
    curl_easy_setopt (req->easy, CURLOPT_CAINFO, self->tls_ca_db_path);

  if ((self->config_flags & OSTREE_FETCHER_FLAGS_TLS_PERMISSIVE) > 0)
    curl_easy_setopt (req->easy, CURLOPT_SSL_VERIFYPEER, 0L);

  if (self->tls_client_cert_path)
    {
      /* Support for pkcs11:
       * https://github.com/ostreedev/ostree/pull/1183
       * This will be used by https://github.com/advancedtelematic/aktualizr
       * at least to fetch certificates.  No test coverage at the moment
       * though. See https://gitlab.com/gnutls/gnutls/tree/master/tests/pkcs11
       * and https://github.com/opendnssec/SoftHSMv2 and
       * https://github.com/p11-glue/p11-kit/tree/master/p11-kit for
       * possible ideas there.
       */
      if (g_str_has_prefix (self->tls_client_key_path, "pkcs11:"))
        {
          curl_easy_setopt (req->easy, CURLOPT_SSLENGINE, "pkcs11");
          curl_easy_setopt (req->easy, CURLOPT_SSLENGINE_DEFAULT, 1L);
          curl_easy_setopt (req->easy, CURLOPT_SSLKEYTYPE, "ENG");
        }
      if (g_str_has_prefix (self->tls_client_cert_path, "pkcs11:"))
        curl_easy_setopt (req->easy, CURLOPT_SSLCERTTYPE, "ENG");

      curl_easy_setopt (req->easy, CURLOPT_SSLCERT, self->tls_client_cert_path);
      curl_easy_setopt (req->easy, CURLOPT_SSLKEY, self->tls_client_key_path);
    }

  if ((self->config_flags & OSTREE_FETCHER_FLAGS_TRANSFER_GZIP) > 0)
    curl_easy_setopt (req->easy, CURLOPT_ACCEPT_ENCODING, "");

  /* We should only speak HTTP; TODO: only enable file if specified */
  curl_easy_setopt (req->easy, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS | CURLPROTO_FILE));
  /* Picked the current version in F25 as of 20170127, since
   * there are numerous HTTP/2 fixes since the original version in
   * libcurl 7.43.0.
   */
#ifdef BUILDOPT_HTTP2
  if (!(self->config_flags & OSTREE_FETCHER_FLAGS_DISABLE_HTTP2))
    {
#if CURL_AT_LEAST_VERSION(7, 51, 0)
      curl_easy_setopt (req->easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
#endif
      /* https://github.com/curl/curl/blob/curl-7_53_0/docs/examples/http2-download.c */
#if (CURLPIPE_MULTIPLEX > 0)
      /* wait for pipe connection to confirm */
      curl_easy_setopt (req->easy, CURLOPT_PIPEWAIT, 1L);
#endif
    }
#endif
  curl_easy_setopt (req->easy, CURLOPT_WRITEFUNCTION, write_cb);
  if (g_getenv ("OSTREE_DEBUG_HTTP"))
    curl_easy_setopt (req->easy, CURLOPT_VERBOSE, 1L);
  curl_easy_setopt (req->easy, CURLOPT_ERRORBUFFER, req->error);
  /* Note that the "easy" object's privdata is the task */
  curl_easy_setopt (req->easy, CURLOPT_NOPROGRESS, 1L);
  curl_easy_setopt (req->easy, CURLOPT_PROGRESSFUNCTION, prog_cb);
  curl_easy_setopt (req->easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt (req->easy, CURLOPT_CONNECTTIMEOUT, 30L);
  /* We used to set CURLOPT_LOW_SPEED_LIMIT and CURLOPT_LOW_SPEED_TIME
   * here, but see https://github.com/ostreedev/ostree/issues/878#issuecomment-347228854
   * basically those options don't play well with HTTP2 at the moment
   * where we can have lots of outstanding requests.  Further,
   * we could implement that functionality at a higher level
   * more consistently too.
   */

  /* closure bindings -> task */
  curl_easy_setopt (req->easy, CURLOPT_PRIVATE, task);
  curl_easy_setopt (req->easy, CURLOPT_WRITEDATA, task);
  curl_easy_setopt (req->easy, CURLOPT_PROGRESSDATA, task);

  CURLMcode multi_rc = curl_multi_add_handle (self->multi, req->easy);
  g_assert (multi_rc == CURLM_OK);
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
  FetcherRequest *req;
  g_autoptr(GMainContext) mainctx = g_main_context_ref_thread_default ();

  /* We don't support multiple concurrent main contexts; take
   * a ref to the first one, and require that later invocations
   * share it.
   */
  if (g_hash_table_size (self->outstanding_requests) == 0
      && mainctx != self->mainctx)
    {
      adopt_steal_mainctx (self, g_steal_pointer (&mainctx));
    }
  else
    {
      g_assert (self->mainctx == mainctx);
    }

  req = g_new0 (FetcherRequest, 1);
  req->refcount = 1;
  req->error[0]='\0';
  req->fetcher = self;
  req->mirrorlist = g_ptr_array_ref (mirrorlist);
  req->filename = g_strdup (filename);
  req->max_size = max_size;
  req->flags = flags;
  req->is_membuf = is_membuf;
  /* We'll allocate the tmpfile on demand, so we handle
   * file I/O errors just in the write func.
   */
  if (req->is_membuf)
    req->output_buf = g_string_new ("");

  task = g_task_new (self, cancellable, callback, user_data);
  /* We'll use the GTask priority for our own priority queue. */
  g_task_set_priority (task, priority);
  g_task_set_source_tag (task, _ostree_fetcher_request_async);
  g_task_set_task_data (task, req, (GDestroyNotify) request_unref);

  initiate_next_curl_request (req, task);

  g_hash_table_add (self->outstanding_requests, g_steal_pointer (&task));
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
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, _ostree_fetcher_request_async), FALSE);

  GTask *task = (GTask*)result;
  FetcherRequest *req = g_task_get_task_data (task);

  if (!g_task_propagate_boolean (task, error))
    return FALSE;

  g_assert (!req->is_membuf);
  *out_tmpf = req->tmpf;
  req->tmpf.initialized = FALSE; /* Transfer ownership */

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
  FetcherRequest *req;
  gpointer ret;

  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, _ostree_fetcher_request_async), FALSE);

  task = (GTask*)result;
  req = g_task_get_task_data (task);

  ret = g_task_propagate_pointer (task, error);
  if (!ret)
    return FALSE;

  g_assert (req->is_membuf);
  g_assert (out_buf);
  *out_buf = ret;

  return TRUE;
}

guint64
_ostree_fetcher_bytes_transferred (OstreeFetcher       *self)
{
  return self->bytes_transferred;
}
