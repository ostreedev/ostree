/*
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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

#include <libsoup/soup.h>

#include <gio/gunixoutputstream.h>

#include "ostree.h"
#include "ot-builtins.h"
#include "otutil.h"

#include <err.h>
#include <locale.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>

#if !SOUP_CHECK_VERSION(3, 0, 0)
#define SoupServerMessage SoupMessage
#define soup_server_message_get_method(msg) ((msg)->method)
#define soup_server_message_get_request_headers(msg) ((msg)->request_headers)
#define soup_server_message_get_response_headers(msg) ((msg)->response_headers)
#define soup_server_message_get_response_body(msg) ((msg)->response_body)
#define soup_server_message_set_status(msg, status) soup_message_set_status (msg, status)
#define soup_server_message_set_redirect(msg, status, uri) \
  soup_message_set_redirect (msg, status, uri)
#define soup_server_message_set_response(msg, ct, ru, rb, rl) \
  soup_message_set_response (msg, ct, ru, rb, rl)
#else
#define soup_server_message_set_status(msg, status) \
  soup_server_message_set_status (msg, status, NULL)
#endif

static char *opt_port_file = NULL;
static char *opt_log = NULL;
static gboolean opt_daemonize;
static gboolean opt_autoexit;
static gboolean opt_force_ranges;
static int opt_random_500s_percentage;
/* We have a strong upper bound for any unlikely
 * cases involving repeated random 500s. */
static int opt_random_500s_max = 100;
static int opt_random_408s_percentage;
static int opt_random_408s_max = 100;
static gint opt_port = 0;
static gchar **opt_expected_cookies;
static gchar **opt_expected_headers;
static gboolean opt_require_basic_auth;

static guint emitted_random_500s_count = 0;
static guint emitted_random_408s_count = 0;

typedef struct
{
  int root_dfd;
  gboolean running;
  GOutputStream *log;
} OtTrivialHttpd;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-trivial-httpd.xml) when changing the option list.
 */

static GOptionEntry options[]
    = { { "daemonize", 'd', 0, G_OPTION_ARG_NONE, &opt_daemonize, "Fork into background when ready",
          NULL },
        { "autoexit", 0, 0, G_OPTION_ARG_NONE, &opt_autoexit,
          "Automatically exit when directory is deleted", NULL },
        { "port", 'P', 0, G_OPTION_ARG_INT, &opt_port, "Use the specified TCP port", "PORT" },
        { "port-file", 'p', 0, G_OPTION_ARG_FILENAME, &opt_port_file,
          "Write port number to PATH (- for standard output)", "PATH" },
        { "force-range-requests", 0, 0, G_OPTION_ARG_NONE, &opt_force_ranges,
          "Force range requests by only serving half of files", NULL },
        { "require-basic-auth", 0, 0, G_OPTION_ARG_NONE, &opt_require_basic_auth,
          "Require username foouser, password barpw", NULL },
        { "random-500s", 0, 0, G_OPTION_ARG_INT, &opt_random_500s_percentage,
          "Generate random HTTP 500 errors approximately for PERCENTAGE requests", "PERCENTAGE" },
        { "random-500s-max", 0, 0, G_OPTION_ARG_INT, &opt_random_500s_max,
          "Limit HTTP 500 errors to MAX (default 100)", "MAX" },
        { "random-408s", 0, 0, G_OPTION_ARG_INT, &opt_random_408s_percentage,
          "Generate random HTTP 408 errors approximately for PERCENTAGE requests", "PERCENTAGE" },
        { "random-408s-max", 0, 0, G_OPTION_ARG_INT, &opt_random_408s_max,
          "Limit HTTP 408 errors to MAX (default 100)", "MAX" },
        { "log-file", 0, 0, G_OPTION_ARG_FILENAME, &opt_log, "Put logs here (use - for stdout)",
          "PATH" },
        { "expected-cookies", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_expected_cookies,
          "Expect given cookies in the http request", "KEY=VALUE" },
        { "expected-header", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_expected_headers,
          "Expect given headers in the http request", "KEY=VALUE" },
        { NULL } };

