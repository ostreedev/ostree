/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
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
helper_test_compress_decompress (const char *data, gssize data_size)
{
  GError *error = NULL;
  g_autoptr(GOutputStream) out_compress = g_memory_output_stream_new_resizable ();
  g_autoptr(GOutputStream) out_decompress = NULL;
  g_autoptr(GInputStream) in_compress = g_memory_input_stream_new_from_data (data, data_size, NULL);
  g_autoptr(GConverter) compressor = (GConverter*)_ostree_lzma_compressor_new (NULL);
  g_autoptr(GInputStream) in_decompress = NULL;
  g_autoptr(GConverter) decompressor = NULL;

  {
    gssize n_bytes_written = g_output_stream_splice (out_compress, in_compress,
                                                     G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET | G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                                     NULL, &error);
    g_assert_cmpint (n_bytes_written, >, 0);
    g_assert_no_error (error);
  }

  in_decompress = g_memory_input_stream_new_from_bytes (g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out_compress)));
  out_decompress = g_memory_output_stream_new_resizable ();

  decompressor = (GConverter*)_ostree_lzma_decompressor_new ();
  {
    gssize n_bytes_written = g_output_stream_splice (out_decompress, in_decompress,
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
test_lzma_compress_decompress (void)
{
  gssize i;
#define BUFFER_SIZE (4096 + 1)
  char buffer[BUFFER_SIZE];
  srandom (1);
  for (i = 0; i < BUFFER_SIZE; i++)
    buffer[i] = random();

  for (i = 2; i <= BUFFER_SIZE; i *= 2)
    {
      helper_test_compress_decompress (buffer, i - 1);
      helper_test_compress_decompress (buffer, i);
      helper_test_compress_decompress (buffer, i + 1);
    }
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/lzma/same-char-string", test_lzma_compress_decompress);

  return g_test_run();
}
