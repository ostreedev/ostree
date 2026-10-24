/* SPDX-License-Identifier: LGPL-2.0+ */

#include "config.h"

#include "libglnx.h"
#include "ostree.h"
#include <stdint.h>

typedef struct
{
  GSocketListener *listener;
  GCancellable *cancellable;
  GMutex mutex;
  GCond cond;
  gboolean done;
  gboolean send_headers;
  int64_t cancelled_at;
} StalledRequest;

static gpointer
serve_stalled_request (gpointer user_data)
{
  StalledRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketConnection) connection
      = g_socket_listener_accept (request->listener, NULL, NULL, &error);
  g_assert_no_error (error);
  g_socket_set_timeout (g_socket_connection_get_socket (connection), 5);

  g_autoptr (GDataInputStream) input
      = g_data_input_stream_new (g_io_stream_get_input_stream (G_IO_STREAM (connection)));
  g_data_input_stream_set_newline_type (input, G_DATA_STREAM_NEWLINE_TYPE_CR_LF);
  while (TRUE)
    {
      size_t length;
      g_autofree char *line = g_data_input_stream_read_line (input, &length, NULL, &error);
      g_assert_no_error (error);
      g_assert_nonnull (line);
      if (length == 0)
        break;
    }

  if (request->send_headers)
    {
      const char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 32\r\nConnection: close\r\n\r\n";
      GOutputStream *output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
      g_output_stream_write_all (output, response, sizeof response - 1, NULL, NULL, &error);
      g_assert_no_error (error);
    }

  /* Cancel only once the client has a request in flight, then withhold its body.
   * Close after five seconds so a broken client fails instead of hanging. */
  g_usleep (100 * 1000);
  request->cancelled_at = g_get_monotonic_time ();
  g_cancellable_cancel (request->cancellable);
  g_mutex_lock (&request->mutex);
  while (!request->done)
    {
      if (!g_cond_wait_until (&request->cond, &request->mutex,
                              request->cancelled_at + 5 * G_USEC_PER_SEC))
        break;
    }
  g_mutex_unlock (&request->mutex);
  g_io_stream_close (G_IO_STREAM (connection), NULL, &error);
  g_assert_no_error (error);
  return NULL;
}

static void
test_cancel_stalled_summary (gconstpointer user_data)
{
  g_autoptr (GError) error = NULL;
  g_auto (GLnxTmpDir) tmpdir = { 0 };
  glnx_mkdtemp ("test-pull-cancel.XXXXXX", 0700, &tmpdir, &error);
  g_assert_no_error (error);
  g_autoptr (GFile) repo_path = g_file_new_for_path (tmpdir.path);
  g_autoptr (OstreeRepo) repo = ostree_repo_new (repo_path);
  ostree_repo_create (repo, OSTREE_REPO_MODE_BARE_USER_ONLY, NULL, &error);
  g_assert_no_error (error);

  g_autoptr (GSocketListener) listener = g_socket_listener_new ();
  g_autoptr (GSocketAddress) address = g_inet_socket_address_new_from_string ("127.0.0.1", 0);
  g_autoptr (GSocketAddress) bound_address = NULL;
  g_socket_listener_add_address (listener, address, G_SOCKET_TYPE_STREAM,
                                 G_SOCKET_PROTOCOL_TCP, NULL, &bound_address, &error);
  g_assert_no_error (error);
  guint16 port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (bound_address));
  g_autofree char *url = g_strdup_printf ("http://127.0.0.1:%u", port);
  GVariantBuilder options;
  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "gpg-verify", g_variant_new_boolean (FALSE));
  g_variant_builder_add (&options, "{sv}", "gpg-verify-summary", g_variant_new_boolean (FALSE));
  g_autoptr (GVariant) opts = g_variant_ref_sink (g_variant_builder_end (&options));
  ostree_repo_remote_add (repo, "origin", url, opts, NULL, &error);
  g_assert_no_error (error);

  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  StalledRequest request = { .listener = listener,
                            .cancellable = cancellable,
                            .send_headers = GPOINTER_TO_INT (user_data) };
  /* The test server is loopback-only; do not let inherited proxy settings
   * route the request away from it. */
  g_setenv ("no_proxy", "127.0.0.1,localhost", TRUE);
  g_setenv ("NO_PROXY", "127.0.0.1,localhost", TRUE);
  g_mutex_init (&request.mutex);
  g_cond_init (&request.cond);
  GThread *server = g_thread_new ("stalled-http", serve_stalled_request, &request);

  g_autoptr (GBytes) summary = NULL;
  gboolean success
      = ostree_repo_remote_fetch_summary (repo, "origin", &summary, NULL, cancellable, &error);
  g_assert_false (success);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

  int64_t finished_at = g_get_monotonic_time ();
  g_mutex_lock (&request.mutex);
  request.done = TRUE;
  g_cond_signal (&request.cond);
  g_mutex_unlock (&request.mutex);
  g_thread_join (server);
  g_cond_clear (&request.cond);
  g_mutex_clear (&request.mutex);

  g_test_message ("Cancellation took %.3f seconds",
                  (finished_at - request.cancelled_at) / (double)G_USEC_PER_SEC);
  g_assert_cmpint (finished_at - request.cancelled_at, <, 3 * G_USEC_PER_SEC);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/pull/cancel-stalled-headers", GINT_TO_POINTER (FALSE),
                        test_cancel_stalled_summary);
  g_test_add_data_func ("/pull/cancel-stalled-body", GINT_TO_POINTER (TRUE),
                        test_cancel_stalled_summary);
  return g_test_run ();
}