static void httpd_log (OtTrivialHttpd *httpd, const gchar *format, ...)
    __attribute__ ((format (printf, 2, 3)));

static void
httpd_log (OtTrivialHttpd *httpd, const gchar *format, ...)
{
  g_autoptr (GString) str = NULL;
  va_list args;
  gsize written;

  if (!httpd->log)
    return;

  {
    g_autoptr (GDateTime) now = g_date_time_new_now_local ();
    g_autofree char *timestamp = g_date_time_format (now, "%F %T");
    str = g_string_new (timestamp);
    g_string_append_printf (str, ".%06d - ", g_date_time_get_microsecond (now));
  }

  va_start (args, format);
  g_string_append_vprintf (str, format, args);
  va_end (args);

  (void)g_output_stream_write_all (httpd->log, str->str, str->len, &written, NULL, NULL);
}

static int
compare_strings (gconstpointer a, gconstpointer b)
{
  const char **sa = (const char **)a;
  const char **sb = (const char **)b;

  return strcmp (*sa, *sb);
}

static GString *
get_directory_listing (int dfd, const char *path)
{
  g_autoptr (GPtrArray) entries = g_ptr_array_new_with_free_func (g_free);
  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };
  g_autoptr (GError) local_error = NULL;
  GError **error = &local_error;
  guint i;
  char *escaped;
  GString *listing;

  listing = g_string_new ("<html>\r\n");

  if (!glnx_dirfd_iterator_init_at (dfd, path, FALSE, &dfd_iter, error))
    goto out;

  while (TRUE)
    {
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, NULL, error))
        goto out;

      if (dent == NULL)
        break;

      escaped = g_markup_escape_text (dent->d_name, -1);
      g_ptr_array_add (entries, escaped);
    }

  g_ptr_array_sort (entries, (GCompareFunc)compare_strings);

  escaped = g_markup_escape_text (strchr (path, '/'), -1);
  g_string_append_printf (listing, "<head><title>Index of %s</title></head>\r\n", escaped);
  g_string_append_printf (listing, "<body><h1>Index of %s</h1>\r\n<p>\r\n", escaped);
  g_free (escaped);
  for (i = 0; i < entries->len; i++)
    {
      g_string_append_printf (listing, "<a href=\"%s\">%s</a><br>\r\n", (char *)entries->pdata[i],
                              (char *)entries->pdata[i]);
      g_free (g_steal_pointer (&entries->pdata[i]));
    }
  g_string_append (listing, "</body>\r\n</html>\r\n");
out:
  if (local_error)
    g_printerr ("%s\n", local_error->message);
  return listing;
}

/* Only allow reading files that have o+r, and for directories, o+x.
 * This makes this server relatively safe to use on multiuser
 * machines.
 */
static gboolean
is_safe_to_access (struct stat *stbuf)
{
  /* Only regular files or directores */
  if (!(S_ISREG (stbuf->st_mode) || S_ISDIR (stbuf->st_mode)))
    return FALSE;
  /* Must be o+r */
  if (!(stbuf->st_mode & S_IROTH))
    return FALSE;
  /* For directories, must be o+x */
  if (S_ISDIR (stbuf->st_mode) && !(stbuf->st_mode & S_IXOTH))
    return FALSE;
  return TRUE;
}

static void
close_socket (SoupServerMessage *msg, gpointer user_data)
{
  GSocket *sock = user_data;
  int sockfd;

  sockfd = g_socket_get_fd (sock);
#ifdef G_OS_WIN32
  shutdown (sockfd, SD_SEND);
#else
  shutdown (sockfd, SHUT_WR);
#endif
}

