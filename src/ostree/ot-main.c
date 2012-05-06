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

#include "ot-main.h"
#include "otutil.h"

static int
usage (char **argv, OstreeBuiltin *builtins, gboolean is_error)
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
set_error_print_usage (GError **error, OstreeBuiltin *builtins, const char *msg, char **argv)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, msg);
  usage (argv, builtins, TRUE);
}

int
ostree_main (int    argc,
             char **argv,
             OstreeBuiltin  *builtins)
{
  OstreeBuiltin *builtin;
  GError *error = NULL;
  int cmd_argc;
  char **cmd_argv = NULL;
  gboolean am_root;
  gboolean have_repo_arg;
  const char *cmd = NULL;
  const char *repo = NULL;
  GFile *repo_file = NULL;
  int arg_off;

  /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_type_init ();

  g_set_prgname (argv[0]);

  if (argc < 2)
    return usage (argv, builtins, 1);

  am_root = getuid () == 0;
  have_repo_arg = g_str_has_prefix (argv[1], "--repo=");

  if (!have_repo_arg && am_root)
    repo = "/sysroot/ostree/repo";
  else if (have_repo_arg)
    repo = argv[1] + strlen ("--repo=");
  else
    repo = NULL;

  if (repo)
    repo_file = ot_gfile_new_for_path (repo);

  cmd = strchr (argv[0], '-');
  if (cmd)
    {
      cmd += 1;
      arg_off = 1;
      if (have_repo_arg)
        arg_off += 1;
    }
  else if (!have_repo_arg)
    {
      arg_off = 2;
      cmd = argv[arg_off-1];
    }
  else
    {
      arg_off = 3;
      cmd = argv[arg_off-1];
    }

  builtin = builtins;
  while (builtin->name)
    {
      if (g_strcmp0 (cmd, builtin->name) == 0)
        break;
      builtin++;
    }

  if (!builtin->name)
    {
      set_error_print_usage (&error, builtins, "Unknown command", argv);
      goto out;
    }

  if (repo == NULL && !(builtin->flags & OSTREE_BUILTIN_FLAG_NO_REPO))
    {
      set_error_print_usage (&error, builtins, "Command requires a --repo argument", argv);
      goto out;
    }
  
  prep_builtin_argv (cmd, argc-arg_off, argv+arg_off, &cmd_argc, &cmd_argv);

  if (!builtin->fn (cmd_argc, cmd_argv, repo_file, &error))
    goto out;

 out:
  g_free (cmd_argv);
  g_clear_object (&repo_file);
  if (error)
    {
      g_printerr ("%s\n", error->message);
      g_clear_error (&error);
      return 1;
    }
  return 0;
}
