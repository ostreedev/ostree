/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

#include "libglnx.h"

#include "ostree-varint.h"

static void
check_one_roundtrip (guint64    val)
{
  g_autoptr(GString) buf = g_string_new (NULL);
  guint64 newval;
  gsize bytes_read;

  _ostree_write_varuint64 (buf, val);
  if (g_test_verbose ())
    {
      g_autoptr(GVariant) v = g_variant_new_from_data (G_VARIANT_TYPE ("ay"), buf->str, buf->len, TRUE, NULL, NULL);
      g_autofree char *data = g_variant_print (v, FALSE);
      g_test_message ("%" G_GUINT64_FORMAT " -> %s", val, data);
    }
  g_assert (_ostree_read_varuint64 ((guint8*)buf->str, buf->len, &newval, &bytes_read));
  g_assert_cmpint (bytes_read, <=, 10);
  g_assert_cmpint (val, ==, newval);
}

static void
test_roundtrips (void)
{
  const guint64 test_inputs[] = { 0, 1, 0x6F, 0xA0, 0xFF, 0xF0F0, 0xCAFE,
                                  0xCAFEBABE, G_MAXUINT64, G_MAXUINT64-1,
                                  G_MAXUINT64 / 2};
  guint i;

  for (i = 0; i < G_N_ELEMENTS (test_inputs); i++)
    check_one_roundtrip (test_inputs[i]);
}

int
main (int argc, char **argv)
{

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/ostree/varint", test_roundtrips);

  return g_test_run ();
}
