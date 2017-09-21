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
#include "bsdiff/bsdiff.h"
#include "bsdiff/bspatch.h"
#include <glib.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>

static int
bzpatch_read (const struct bspatch_stream* stream, void* buffer, int length)
{
  GInputStream *in = stream->opaque;
  if (length && ! g_input_stream_read (in,
                                       buffer,
                                       length,
                                       NULL,
                                       NULL))
    return -1;

  return 0;
}

static int
bzdiff_write (struct bsdiff_stream* stream, const void* buffer, int size)
{
  GOutputStream *out = stream->opaque;
  if (! g_output_stream_write (out,
                               buffer,
                               size,
                               NULL,
                               NULL))
    return -1;

  return 0;
}

static void
test_bsdiff (void)
{
#define OLD_SIZE 512
#define NEW_SIZE (512+24)

  struct bsdiff_stream bsdiff_stream;
  struct bspatch_stream bspatch_stream;
  int i;
  g_autofree guint8 *old = g_new (guint8, OLD_SIZE);
  g_autofree guint8 *new = g_new (guint8, NEW_SIZE);
  g_autofree guint8 *new_generated = g_new0 (guint8, NEW_SIZE);
  g_autoptr(GOutputStream) out = g_memory_output_stream_new_resizable ();
  g_autoptr(GInputStream) in = NULL;

  new[0] = 'A';
  for (i = 0; i < OLD_SIZE; i++)
    {
      old[i] = i;
      new[i + 1] = old[i];
    }
  for (i = OLD_SIZE + 1; i < NEW_SIZE; i++)
    new[i] = i;

  bsdiff_stream.malloc = malloc;
  bsdiff_stream.free = free;
  bsdiff_stream.write = bzdiff_write;
  bsdiff_stream.opaque = out;
  g_assert_cmpint (bsdiff (old, OLD_SIZE, new, NEW_SIZE, &bsdiff_stream), ==, 0);

  g_assert (g_output_stream_close (out, NULL, NULL));

  /* Now generate NEW_GENERATED from OLD and OUT.  */
  { g_autoptr(GBytes) bytes = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out));
    in = g_memory_input_stream_new_from_bytes (bytes);
  }
  bspatch_stream.read = bzpatch_read;
  bspatch_stream.opaque = in;

  g_assert_cmpint (bspatch (old, OLD_SIZE, new_generated, NEW_SIZE, &bspatch_stream), ==, 0);

  g_assert_cmpint (memcmp (new, new_generated, NEW_SIZE), ==, 0);
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/bsdiff", test_bsdiff);
  return g_test_run();
}