/* Returns the ETag including the surrounding quotes */
static gchar *
calculate_etag (GMappedFile *mapping)
{
  g_autoptr (GBytes) bytes = g_mapped_file_get_bytes (mapping);
  g_autofree gchar *checksum = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, bytes);
  return g_strconcat ("\"", checksum, "\"", NULL);
}

static GSList *
_server_cookies_from_request (SoupServerMessage *msg)
{
  SoupCookie *cookie;
  GSList *cookies = NULL;
  GHashTable *params;
  GHashTableIter iter;
  gpointer name, value;
  const char *header;
  const char *host;

  header = soup_message_headers_get_one (soup_server_message_get_request_headers (msg), "Cookie");
  if (!header)
    return NULL;

#if !SOUP_CHECK_VERSION(3, 0, 0)
  host = soup_uri_get_host (soup_message_get_uri (msg));
#else
  host = g_uri_get_host (soup_server_message_get_uri (msg));
#endif
  params = soup_header_parse_semi_param_list (header);
  g_hash_table_iter_init (&iter, params);

  while (g_hash_table_iter_next (&iter, &name, &value))
    {
      if (!name || !value)
        continue;
      cookie = soup_cookie_new (name, value, host, NULL, 0);
      cookies = g_slist_prepend (cookies, cookie);
    }

  soup_header_free_param_list (params);

  return g_slist_reverse (cookies);
}

static void
#if !SOUP_CHECK_VERSION(3, 0, 0)
do_get (OtTrivialHttpd *self, SoupServer *server, SoupServerMessage *msg, const char *path,
        SoupClientContext *context)
