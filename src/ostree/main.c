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

#include <string.h>

#include "ot-builtins.h"

static OstreeBuiltin builtins[] = {
  { "checkout", ostree_builtin_checkout, 0 },
  { "checksum", ostree_builtin_checksum, OSTREE_BUILTIN_FLAG_NO_REPO },
  { "diff", ostree_builtin_diff, 0 },
  { "init", ostree_builtin_init, 0 },
  { "commit", ostree_builtin_commit, 0 },
  { "compose", ostree_builtin_compose, 0 },
  { "local-clone", ostree_builtin_local_clone, 0 },
  { "log", ostree_builtin_log, 0 },
#ifdef HAVE_LIBSOUP_GNOME
  { "pull", ostree_builtin_pull, 0 },
#endif
  { "fsck", ostree_builtin_fsck, 0 },
  { "remote", ostree_builtin_remote, 0 },
  { "rev-parse", ostree_builtin_rev_parse, 0 },
  { "remote", ostree_builtin_remote, 0 },
  { "run-triggers", ostree_builtin_run_triggers, 0 },
  { "show", ostree_builtin_show, 0 },
  { NULL }
};

static int
usage (char **argv, gboolean is_error)
{
  OstreeBuiltin *builtin = builtins;
  void (*print_func) (const gchar *format, ...);

  if (is_error)
    print_func = g_printerr;
  else
    print_func = g_print;

  print_func ("usage: %s --repo=PATH COMMAND [options]\n",
              argv[0]);
  print_func ("Builtin commands:\n");

  while (builtin->name)
    {
      print_func ("  %s\n", builtin->name);
      builtin++;
    }
  return (is_error ? 1 : 0);
}

static void
prep_builtin_argv (const char *builtin,
                   int argc,
                   char **argv,
                   int *out_argc,
                   char ***out_argv)
{
  int i;
  char **cmd_argv;
  
  cmd_argv = g_new0 (char *, argc + 2);
  
  cmd_argv[0] = (char*)builtin;
  for (i = 0; i < argc; i++)
    cmd_argv[i+1] = argv[i];
  cmd_argv[i+1] = NULL;
  *out_argc = argc+1;
  *out_argv = cmd_argv;
}

static void
set_error_print_usage (GError **error, const char *msg, char **argv)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, msg);
  usage (argv, TRUE);
}

int
main (int    argc,
      char **argv)
{
  OstreeBuiltin *builtin;
  GError *error = NULL;
  int cmd_argc;
  char **cmd_argv = NULL;
  gboolean am_root;
  gboolean have_repo_arg;
  const char *cmd = NULL;
  const char *repo = NULL;

  g_type_init ();

  g_set_prgname (argv[0]);

  if (argc < 2)
    return usage (argv, 1);

  am_root = getuid () == 0;
  have_repo_arg = g_str_has_prefix (argv[1], "--repo=");

  if (!have_repo_arg && am_root)
    repo = "/sysroot/ostree/repo";
  else if (have_repo_arg)
    repo = argv[1] + strlen ("--repo=");
  else
    repo = NULL;

  if (!have_repo_arg)
    cmd = argv[1];
  else
    cmd = argv[2];

  builtin = builtins;
  while (builtin->name)
    {
      if (strcmp (cmd, builtin->name) == 0)
        break;
      builtin++;
    }

  if (!builtin)
    {
      set_error_print_usage (&error, "Unknown command", argv);
      goto out;
    }

  if (repo == NULL && !(builtin->flags & OSTREE_BUILTIN_FLAG_NO_REPO))
    {
      set_error_print_usage (&error, "Command requires a --repo argument", argv);
      goto out;
    }
  
  if (!have_repo_arg)
    prep_builtin_argv (cmd, argc-2, argv+2, &cmd_argc, &cmd_argv);
  else
    prep_builtin_argv (cmd, argc-3, argv+3, &cmd_argc, &cmd_argv);

  if (!builtin->fn (cmd_argc, cmd_argv, repo, &error))
    goto out;

 out:
  g_free (cmd_argv);
  if (error)
    {
      g_printerr ("%s\n", error->message);
      g_clear_error (&error);
      return 1;
    }
  return 0;
}
