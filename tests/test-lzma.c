/*
 * Copyright (C) 2015 Red Hat, Inc.
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
#include "libglnx.h"
#include "ostree-lzma-compressor.h"
#include "ostree-lzma-decompressor.h"
#include "ot-fs-utils.h"
#include <gio/gio.h>
#include <gio/gmemoryoutputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib.h>
#include <lzma.h>
#include <stdlib.h>
#include <string.h>

static void
helper_test_compress_decompress (const guint8 *data, gssize data_size)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GOutputStream) out_compress = g_memory_output_stream_new_resizable ();
  g_autoptr (GOutputStream) out_decompress = NULL;
  g_autoptr (GInputStream) in_compress
      = g_memory_input_stream_new_from_data (data, data_size, NULL);
  g_autoptr (GInputStream) in_decompress = NULL;

  {
    gssize n_bytes_written;
    g_autoptr (GInputStream) convin = NULL;
    g_autoptr (GConverter) compressor = (GConverter *)_ostree_lzma_compressor_new (NULL);
    convin = g_converter_input_stream_new ((GInputStream *)in_compress, compressor);
    n_bytes_written = g_output_stream_splice (
        out_compress, convin,
        G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET | G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE, NULL, &error);
    g_assert_cmpint (n_bytes_written, >, 0);
    g_assert_no_error (error);
  }

  out_decompress = g_memory_output_stream_new_resizable ();

  {
    gssize n_bytes_written;
    g_autoptr (GInputStream) convin = NULL;
    g_autoptr (GConverter) decompressor = (GConverter *)_ostree_lzma_decompressor_new ();
    g_autoptr (GBytes) bytes
        = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out_compress));

    in_decompress = g_memory_input_stream_new_from_bytes (bytes);
    convin = g_converter_input_stream_new ((GInputStream *)in_decompress, decompressor);
    n_bytes_written = g_output_stream_splice (
        out_decompress, convin,
        G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET | G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE, NULL, &error);
    g_assert_cmpint (n_bytes_written, >, 0);
    g_assert_no_error (error);
  }

  g_assert_cmpint (g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (out_decompress)),
                   ==, data_size);
  {
    gpointer new_data = g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (out_decompress));
    g_assert_cmpint (memcmp (new_data, data, data_size), ==, 0);
  }
}

static void
test_lzma_random (void)
{
  gssize i;
  guint8 buffer[4096];
  g_autoptr (GRand) r = g_rand_new ();
  for (i = 0; i < sizeof (buffer); i++)
    buffer[i] = g_rand_int (r);

  for (i = 2; i < (sizeof (buffer) - 1); i *= 2)
    {
      helper_test_compress_decompress (buffer, i - 1);
      helper_test_compress_decompress (buffer, i);
      helper_test_compress_decompress (buffer, i + 1);
    }
}

static void
test_lzma_big_buffer (void)
{
  const guint32 buffer_size = 1 << 21;
  g_autofree guint8 *buffer = g_new (guint8, buffer_size);

  memset (buffer, (int)'a', buffer_size);

  helper_test_compress_decompress (buffer, buffer_size);
}

/* Test that the LZMA decompressor rejects streams whose dictionary size
 * exceeds the configured memory limit (OSTREE_LZMA_DECODER_MEMLIMIT = 100 MiB).
 * We craft a minimal valid LZMA stream header that requests a 128 MiB
 * dictionary, which should trigger LZMA_MEMLIMIT_ERROR -> "Exceeded memory
 * limit" when the decompressor tries to process it.
 */