#else
do_get (OtTrivialHttpd *self, SoupServer *server, SoupServerMessage *msg, const char *path)
#endif
{
  char *slash;
  int ret;
  struct stat stbuf;

  httpd_log (self, "serving %s\n", path);

  if (opt_expected_cookies)
    {
      GSList *cookies = _server_cookies_from_request (msg);
      GSList *l;
      int i;

      for (i = 0; opt_expected_cookies[i] != NULL; i++)
        {
          gboolean found = FALSE;
          gchar *k = opt_expected_cookies[i];
          gchar *v = strchr (k, '=') + 1;

          for (l = cookies; l != NULL; l = g_slist_next (l))
            {
              SoupCookie *c = l->data;

              if (!strncmp (k, soup_cookie_get_name (c), v - k - 1)
                  && !strcmp (v, soup_cookie_get_value (c)))
                {
                  found = TRUE;
                  break;
                }
            }

          if (!found)
            {
              httpd_log (self, "Expected cookie not found %s\n", k);
              soup_server_message_set_status (msg, SOUP_STATUS_FORBIDDEN);
              g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);
              goto out;
            }
        }
      g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);
    }

  if (opt_expected_headers)
    {
      for (int i = 0; opt_expected_headers[i] != NULL; i++)
        {
          const gchar *kv = opt_expected_headers[i];
          const gchar *eq = strchr (kv, '=');

          g_assert (eq);

          {
            g_autofree char *k = g_strndup (kv, eq - kv);
            const gchar *expected_v = eq + 1;
            const gchar *found_v
                = soup_message_headers_get_one (soup_server_message_get_request_headers (msg), k);

            if (!found_v)
              {
                httpd_log (self, "Expected header not found %s\n", k);
                soup_server_message_set_status (msg, SOUP_STATUS_FORBIDDEN);
                goto out;
              }
            if (strcmp (found_v, expected_v) != 0)
              {
                httpd_log (self, "Expected header %s: %s but found %s\n", k, expected_v, found_v);
                soup_server_message_set_status (msg, SOUP_STATUS_FORBIDDEN);
                goto out;
              }
          }
        }
    }

  if (strstr (path, "../") != NULL)
    {
      soup_server_message_set_status (msg, SOUP_STATUS_FORBIDDEN);
      goto out;
    }

  if (opt_random_500s_percentage > 0 && emitted_random_500s_count < opt_random_500s_max
      && g_random_int_range (0, 100) < opt_random_500s_percentage)
    {
      emitted_random_500s_count++;
      soup_server_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
      goto out;
    }
  else if (opt_random_408s_percentage > 0 && emitted_random_408s_count < opt_random_408s_max
           && g_random_int_range (0, 100) < opt_random_408s_percentage)
    {
      emitted_random_408s_count++;
      soup_server_message_set_status (msg, SOUP_STATUS_REQUEST_TIMEOUT);
      goto out;
    }

  while (path[0] == '/')
    path++;

  do
    ret = fstatat (self->root_dfd, path, &stbuf, 0);
  while (ret == -1 && errno == EINTR);
  if (ret == -1)
    {
      if (errno == EPERM)
        soup_server_message_set_status (msg, SOUP_STATUS_FORBIDDEN);
      else if (errno == ENOENT)
        soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND);
      else
        soup_server_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
      goto out;
    }

  if (!is_safe_to_access (&stbuf))
    {
      soup_server_message_set_status (msg, SOUP_STATUS_FORBIDDEN);
      goto out;
    }

  if (S_ISDIR (stbuf.st_mode))
    {
      slash = strrchr (path, '/');
      if (!slash || slash[1])
        {
          g_autofree char *redir_uri = NULL;

#if !SOUP_CHECK_VERSION(3, 0, 0)
          redir_uri = g_strdup_printf ("%s/", soup_uri_get_path (soup_message_get_uri (msg)));
#else
          redir_uri = g_strdup_printf ("%s/", g_uri_get_path (soup_server_message_get_uri (msg)));
#endif
          soup_server_message_set_redirect (msg, SOUP_STATUS_MOVED_PERMANENTLY, redir_uri);
        }
      else
        {
          g_autofree char *index_realpath = g_strconcat (path, "/index.html", NULL);
          if (fstatat (self->root_dfd, index_realpath, &stbuf, 0) != -1)
            {
              g_autofree char *index_path = g_strconcat (path, "/index.html", NULL);
#if !SOUP_CHECK_VERSION(3, 0, 0)
              do_get (self, server, msg, index_path, context);
#else
              do_get (self, server, msg, index_path);
#endif
            }
          else
            {
              GString *listing = get_directory_listing (self->root_dfd, path);
              soup_server_message_set_response (msg, "text/html", SOUP_MEMORY_TAKE, listing->str,
                                                listing->len);
              soup_server_message_set_status (msg, SOUP_STATUS_OK);
              g_string_free (listing, FALSE);
            }
        }
    }
  else
    {
      if (!S_ISREG (stbuf.st_mode))
        {
          soup_server_message_set_status (msg, SOUP_STATUS_FORBIDDEN);
          goto out;
        }

      glnx_autofd int fd = openat (self->root_dfd, path, O_RDONLY | O_CLOEXEC);
      if (fd < 0)
        {
          soup_server_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
          goto out;
        }

      g_autoptr (GMappedFile) mapping = g_mapped_file_new_from_fd (fd, FALSE, NULL);
      if (!mapping)
        {
          soup_server_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
          goto out;
        }
      (void)close (fd);
      fd = -1;

      /* Send caching headers */
      g_autoptr (GDateTime) last_modified = g_date_time_new_from_unix_utc (stbuf.st_mtim.tv_sec);
      if (last_modified != NULL)
        {
          g_autofree gchar *formatted
              = g_date_time_format (last_modified, "%a, %d %b %Y %H:%M:%S GMT");
          soup_message_headers_append (soup_server_message_get_response_headers (msg),
                                       "Last-Modified", formatted);
        }

      g_autofree gchar *etag = calculate_etag (mapping);
      if (etag != NULL)
        soup_message_headers_append (soup_server_message_get_response_headers (msg), "ETag", etag);

      if (!strcmp (soup_server_message_get_method (msg), "GET"))
        {
          gsize buffer_length, file_size;
          SoupRange *ranges;
          int ranges_length;
          gboolean have_ranges;

          file_size = g_mapped_file_get_length (mapping);
          have_ranges = soup_message_headers_get_ranges (
              soup_server_message_get_request_headers (msg), file_size, &ranges, &ranges_length);
          if (opt_force_ranges && !have_ranges && g_strrstr (path, "/objects") != NULL)
            {
              GSocket *sock;
              buffer_length = file_size / 2;
              soup_message_headers_set_content_length (
                  soup_server_message_get_response_headers (msg), file_size);
              soup_message_headers_append (soup_server_message_get_response_headers (msg),
                                           "Connection", "close");

              /* soup-message-io will wait for us to add
               * another chunk after the first, to fill out
               * the declared Content-Length. Instead, we
               * forcibly close the socket at that point.
               */
#if !SOUP_CHECK_VERSION(3, 0, 0)
              sock = soup_client_context_get_gsocket (context);
#else
              sock = soup_server_message_get_socket (msg);
#endif
              g_signal_connect (msg, "wrote-chunk", G_CALLBACK (close_socket), sock);
            }
          else
            buffer_length = file_size;

          if (have_ranges)
            {
              if (ranges_length > 0 && ranges[0].start >= file_size)
                {
                  soup_server_message_set_status (msg, SOUP_STATUS_REQUESTED_RANGE_NOT_SATISFIABLE);
                  soup_message_headers_free_ranges (soup_server_message_get_request_headers (msg),
                                                    ranges);
                  goto out;
                }
              soup_message_headers_free_ranges (soup_server_message_get_request_headers (msg),
                                                ranges);
            }
#if !SOUP_CHECK_VERSION(3, 0, 0)
          if (buffer_length > 0)
            {
              SoupBuffer *buffer;

              buffer = soup_buffer_new_with_owner (g_mapped_file_get_contents (mapping),
                                                   buffer_length, g_mapped_file_ref (mapping),
                                                   (GDestroyNotify)g_mapped_file_unref);
              soup_message_body_append_buffer (msg->response_body, buffer);
              soup_buffer_free (buffer);
            }
#else
          if (buffer_length > 0 && buffer_length == file_size)
            {
              GBytes *bytes = g_mapped_file_get_bytes (mapping);
              soup_message_body_append_bytes (soup_server_message_get_response_body (msg), bytes);
              g_bytes_unref (bytes);
            }
          else if (buffer_length > 0)
            {
              gchar *contents = g_mapped_file_get_contents (mapping);
              soup_message_body_append (soup_server_message_get_response_body (msg),
                                        SOUP_MEMORY_COPY, contents, buffer_length);
            }
#endif
        }
      else /* method == HEAD */
        {
          g_autofree char *length = NULL;

          /* We could just use the same code for both GET and
           * HEAD (soup-message-server-io.c will fix things up).
           * But we'll optimize and avoid the extra I/O.
           */
          length = g_strdup_printf ("%lu", (gulong)stbuf.st_size);
          soup_message_headers_append (soup_server_message_get_response_headers (msg),
                                       "Content-Length", length);
        }

      /* Check client’s caching headers. */
      const gchar *if_modified_since = soup_message_headers_get_one (
          soup_server_message_get_request_headers (msg), "If-Modified-Since");
      const gchar *if_none_match = soup_message_headers_get_one (
          soup_server_message_get_request_headers (msg), "If-None-Match");

      if (if_none_match != NULL && etag != NULL)
        {
          if (g_strcmp0 (etag, if_none_match) == 0)
            {
              soup_server_message_set_status (msg, SOUP_STATUS_NOT_MODIFIED);
              soup_message_body_truncate (soup_server_message_get_response_body (msg));
            }
          else
            {
              soup_server_message_set_status (msg, SOUP_STATUS_OK);
            }
        }
      else if (if_modified_since != NULL && last_modified != NULL)
        {
          g_autoptr (GDateTime) if_modified_since_dt = NULL;
#if !SOUP_CHECK_VERSION(3, 0, 0)
          SoupDate *if_modified_since_sd = soup_date_new_from_string (if_modified_since);

          if (if_modified_since_sd != NULL)
            if_modified_since_dt
                = g_date_time_new_from_unix_utc (soup_date_to_time_t (if_modified_since_sd));
#else
          if_modified_since_dt = soup_date_time_new_from_http_string (if_modified_since);
#endif

          if (if_modified_since_dt != NULL
              && g_date_time_compare (last_modified, if_modified_since_dt) <= 0)
            {
              soup_server_message_set_status (msg, SOUP_STATUS_NOT_MODIFIED);
              soup_message_body_truncate (soup_server_message_get_response_body (msg));
            }
          else
            {
              soup_server_message_set_status (msg, SOUP_STATUS_OK);
            }
        }
      else
        {
          soup_server_message_set_status (msg, SOUP_STATUS_OK);
        }
    }
