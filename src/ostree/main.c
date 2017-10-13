/*
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
  { "admin", ostree_builtin_admin ,
    "Commands that needs admin privilege" },
  { "cat", ostree_builtin_cat,
    "Concatenate contents of files"},
  { "checkout", ostree_builtin_checkout,
    "Check out a commit into a filesystem tree" },
  { "checksum", ostree_builtin_checksum,
    "Checksum a file or directory" },
  { "commit", ostree_builtin_commit,
    "Commit a new revision" },
  { "config", ostree_builtin_config,
    "Change repo configuration settings" },
  { "diff", ostree_builtin_diff,
    "Compare directory TARGETDIR against revision REV"},
  { "export", ostree_builtin_export,
    "Stream COMMIT to stdout in tar format" },
#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
  { "find-remotes", ostree_builtin_find_remotes,
    "Find remotes to serve the given refs" },
  { "create-usb", ostree_builtin_create_usb,
    "Copy the refs to a USB stick" },
#endif
  { "fsck", ostree_builtin_fsck,
    "Check the repository for consistency" },
  { "gpg-sign", ostree_builtin_gpg_sign,
    "Sign a commit" },
  { "init", ostree_builtin_init,
    "Initialize a new empty repository" },
  { "log", ostree_builtin_log,
    "Show log starting at commit or ref" },
  { "ls", ostree_builtin_ls,
    "List file paths" },
  { "prune", ostree_builtin_prune,
    "Search for unreachable objects" },
  { "pull-local", ostree_builtin_pull_local,
    "Copy data from SRC_REPO" },
#ifdef HAVE_LIBCURL_OR_LIBSOUP
  { "pull", ostree_builtin_pull,
    "Download data from remote repository" },
#endif
  { "refs", ostree_builtin_refs,
    "List refs" },
  { "remote", ostree_builtin_remote,
    "Remote commands that may involve internet access" },
  { "reset", ostree_builtin_reset,
    "Reset a REF to a previous COMMIT" },
  { "rev-parse", ostree_builtin_rev_parse,
    "Output the target of a rev" },
  { "show", ostree_builtin_show,
    "Output a metadata object" },
  { "static-delta", ostree_builtin_static_delta,
    "Static delta related commands" },
  { "summary", ostree_builtin_summary,
    "Manage summary metadata" },
#if defined(HAVE_LIBSOUP) && defined(BUILDOPT_ENABLE_TRIVIAL_HTTPD_CMDLINE)
  { "trivial-httpd", ostree_builtin_trivial_httpd,
    NULL },
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
