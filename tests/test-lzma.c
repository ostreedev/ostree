/*
 * Copyright (C) 2015 Red Hat, Inc.
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
#include "libglnx.h"
#include <glib.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>
#include "ostree-lzma-compressor.h"
#include "ostree-lzma-decompressor.h"
#include <gio/gunixoutputstream.h>
#include <gio/gmemoryoutputstream.h>

static void
helper_test_compress_decompress (const guint8 *data, gssize data_size)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GOutputStream) out_compress = g_memory_output_stream_new_resizable ();
  g_autoptr(GOutputStream) out_decompress = NULL;
  g_autoptr(GInputStream) in_compress = g_memory_input_stream_new_from_data (data, data_size, NULL);
  g_autoptr(GInputStream) in_decompress = NULL;

  {
    gssize n_bytes_written;
    g_autoptr(GInputStream) convin = NULL;
    g_autoptr(GConverter) compressor = (GConverter*)_ostree_lzma_compressor_new (NULL);
    convin = g_converter_input_stream_new ((GInputStream*) in_compress, compressor);
    n_bytes_written = g_output_stream_splice (out_compress, convin,
                                              G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET | G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                              NULL, &error);
    g_assert_cmpint (n_bytes_written, >, 0);
    g_assert_no_error (error);
  }

  out_decompress = g_memory_output_stream_new_resizable ();

  {
    gssize n_bytes_written;
    g_autoptr(GInputStream) convin = NULL;
    g_autoptr(GConverter) decompressor = (GConverter*)_ostree_lzma_decompressor_new ();
    g_autoptr(GBytes) bytes = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out_compress));

    in_decompress = g_memory_input_stream_new_from_bytes (bytes);
    convin = g_converter_input_stream_new ((GInputStream*) in_decompress, decompressor);
    n_bytes_written = g_output_stream_splice (out_decompress, convin,
                                              G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET | G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                              NULL, &error);
    g_assert_cmpint (n_bytes_written, >, 0);
    g_assert_no_error (error);
  }

  g_assert_cmpint (g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (out_decompress)), ==, data_size);
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
  g_autoptr(GRand) r = g_rand_new ();
  for (i = 0; i < sizeof(buffer); i++)
    buffer[i] = g_rand_int (r);

  for (i = 2; i < (sizeof(buffer) - 1); i *= 2)
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

  memset (buffer, (int) 'a', buffer_size);

  helper_test_compress_decompress (buffer, buffer_size);
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/lzma/random-buffer", test_lzma_random);
  g_test_add_func ("/lzma/big-buffer", test_lzma_big_buffer);

  return g_test_run();
}