out:
  {
#if !SOUP_CHECK_VERSION(3, 0, 0)
    guint status = 0;
    g_autofree gchar *reason = NULL;

    g_object_get (msg, "status-code", &status, "reason-phrase", &reason, NULL);
#else
    guint status = soup_server_message_get_status (msg);
    const char *reason = soup_server_message_get_reason_phrase (msg);
#endif

    httpd_log (self, "  status: %s (%u)\n", reason, status);
  }
  return;
}

static void
#if !SOUP_CHECK_VERSION(3, 0, 0)
httpd_callback (SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query,
                SoupClientContext *context, gpointer data)
#else
httpd_callback (SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query,
                gpointer data)
#endif
{
  OtTrivialHttpd *self = data;
  const char *meth = soup_server_message_get_method (msg);

  if (!strcmp (meth, "GET") || !strcmp (meth, "HEAD"))
#if !SOUP_CHECK_VERSION(3, 0, 0)
    do_get (self, server, msg, path, context);
#else
    do_get (self, server, msg, path);
#endif
  else
    soup_server_message_set_status (msg, SOUP_STATUS_NOT_IMPLEMENTED);
}

static gboolean
basic_auth_callback (SoupAuthDomain *auth_domain, SoupServerMessage *msg, const char *username,
                     const char *password, gpointer data)
{
  return g_str_equal (username, "foouser") && g_str_equal (password, "barpw");
}

