/* SPDX-License-Identifier: LGPL-2.0+ */

#include "config.h"

#include "ostree-fetcher-util.h"

typedef struct
{
  GSocketListener *listener;
  GCancellable *stop;
  const char *mode;
  int attempts;
  int fast_attempts;
} Server;

static gpointer
serve (gpointer user_data)
{
  Server *server = user_data;
  while (TRUE)
    {
      g_autoptr (GError) error = NULL;
      g_autoptr (GSocketConnection) connection
          = g_socket_listener_accept (server->listener, NULL, server->stop, &error);
      if (connection == NULL)
        {
          g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
          break;
        }
      g_autoptr (GDataInputStream) input
          = g_data_input_stream_new (g_io_stream_get_input_stream (G_IO_STREAM (connection)));
      g_autofree char *request = g_data_input_stream_read_line (input, NULL, NULL, &error);
      g_assert_no_error (error);
      while (TRUE)
        {
          g_autofree char *line = g_data_input_stream_read_line (input, NULL, NULL, &error);
          g_assert_no_error (error);
          g_assert_nonnull (line);
          if (*g_strstrip (line) == '\0')
            break;
        }

      gboolean fast = g_str_has_prefix (request, "GET /fast ");
      if (g_str_equal (server->mode, "redirect") && g_str_has_prefix (request, "GET /slow "))
        {
          const char *response = "HTTP/1.1 302 Found\r\nLocation: /target\r\n"
                                 "Retry-After: 60\r\nContent-Length: 0\r\n"
                                 "Connection: close\r\n\r\n";
          g_assert_true (
              g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM (connection)),
                                         response, strlen (response), NULL, NULL, &error));
          g_assert_no_error (error);
          g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, &error));
          g_assert_no_error (error);
          continue;
        }
      int attempt = fast ? -1 : g_atomic_int_add (&server->attempts, 1);
      gboolean unavailable = !fast && (attempt == 0 || g_str_equal (server->mode, "always"));
      if (g_str_equal (server->mode, "mirrors"))
        unavailable = !fast || g_atomic_int_add (&server->fast_attempts, 1) == 0;
      g_autofree char *header = NULL;
      if (unavailable)
        {
          const char *value = "1";
          if (g_str_equal (server->mode, "date") || g_str_equal (server->mode, "date-rfc850")
              || g_str_equal (server->mode, "date-asctime"))
            {
              g_autoptr (GDateTime) now = g_date_time_new_now_utc ();
              g_autoptr (GDateTime) then = g_date_time_add_seconds (now, 2);
              const char *format = "Retry-After: %a, %d %b %Y %H:%M:%S GMT\r\n";
              if (g_str_equal (server->mode, "date-rfc850"))
                format = "Retry-After: %A, %d-%b-%y %H:%M:%S GMT\r\n";
              else if (g_str_equal (server->mode, "date-asctime"))
                format = "Retry-After: %a %b %e %H:%M:%S %Y\r\n";
              header = g_date_time_format (then, format);
            }
          else
            {
              if (g_str_equal (server->mode, "invalid"))
                value = "invalid";
              else if (g_str_equal (server->mode, "past"))
                value = "Sun, 06 Nov 1994 08:49:37 GMT";
              else if (g_str_equal (server->mode, "zero"))
                value = "0";
              else if (g_str_equal (server->mode, "long"))
                value = "61";
              else if (g_str_equal (server->mode, "overflow"))
                value = "99999999999999999999999999999999";
              else if (g_str_equal (server->mode, "cancel"))
                value = "60";
              else if (g_str_equal (server->mode, "mirrors"))
                value = fast ? "0" : "61";
              header = g_strdup_printf ("rEtRy-AfTeR: %s\r\n", value);
            }
        }
      if (g_str_equal (server->mode, "redirect"))
        g_clear_pointer (&header, g_free);
      if (unavailable
          && (g_str_equal (server->mode, "truncated")
              || g_str_equal (server->mode, "truncated-long")
              || g_str_equal (server->mode, "truncated-mirrors")))
        {
          const char *delay = g_str_equal (server->mode, "truncated") ? "1" : "61";
          g_autofree char *response
              = g_strdup_printf ("HTTP/1.1 503 Service Unavailable\r\nRetry-After: %s\r\n"
                                 "Content-Length: 100\r\nConnection: close\r\n\r\nshort",
                                 delay);
          g_assert_true (
              g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM (connection)),
                                         response, strlen (response), NULL, NULL, &error));
          g_assert_no_error (error);
          g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, &error));
          g_assert_no_error (error);
          continue;
        }
      g_autofree char *response
          = g_strdup_printf ("HTTP/1.1 %s\r\n%sContent-Length: %u\r\nConnection: close\r\n\r\n%s",
                             unavailable ? "503 Service Unavailable" : "200 OK", header ?: "",
                             unavailable ? 0 : 2, unavailable ? "" : "ok");
      g_assert_true (
          g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM (connection)),
                                     response, strlen (response), NULL, NULL, &error));
      g_assert_no_error (error);
      g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, &error));
      g_assert_no_error (error);
    }
  return NULL;
}

