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
#include <unistd.h>
#include <locale.h>

#include "ot-main.h"
#include "ot-builtins.h"

static OstreeCommand commands[] = {
  { "admin", ostree_builtin_admin, OSTREE_BUILTIN_FLAG_NO_REPO },
  { "cat", ostree_builtin_cat, 0 },
  { "checkout", ostree_builtin_checkout, 0 },
  { "checksum", ostree_builtin_checksum, OSTREE_BUILTIN_FLAG_NO_REPO },
  { "commit", ostree_builtin_commit, 0 },
  { "config", ostree_builtin_config, 0 },
  { "diff", ostree_builtin_diff, 0 },
  { "fsck", ostree_builtin_fsck, 0 },
  { "init", ostree_builtin_init, OSTREE_BUILTIN_FLAG_NO_CHECK },
  { "log", ostree_builtin_log, 0 },
  { "ls", ostree_builtin_ls, 0 },
  { "prune", ostree_builtin_prune, 0 },
  { "pull-local", ostree_builtin_pull_local, 0 },
#ifdef HAVE_LIBSOUP 
  { "pull", ostree_builtin_pull, 0 },
#endif
  { "refs", ostree_builtin_refs, 0 },
  { "remote", ostree_builtin_remote, 0 },
  { "reset", ostree_builtin_reset, 0 },
  { "rev-parse", ostree_builtin_rev_parse, 0 },
  { "show", ostree_builtin_show, 0 },
  { "static-delta", ostree_builtin_static_delta, 0 },
#ifdef HAVE_LIBSOUP 
  { "trivial-httpd", ostree_builtin_trivial_httpd, OSTREE_BUILTIN_FLAG_NO_REPO },
#endif
  { NULL }
};

int
main (int    argc,
      char **argv)
{
  GError *error = NULL;
  int ret;

  setlocale (LC_ALL, "");

  ret = ostree_run (argc, argv, commands, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    ostree_usage (argv, commands, TRUE);

  if (error != NULL)
    {
      int is_tty = isatty (1);
      const char *prefix = "";
      const char *suffix = "";
      if (is_tty)
        {
          prefix = "\x1b[31m\x1b[1m"; /* red, bold */
          suffix = "\x1b[22m\x1b[0m"; /* bold off, color reset */
        }
      g_printerr ("%serror: %s%s\n", prefix, suffix, error->message);
      g_error_free (error);
    }

  return ret;
}
