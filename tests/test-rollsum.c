/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

#include "libgsystem.h"

#include "bupsplit.h"

#define BLOB_MAX (8192*4)
#define BLOB_READ_SIZE (1024*1024)

int
main (int argc, char **argv)
{
  GCancellable *cancellable = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;
  gs_unref_object GFile *path = NULL;
  GBytes *bytes = NULL;

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  if (argc > 1)
    {
      const guint8 *start;
      gsize len;

      path = g_file_new_for_path (argv[1]);
      bytes = gs_file_map_readonly (path, cancellable, error);
      if (!bytes)
	goto out;

      start = g_bytes_get_data (bytes, &len);
      while (TRUE)
	{
	  int offset, bits;
	  offset = bupsplit_find_ofs (start, MIN(G_MAXINT32, len), &bits); 
	  if (offset == 0)
	    break;
	  if (offset > BLOB_MAX)
	    offset = BLOB_MAX;
          g_print ("%" G_GUINT64_FORMAT "\n", (guint64)offset);
	  start += offset;
          len -= offset;
        }
    }
  else
    {
      bupsplit_selftest ();
    }

 out:
  g_clear_pointer (&bytes, g_bytes_unref);
  if (local_error)
    {
      g_printerr ("%s\n", local_error->message);
      g_error_free (local_error);
      return 1;
    }
  return 0;
}