static gpointer
cancel_later (gpointer user_data)
{
  g_usleep (500 * 1000);
  g_cancellable_cancel (user_data);
  return NULL;
}

static void
test_retry_after (gconstpointer user_data)
{
  const char *mode = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketListener) listener = g_socket_listener_new ();
  g_autoptr (GCancellable) stop = g_cancellable_new ();
  unsigned short port = g_socket_listener_add_any_inet_port (listener, NULL, &error);
  g_assert_no_error (error);
  Server server = { listener, stop, mode, 0, 0 };
  GThread *thread = g_thread_new ("retry-after-server", serve, &server);
  g_autofree char *url = g_strdup_printf ("http://127.0.0.1:%hu/slow", port);
  g_autoptr (OstreeFetcherURI) uri = _ostree_fetcher_uri_parse (url, &error);
  g_assert_no_error (error);
  g_autoptr (OstreeFetcher) fetcher
      = _ostree_fetcher_new (AT_FDCWD, NULL, OSTREE_FETCHER_FLAGS_DISABLE_HTTP2);
  _ostree_fetcher_set_retry_all (fetcher, !g_str_equal (mode, "disabled"));
  /* Ignore any proxy settings inherited from the environment. */
  _ostree_fetcher_set_proxy (fetcher, "");
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  GThread *cancel_thread = NULL;
  if (g_str_equal (mode, "cancel"))
    cancel_thread = g_thread_new ("cancel-fetch", cancel_later, cancellable);

  g_autoptr (GBytes) contents = NULL;
  int64_t start = g_get_monotonic_time ();
  gboolean success;
  if (g_str_equal (mode, "mirrors"))
    {
      g_autoptr (GPtrArray) mirrors
          = g_ptr_array_new_with_free_func ((GDestroyNotify)_ostree_fetcher_uri_free);
      g_ptr_array_add (mirrors, _ostree_fetcher_uri_clone (uri));
      g_autofree char *fast_url = g_strdup_printf ("http://127.0.0.1:%hu/fast", port);
      g_ptr_array_add (mirrors, _ostree_fetcher_uri_parse (fast_url, &error));
      g_assert_no_error (error);
      success = _ostree_fetcher_mirrored_request_to_membuf (fetcher, mirrors, NULL, 0, NULL, 0, 2,
                                                            &contents, NULL, NULL, NULL, 1024,
                                                            cancellable, &error);
    }
  else
    success = _ostree_fetcher_request_uri_to_membuf (
        fetcher, uri, 0, NULL, 0, g_str_equal (mode, "exhausted") ? 0 : 2, &contents, NULL, NULL,
        NULL, 1024, cancellable, &error);
  int64_t elapsed = g_get_monotonic_time () - start;
  if (cancel_thread != NULL)
    g_thread_join (cancel_thread);

  if (g_str_equal (mode, "cancel"))
    {
      g_assert_false (success);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_assert_cmpint (g_atomic_int_get (&server.attempts), ==, 1);
      g_assert_cmpint (elapsed, <, 5 * G_USEC_PER_SEC);
    }
  else if (g_str_equal (mode, "long") || g_str_equal (mode, "overflow")
           || g_str_equal (mode, "truncated-long") || g_str_equal (mode, "disabled")
           || g_str_equal (mode, "exhausted"))
    {
      g_assert_false (success);
      g_assert_nonnull (error);
      g_assert_cmpint (g_atomic_int_get (&server.attempts), ==, 1);
      /* Exhausted retries and declined retries must return without waiting. */
      g_assert_cmpint (elapsed, <, G_USEC_PER_SEC);
      if (g_str_equal (mode, "long") || g_str_equal (mode, "overflow")
          || g_str_equal (mode, "truncated-long"))
        {
          /* A new request through this fetcher must also respect the delay. */
          g_clear_error (&error);
          g_assert_false (_ostree_fetcher_request_uri_to_membuf (
              fetcher, uri, 0, NULL, 0, 2, &contents, NULL, NULL, NULL, 1024, NULL, &error));
          g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
          g_assert_cmpint (g_atomic_int_get (&server.attempts), ==, 1);
        }
    }
  else if (g_str_equal (mode, "always"))
    {
      g_assert_false (success);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
      g_assert_cmpint (g_atomic_int_get (&server.attempts), ==, 3);
      g_assert_cmpint (elapsed, >=, 2 * G_USEC_PER_SEC);
    }
  else
    {
      g_assert_true (success);
      g_assert_no_error (error);
      g_assert_cmpmem (g_bytes_get_data (contents, NULL), g_bytes_get_size (contents), "ok", 2);
      g_assert_cmpint (g_atomic_int_get (&server.attempts), ==,
                       g_str_equal (mode, "mirrors") ? 1 : 2);
      if (g_str_equal (mode, "mirrors"))
        g_assert_cmpint (g_atomic_int_get (&server.fast_attempts), ==, 2);
      if (g_str_equal (mode, "seconds") || g_str_has_prefix (mode, "date")
          || g_str_equal (mode, "truncated"))
        g_assert_cmpint (elapsed, >=, G_USEC_PER_SEC);
      if (g_str_equal (mode, "redirect"))
        g_assert_cmpint (elapsed, <, G_USEC_PER_SEC);
    }

  /* After cancellation, reuse the fetcher with a new synchronous request.
   * It creates a new main context, so the fetcher must release the old one. */
  if (g_str_equal (mode, "cancel"))
    {
      g_clear_error (&error);
      g_autofree char *fast_url = g_strdup_printf ("http://127.0.0.1:%hu/fast", port);
      g_autoptr (OstreeFetcherURI) fast_uri = _ostree_fetcher_uri_parse (fast_url, &error);
      g_assert_no_error (error);
      g_assert_true (_ostree_fetcher_request_uri_to_membuf (
          fetcher, fast_uri, 0, NULL, 0, 0, &contents, NULL, NULL, NULL, 1024, NULL, &error));
      g_assert_no_error (error);
    }

  g_cancellable_cancel (stop);
  g_thread_join (thread);
}

