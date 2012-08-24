/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <gio/gio.h>

#include <errno.h>
#include <string.h>

#include "ot-main.h"
#include "ot-builtins.h"

static OstreeCommand commands[] = {
  { "cat", ostree_builtin_cat, 0 },
  { "commit", ostree_builtin_commit, 0 },
  { "config", ostree_builtin_config, 0 },
  { "checkout", ostree_builtin_checkout, 0 },
  { "checksum", ostree_builtin_checksum, OSTREE_BUILTIN_FLAG_NO_REPO },
  { "diff", ostree_builtin_diff, 0 },
  { "fsck", ostree_builtin_fsck, 0 },
  { "init", ostree_builtin_init, 0 },
  { "log", ostree_builtin_log, 0 },
  { "ls", ostree_builtin_ls, 0 },
  { "pack", ostree_builtin_pack, 0 },
  { "prune", ostree_builtin_prune, 0 },
  { "pull", NULL, 0 },
  { "pull-local", ostree_builtin_pull_local, 0 },
  { "remote", ostree_builtin_remote, 0 },
  { "rev-parse", ostree_builtin_rev_parse, 0 },
  { "show", ostree_builtin_show, 0 },
  { "unpack", ostree_builtin_unpack, 0 },
  { "write-refs", ostree_builtin_write_refs, 0 },
  { NULL }
};

static int
exec_external (int      argc,
               char   **argv,
               GError **error)
{
  gchar *command;
  gchar *tmp;
  int errn;

  command = g_strdup_printf ("ostree-%s", argv[1]);

  tmp = argv[1];
  argv[1] = command;

  execvp (command, argv + 1);

  errn = errno;
  argv[1] = tmp;
  g_free (command);

  if (errn == ENOENT)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unknown command: '%s'", argv[1]);
    }
  else
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errn),
                   "Failed to execute command: %s", g_strerror (errn));
    }

  return 1;
}

int
main (int    argc,
      char **argv)
{
  GError *error = NULL;
  int ret;

  ret = ostree_run (argc, argv, commands, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    {
      g_clear_error (&error);
      ret = exec_external (argc, argv, &error);
    }

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    ostree_usage (argv, commands, TRUE);

  if (error != NULL)
    {
      g_printerr ("%s\n", error->message);
      g_error_free (error);
    }

  return ret;
}
