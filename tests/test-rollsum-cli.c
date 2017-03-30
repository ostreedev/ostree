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

#include "ostree-rollsum.h"

#include "libglnx.h"

int
main (int argc, char **argv)
{
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  GBytes *from_bytes = NULL;
  GBytes *to_bytes = NULL;
  const char *from_path;
  const char *to_path;
  OstreeRollsumMatches *matches;
  GMappedFile *mfile;

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  if (argc < 3)
    return 1;

  from_path = argv[1];
  to_path = argv[2];

  mfile = g_mapped_file_new (from_path, FALSE, error);
  if (!mfile)
    goto out;
  from_bytes = g_mapped_file_get_bytes (mfile);
  g_mapped_file_unref (mfile);
  mfile = g_mapped_file_new (to_path, FALSE, error);
  if (!mfile)
    goto out;
  to_bytes = g_mapped_file_get_bytes (mfile);
  g_mapped_file_unref (mfile);

  matches = _ostree_compute_rollsum_matches (from_bytes, to_bytes);

  g_printerr ("rollsum crcs=%u bufs=%u total=%u matchsize=%llu\n",
              matches->crcmatches,
              matches->bufmatches,
              matches->total, (unsigned long long)matches->match_size);

 out:
  if (local_error)
    {
      g_printerr ("%s\n", local_error->message);
      return 1;
    }
  return 0;
}