typedef struct
{
  gboolean tmpfile;
  gboolean done;
  int64_t finished;
} Completion;

static void
on_complete (GObject *object, GAsyncResult *result, gpointer user_data)
{
  Completion *completion = user_data;
  g_autoptr (GError) error = NULL;
  if (completion->tmpfile)
    {
      g_auto (GLnxTmpfile) tmpfile = { 0 };
      g_assert_true (_ostree_fetcher_request_to_tmpfile_finish (
          OSTREE_FETCHER (object), result, &tmpfile, NULL, NULL, NULL, &error));
      g_assert_no_error (error);
      char buf[2];
      g_assert_cmpint (read (tmpfile.fd, buf, sizeof (buf)), ==, sizeof (buf));
      g_assert_cmpmem (buf, sizeof (buf), "ok", 2);
    }
  else
    {
      g_autoptr (GBytes) bytes = NULL;
      g_assert_true (_ostree_fetcher_request_to_membuf_finish (OSTREE_FETCHER (object), result,
                                                               &bytes, NULL, NULL, NULL, &error));
      g_assert_no_error (error);
      g_assert_cmpmem (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes), "ok", 2);
    }
  completion->finished = g_get_monotonic_time ();
  completion->done = TRUE;
}

static void
test_truncated_mirrors (gconstpointer user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketListener) listener = g_socket_listener_new ();
  g_autoptr (GCancellable) stop = g_cancellable_new ();
  unsigned short port = g_socket_listener_add_any_inet_port (listener, NULL, &error);
  g_assert_no_error (error);
  Server server = { listener, stop, "truncated-mirrors", 0, 0 };
  GThread *thread = g_thread_new ("retry-after-server", serve, &server);
  g_autoptr (GPtrArray) mirrors
      = g_ptr_array_new_with_free_func ((GDestroyNotify)_ostree_fetcher_uri_free);
  const char *paths[] = { "slow", "fast" };
  for (size_t i = 0; i < G_N_ELEMENTS (paths); i++)
    {
      g_autofree char *url = g_strdup_printf ("http://127.0.0.1:%hu/%s", port, paths[i]);
      g_ptr_array_add (mirrors, _ostree_fetcher_uri_parse (url, &error));
      g_assert_no_error (error);
    }
  g_autoptr (OstreeFetcher) fetcher
      = _ostree_fetcher_new (AT_FDCWD, NULL, OSTREE_FETCHER_FLAGS_DISABLE_HTTP2);
  _ostree_fetcher_set_retry_all (fetcher, TRUE);
  _ostree_fetcher_set_proxy (fetcher, "");
  _ostree_fetcher_set_force_anonymous_tmpfiles (fetcher);

  Completion completion = { .tmpfile = GPOINTER_TO_INT (user_data) };
  if (completion.tmpfile)
    _ostree_fetcher_request_to_tmpfile (fetcher, mirrors, NULL, 0, NULL, 0, 1024,
                                        G_PRIORITY_DEFAULT, NULL, on_complete, &completion);
  else
    _ostree_fetcher_request_to_membuf (fetcher, mirrors, NULL, 0, NULL, 0, 1024, G_PRIORITY_DEFAULT,
                                       NULL, on_complete, &completion);
  while (!completion.done)
    g_main_context_iteration (NULL, TRUE);
  g_assert_cmpint (g_atomic_int_get (&server.attempts), ==, 1);

  /* The failed mirror must remain blocked after fallback succeeds. */
  g_autoptr (GBytes) contents = NULL;
  g_assert_false (_ostree_fetcher_request_uri_to_membuf (
      fetcher, mirrors->pdata[0], 0, NULL, 0, 0, &contents, NULL, NULL, NULL, 1024, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpint (g_atomic_int_get (&server.attempts), ==, 1);

  g_cancellable_cancel (stop);
  g_thread_join (thread);
}

static void
test_concurrent (gconstpointer user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketListener) listener = g_socket_listener_new ();
  g_autoptr (GCancellable) stop = g_cancellable_new ();
  unsigned short port = g_socket_listener_add_any_inet_port (listener, NULL, &error);
  g_assert_no_error (error);
  Server server = { listener, stop, "seconds", 0, 0 };
  GThread *thread = g_thread_new ("retry-after-server", serve, &server);
  g_autofree char *url = g_strdup_printf ("http://127.0.0.1:%hu/", port);
  g_autoptr (GPtrArray) mirrors
      = g_ptr_array_new_with_free_func ((GDestroyNotify)_ostree_fetcher_uri_free);
  g_ptr_array_add (mirrors, _ostree_fetcher_uri_parse (url, &error));
  g_assert_no_error (error);
  g_autoptr (OstreeFetcher) fetcher
      = _ostree_fetcher_new (AT_FDCWD, NULL, OSTREE_FETCHER_FLAGS_DISABLE_HTTP2);
  _ostree_fetcher_set_retry_all (fetcher, TRUE);
  _ostree_fetcher_set_proxy (fetcher, "");
  _ostree_fetcher_set_force_anonymous_tmpfiles (fetcher);

  /* Record a retry delay without waiting or retrying. */
  g_autoptr (GBytes) contents = NULL;
  int64_t start = g_get_monotonic_time ();
  g_assert_false (_ostree_fetcher_mirrored_request_to_membuf (
      fetcher, mirrors, "slow", 0, NULL, 0, 0, &contents, NULL, NULL, NULL, 1024, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_clear_error (&error);

  Completion slow = { .tmpfile = GPOINTER_TO_INT (user_data) };
  Completion fast = { 0 };
  if (slow.tmpfile)
    _ostree_fetcher_request_to_tmpfile (fetcher, mirrors, "slow", 0, NULL, 0, 1024,
                                        G_PRIORITY_DEFAULT, NULL, on_complete, &slow);
  else
    _ostree_fetcher_request_to_membuf (fetcher, mirrors, "slow", 0, NULL, 0, 1024,
                                       G_PRIORITY_DEFAULT, NULL, on_complete, &slow);
  _ostree_fetcher_request_to_membuf (fetcher, mirrors, "fast", 0, NULL, 0, 1024, G_PRIORITY_DEFAULT,
                                     NULL, on_complete, &fast);
  while (!slow.done || !fast.done)
    g_main_context_iteration (NULL, TRUE);
  g_assert_cmpint (fast.finished, <, slow.finished);
  g_assert_cmpint (fast.finished - start, <, G_USEC_PER_SEC);
  g_assert_cmpint (slow.finished - start, >=, G_USEC_PER_SEC);
  g_assert_cmpint (g_atomic_int_get (&server.attempts), ==, 2);

  g_cancellable_cancel (stop);
  g_thread_join (thread);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  const char *modes[]
      = { "seconds", "date",     "date-rfc850", "date-asctime", "invalid",       "past",
          "zero",    "long",     "overflow",    "disabled",     "exhausted",     "always",
          "cancel",  "redirect", "mirrors",     "truncated",    "truncated-long" };
  for (size_t i = 0; i < G_N_ELEMENTS (modes); i++)
    {
      g_autofree char *name = g_strdup_printf ("/fetcher/retry-after/%s", modes[i]);
      g_test_add_data_func (name, modes[i], test_retry_after);
    }
  g_test_add_data_func ("/fetcher/retry-after/concurrent-membuf", GINT_TO_POINTER (FALSE),
                        test_concurrent);
  g_test_add_data_func ("/fetcher/retry-after/concurrent-tmpfile", GINT_TO_POINTER (TRUE),
                        test_concurrent);
  g_test_add_data_func ("/fetcher/retry-after/truncated-mirrors-membuf", GINT_TO_POINTER (FALSE),
                        test_truncated_mirrors);
  g_test_add_data_func ("/fetcher/retry-after/truncated-mirrors-tmpfile", GINT_TO_POINTER (TRUE),
                        test_truncated_mirrors);
  return g_test_run ();
}