static void
test_lzma_memlimit (void)
{
  g_autoptr (GError) error = NULL;

  /* Build a minimal XZ stream that requests a 128 MiB dictionary.
   * lzma_options_lzma with dict_size = 128 MiB, compressed via
   * lzma_stream_encoder() with LZMA_CHECK_CRC64.
   */
  lzma_options_lzma opts;
  lzma_lzma_preset (&opts, LZMA_PRESET_DEFAULT);
  opts.dict_size = 128U * 1024U * 1024U; /* 128 MiB - exceeds 100 MiB limit */

  lzma_filter filters[] = { { .id = LZMA_FILTER_LZMA2, .options = &opts },
                            { .id = LZMA_VLI_UNKNOWN, .options = NULL } };

  lzma_stream strm = LZMA_STREAM_INIT;
  lzma_ret ret = lzma_stream_encoder (&strm, filters, LZMA_CHECK_CRC64);
  g_assert_cmpint (ret, ==, LZMA_OK);

  /* Compress a tiny payload just to produce a valid stream header */
  const guint8 input[] = "test";
  guint8 outbuf[4096];
  strm.next_in = input;
  strm.avail_in = sizeof (input);
  strm.next_out = outbuf;
  strm.avail_out = sizeof (outbuf);
  ret = lzma_code (&strm, LZMA_FINISH);
  g_assert (ret == LZMA_STREAM_END || ret == LZMA_OK);
  gsize compressed_size = sizeof (outbuf) - strm.avail_out;
  lzma_end (&strm);

  /* Now try to decompress through our decompressor — it should fail
   * with "Exceeded memory limit" because the stream header requests
   * a 128 MiB dictionary but our limit is 100 MiB.
   */
  g_autoptr (GConverter) decomp = (GConverter *)_ostree_lzma_decompressor_new ();
  g_autoptr (GInputStream) raw_in
      = g_memory_input_stream_new_from_data (outbuf, compressed_size, NULL);
  g_autoptr (GInputStream) convin = g_converter_input_stream_new (raw_in, decomp);
  g_autoptr (GOutputStream) out = g_memory_output_stream_new_resizable ();

  gssize n = g_output_stream_splice (
      out, convin, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, NULL,
      &error);
  g_assert_cmpint (n, ==, -1);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  /* The error message from ostree-lzma-common.c for LZMA_MEMLIMIT_ERROR */
  g_assert (strstr (error->message, "memory limit") != NULL);
}

/* Test that ot_map_anonymous_tmpfile_from_content_with_limit() correctly
 * rejects input that exceeds the configured byte limit.
 */
static void
test_lzma_decompressed_size_limit (void)
{
  g_autoptr (GError) error = NULL;
  const gsize data_size = 8192;
  g_autofree guint8 *data = g_new0 (guint8, data_size);
  memset (data, 'X', data_size);

  g_autoptr (GInputStream) in = g_memory_input_stream_new_from_data (data, data_size, NULL);

  /* Set a limit smaller than the data — should fail */
  GBytes *result = ot_map_anonymous_tmpfile_from_content_with_limit (in, 4096, NULL, &error);
  g_assert_null (result);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
  g_assert (strstr (error->message, "exceeds configured limit") != NULL);
}

/* Test that ot_map_anonymous_tmpfile_from_content_with_limit() succeeds
 * when the input fits within the limit.
 */
static void
test_lzma_decompressed_size_limit_ok (void)
{
  g_autoptr (GError) error = NULL;
  const gsize data_size = 4096;
  g_autofree guint8 *data = g_new0 (guint8, data_size);
  memset (data, 'Y', data_size);

  g_autoptr (GInputStream) in = g_memory_input_stream_new_from_data (data, data_size, NULL);

  /* Limit is exactly the data size — should succeed */
  g_autoptr (GBytes) result
      = ot_map_anonymous_tmpfile_from_content_with_limit (in, data_size, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (result);
  g_assert_cmpint (g_bytes_get_size (result), ==, data_size);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/lzma/random-buffer", test_lzma_random);
  g_test_add_func ("/lzma/big-buffer", test_lzma_big_buffer);
  g_test_add_func ("/lzma/memlimit", test_lzma_memlimit);
  g_test_add_func ("/lzma/decompressed-size-limit", test_lzma_decompressed_size_limit);
  g_test_add_func ("/lzma/decompressed-size-limit-ok", test_lzma_decompressed_size_limit_ok);

  return g_test_run ();
}
