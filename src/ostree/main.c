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
  { "admin", ostree_builtin_admin },
  { "cat", ostree_builtin_cat },
  { "checkout", ostree_builtin_checkout },
  { "checksum", ostree_builtin_checksum },
  { "commit", ostree_builtin_commit },
  { "config", ostree_builtin_config },
  { "diff", ostree_builtin_diff },
  { "export", ostree_builtin_export },
  { "fsck", ostree_builtin_fsck },
  { "gpg-sign", ostree_builtin_gpg_sign },
  { "init", ostree_builtin_init },
  { "log", ostree_builtin_log },
  { "ls", ostree_builtin_ls },
  { "prune", ostree_builtin_prune },
  { "pull-local", ostree_builtin_pull_local },
#ifdef HAVE_LIBSOUP 
  { "pull", ostree_builtin_pull },
#endif
  { "refs", ostree_builtin_refs },
  { "remote", ostree_builtin_remote },
  { "reset", ostree_builtin_reset },
  { "rev-parse", ostree_builtin_rev_parse },
  { "show", ostree_builtin_show },
  { "static-delta", ostree_builtin_static_delta },
  { "summary", ostree_builtin_summary },
#if defined(HAVE_LIBSOUP) && defined(BUILDOPT_ENABLE_TRIVIAL_HTTPD_CMDLINE)
  { "trivial-httpd", ostree_builtin_trivial_httpd },
#endif
  { NULL }
};

int
main (int    argc,
      char **argv)
{
  g_autoptr(GError) error = NULL;
  int ret;

  setlocale (LC_ALL, "");

  g_set_prgname (argv[0]);

  ret = ostree_run (argc, argv, commands, &error);

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
    }

  return ret;
}
