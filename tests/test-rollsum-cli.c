/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