static void
on_dir_changed (GFileMonitor *mon, GFile *file, GFile *other, GFileMonitorEvent event,
                gpointer user_data)
{
  OtTrivialHttpd *self = user_data;

  if (event == G_FILE_MONITOR_EVENT_DELETED)
    {
      httpd_log (self, "root directory removed, exiting\n");
      self->running = FALSE;
      g_main_context_wakeup (NULL);
    }
}

static gboolean
run (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr (GOptionContext) context = NULL;
  const char *dirpath;
  OtTrivialHttpd appstruct = {
    0,
  };
  OtTrivialHttpd *app = &appstruct;
  int pipefd[2] = { -1, -1 };
  glnx_unref_object SoupServer *server = NULL;
  g_autoptr (GFileMonitor) dirmon = NULL;

  context = g_option_context_new ("[DIR] - Simple webserver");
  g_option_context_add_main_entries (context, options, NULL);

  app->root_dfd = -1;

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc > 1)
    dirpath = argv[1];
  else
    dirpath = ".";

  if (!glnx_opendirat (AT_FDCWD, dirpath, TRUE, &app->root_dfd, error))
    goto out;

  if (!(opt_random_500s_percentage >= 0 && opt_random_500s_percentage <= 99))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid --random-500s=%u",
                   opt_random_500s_percentage);
      goto out;
    }

  if (!(opt_random_408s_percentage >= 0 && opt_random_408s_percentage <= 99))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid --random-408s=%u",
                   opt_random_408s_percentage);
      goto out;
    }

  if (opt_daemonize && (g_strcmp0 (opt_log, "-") == 0))
    {
      ot_util_usage_error (context, "Cannot use --log-file=- and --daemonize at the same time",
                           error);
      goto out;
    }

  /* Fork early before glib sets up its worker context and thread since they'll
   * be gone once the parent exits. The parent waits on a pipe with the child to
   * handle setup errors. The child writes a 0 when setup is successful and a 1
   * otherwise.
   */
  if (opt_daemonize)
    {
      if (pipe (pipefd) == -1)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }

      pid_t pid = fork ();
      if (pid == -1)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
      else if (pid > 0)
        {
          /* Parent, read the child status from the pipe */
          glnx_close_fd (&pipefd[1]);
          guint8 buf;
          int res = TEMP_FAILURE_RETRY (read (pipefd[0], &buf, 1));
          if (res < 0)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
          else if (res == 0)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Child process closed pipe without writing status");
              goto out;
            }

          g_debug ("Read %u from child", buf);
          if (buf > 0)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Child process failed during setup");
              goto out;
            }
          glnx_close_fd (&pipefd[0]);

          ret = TRUE;
          goto out;
        }

      /* Child, continue */
      glnx_close_fd (&pipefd[0]);
    }
  else
    {
      /* Since we're used for testing purposes, let's just do this by
       * default.  This ensures we exit when our parent does.
       */
      if (prctl (PR_SET_PDEATHSIG, SIGTERM) != 0)
        {
          if (errno != ENOSYS)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
    }

  if (opt_log)
    {
      GOutputStream *stream = NULL;

      if (g_strcmp0 (opt_log, "-") == 0)
        {
          stream = G_OUTPUT_STREAM (g_unix_output_stream_new (STDOUT_FILENO, FALSE));
        }
      else
        {
          g_autoptr (GFile) log_file = NULL;
          GFileOutputStream *log_stream;

          log_file = g_file_new_for_path (opt_log);
          log_stream = g_file_create (log_file, G_FILE_CREATE_PRIVATE, cancellable, error);
          if (!log_stream)
            goto out;
          stream = G_OUTPUT_STREAM (log_stream);
        }

      app->log = stream;
    }

#if SOUP_CHECK_VERSION(2, 48, 0)
  server = soup_server_new ("server-header", "ostree-httpd ", NULL);
  if (!soup_server_listen_all (server, opt_port, 0, error))
    goto out;
#else
  server = soup_server_new (SOUP_SERVER_PORT, opt_port, SOUP_SERVER_SERVER_HEADER, "ostree-httpd ",
                            NULL);
#endif

  if (opt_require_basic_auth)
    {
#if !SOUP_CHECK_VERSION(3, 0, 0)
      glnx_unref_object SoupAuthDomain *auth_domain = soup_auth_domain_basic_new (
          SOUP_AUTH_DOMAIN_REALM, "auth-test", SOUP_AUTH_DOMAIN_ADD_PATH, "/",
          SOUP_AUTH_DOMAIN_BASIC_AUTH_CALLBACK, basic_auth_callback, NULL);
#else
      glnx_unref_object SoupAuthDomain *auth_domain
          = soup_auth_domain_basic_new ("realm", "auth-test", NULL);
      soup_auth_domain_add_path (auth_domain, "/");
      soup_auth_domain_basic_set_auth_callback (auth_domain, basic_auth_callback, NULL, NULL);
#endif
      soup_server_add_auth_domain (server, auth_domain);
    }

  soup_server_add_handler (server, NULL, httpd_callback, app, NULL);
  if (opt_port_file)
    {
      g_autofree char *portstr = NULL;
#if SOUP_CHECK_VERSION(2, 48, 0)
      GSList *listeners = soup_server_get_listeners (server);
      g_autoptr (GSocket) listener = NULL;
      g_autoptr (GSocketAddress) addr = NULL;

      g_assert (listeners);
      listener = g_object_ref (listeners->data);
      g_slist_free (listeners);
      listeners = NULL;
      addr = g_socket_get_local_address (listener, error);
      if (!addr)
        goto out;

      g_assert (G_IS_INET_SOCKET_ADDRESS (addr));

      portstr
          = g_strdup_printf ("%u\n", g_inet_socket_address_get_port ((GInetSocketAddress *)addr));
#else
      portstr = g_strdup_printf ("%u\n", soup_server_get_port (server));
#endif

      if (g_strcmp0 ("-", opt_port_file) == 0)
        {
          fputs (portstr, stdout); // not g_print - this must go to stdout, not a handler
          fflush (stdout);
        }
      else if (!g_file_set_contents (opt_port_file, portstr, strlen (portstr), error))
        goto out;
    }
#if !SOUP_CHECK_VERSION(2, 48, 0)
  soup_server_run_async (server);
#endif

  if (opt_daemonize)
    {
      /* Write back a 0 to the pipe to indicate setup was successful. */
      guint8 buf = 0;
      g_debug ("Writing %u to parent", buf);
      if (TEMP_FAILURE_RETRY (write (pipefd[1], &buf, 1)) == -1)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
      glnx_close_fd (&pipefd[1]);

      if (setsid () < 0)
        {
          glnx_set_prefix_error_from_errno (error, "%s", "setsid: ");
          goto out;
        }
      /* Daemonising: close stdout/stderr so $() et al work on us */
      if (freopen ("/dev/null", "r", stdin) == NULL)
        {
          glnx_set_prefix_error_from_errno (error, "%s", "freopen: ");
          goto out;
        }
      if (freopen ("/dev/null", "w", stdout) == NULL)
        {
          glnx_set_prefix_error_from_errno (error, "%s", "freopen: ");
          goto out;
        }
      if (freopen ("/dev/null", "w", stderr) == NULL)
        {
          glnx_set_prefix_error_from_errno (error, "%s", "freopen: ");
          goto out;
        }
    }

  app->running = TRUE;
  if (opt_autoexit)
    {
      gboolean is_symlink = FALSE;
      g_autoptr (GFile) root = NULL;
      g_autoptr (GFileInfo) info = NULL;

      root = g_file_new_for_path (dirpath);
      info = g_file_query_info (root, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK,
                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
      if (!info)
        goto out;

      is_symlink = g_file_info_get_is_symlink (info);

      if (is_symlink)
        dirmon = g_file_monitor_file (root, 0, cancellable, error);
      else
        dirmon = g_file_monitor_directory (root, 0, cancellable, error);

      if (!dirmon)
        goto out;
      g_signal_connect (dirmon, "changed", G_CALLBACK (on_dir_changed), app);
    }
  httpd_log (app, "serving at root %s\n", dirpath);
  while (app->running)
    g_main_context_iteration (NULL, TRUE);

  ret = TRUE;
out:
  if (pipefd[0] >= 0)
    {
      /* Read end in the parent. This should only be open on errors. */
      g_assert_false (ret);
      glnx_close_fd (&pipefd[0]);
    }
  if (pipefd[1] >= 0)
    {
      /* Write end in the child. This should only be open on errors. */
      g_assert_false (ret);
      guint8 buf = 1;
      g_debug ("Writing %u to parent", buf);
      (void)TEMP_FAILURE_RETRY (write (pipefd[1], &buf, 1));
      glnx_close_fd (&pipefd[1]);
    }
  if (app->root_dfd != -1)
    (void)close (app->root_dfd);
  g_clear_object (&app->log);
  return ret;
}

int
main (int argc, char **argv)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GCancellable) cancellable = NULL;

  setlocale (LC_ALL, "");

  g_set_prgname (argv[0]);

  if (!run (argc, argv, cancellable, &error))
    {
      g_printerr ("%s%serror:%s%s %s\n", ot_get_red_start (), ot_get_bold_start (),
                  ot_get_bold_end (), ot_get_red_end (), error->message);
      return 1;
    }

  return 0;
}
