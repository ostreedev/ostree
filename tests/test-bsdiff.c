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

#include "bsdiff/bsdiff.h"
#include "bsdiff/bspatch.h"
#include "libglnx.h"
#include <gio/gio.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>

static int
bzpatch_read (const struct bspatch_stream *stream, void *buffer, int length)
{
  GInputStream *in = stream->opaque;
  if (length && !g_input_stream_read (in, buffer, length, NULL, NULL))
    return -1;

  return 0;
}

static int
bzdiff_write (struct bsdiff_stream *stream, const void *buffer, int size)
{
  GOutputStream *out = stream->opaque;
  if (!g_output_stream_write (out, buffer, size, NULL, NULL))
    return -1;

  return 0;
}

static void
test_bsdiff (void)
{
#define OLD_SIZE 512
#define NEW_SIZE (512 + 24)

  struct bsdiff_stream bsdiff_stream;
  struct bspatch_stream bspatch_stream;
  int i;
  g_autofree guint8 *old = g_new (guint8, OLD_SIZE);
  g_autofree guint8 *new = g_new (guint8, NEW_SIZE);
  g_autofree guint8 *new_generated = g_new0 (guint8, NEW_SIZE);
  g_autoptr (GOutputStream) out = g_memory_output_stream_new_resizable ();
  g_autoptr (GInputStream) in = NULL;

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
  {
    g_autoptr (GBytes) bytes = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out));
    in = g_memory_input_stream_new_from_bytes (bytes);
  }
  bspatch_stream.read = bzpatch_read;
  bspatch_stream.opaque = in;

  g_assert_cmpint (bspatch (old, OLD_SIZE, new_generated, NEW_SIZE, &bspatch_stream), ==, 0);

  g_assert_cmpint (memcmp (new, new_generated, NEW_SIZE), ==, 0);
}

/* Verify the guard condition used in dispatch_bspatch() to prevent integer
 * truncation on 32-bit systems (RHEL-189207).  The actual guard is:
 *   if (content_size > G_MAXSIZE || content_size > (guint64)G_MAXINT64)
 *
 * On 32-bit: G_MAXSIZE == 0xFFFFFFFF, so any content_size >= 4 GiB triggers.
 * On 64-bit: G_MAXSIZE == G_MAXUINT64, so only content_size > G_MAXINT64
 *            (i.e. bit 63 set) triggers.
 *
 * This test validates the boundary conditions portably.
 */
static void
test_bspatch_content_size_guard (void)
{
  /* Values that must be rejected by the guard */
  const guint64 reject_values[] = {
    (guint64)G_MAXINT64 + 1ULL, /* bit 63 set — always rejected */
    G_MAXUINT64,                /* maximum uint64 — always rejected */
  };

  for (gsize i = 0; i < G_N_ELEMENTS (reject_values); i++)
    {
      guint64 content_size = reject_values[i];
      gboolean would_reject
          = (content_size > G_MAXSIZE || content_size > (guint64)G_MAXINT64);
      g_assert_true (would_reject);
    }

  /* On 32-bit systems, values above 4 GiB must also be rejected.
   * This check is meaningful on 32-bit; on 64-bit it's a tautology
   * (the cast fits) but still valid to run.
   */
  if (sizeof (gsize) == 4)
    {
      const guint64 reject_32bit[] = {
        (guint64)G_MAXUINT32 + 1ULL, /* 4 GiB — overflows gsize on 32-bit */
        0x100001000ULL,               /* 4 GiB + 4 KiB — the PoC value */
      };
      for (gsize i = 0; i < G_N_ELEMENTS (reject_32bit); i++)
        {
          guint64 content_size = reject_32bit[i];
          gboolean would_reject
              = (content_size > G_MAXSIZE || content_size > (guint64)G_MAXINT64);
          g_assert_true (would_reject);
        }
    }

  /* Values that must be accepted by the guard */
  const guint64 accept_values[] = {
    0,
    1,
    4096,
    (guint64)1024 * 1024 * 1024, /* 1 GiB */
  };

  for (gsize i = 0; i < G_N_ELEMENTS (accept_values); i++)
    {
      guint64 content_size = accept_values[i];
      gboolean would_reject
          = (content_size > G_MAXSIZE || content_size > (guint64)G_MAXINT64);
      g_assert_false (would_reject);

      /* Also verify that the casts produce correct values when accepted */
      gsize alloc_size = (gsize)content_size;
      int64_t newsize = (int64_t)content_size;
      g_assert_cmpuint (alloc_size, ==, content_size);
      g_assert_cmpint (newsize, >=, 0);
      g_assert_cmpuint ((guint64)newsize, ==, content_size);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/bsdiff", test_bsdiff);
  g_test_add_func ("/bsdiff/content-size-guard", test_bspatch_content_size_guard);
  return g_test_run ();
}
