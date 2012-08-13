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

#include "ot-admin-main.h"
#include "otutil.h"

static int
usage (char **argv, OtAdminBuiltin *builtins, gboolean is_error)
{
  OtAdminBuiltin *builtin = builtins;
  void (*print_func) (const gchar *format, ...);

  if (is_error)
    print_func = g_printerr;
  else
    print_func = g_print;

  print_func ("usage: %s COMMAND [options]\n",
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

  /* Should be argc - 1 + 1, to account for
     the first argument (removed) and for NULL pointer */
  cmd_argv = g_new0 (char *, argc);

  for (i = 0; i < argc-1; i++)
    cmd_argv[i] = argv[i+1];
  cmd_argv[i] = NULL;
  *out_argc = argc-1;
  *out_argv = cmd_argv;
}

static void
set_error_print_usage (GError **error, OtAdminBuiltin *builtins, const char *msg, char **argv)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, msg);
  usage (argv, builtins, TRUE);
}

int
ot_admin_main (int    argc,
               char **argv,
               OtAdminBuiltin  *builtins)
{
  OtAdminBuiltin *builtin;
  GError *error = NULL;
  int cmd_argc;
  char **cmd_argv = NULL;
  const char *cmd = NULL;

  /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_type_init ();

  g_set_prgname (argv[0]);

  if (argc < 2)
    return usage (argv, builtins, 1);

  if (geteuid () != 0)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "ostadmin: Can only be run as root");
      goto out;
    }

  cmd = argv[1];

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

  prep_builtin_argv (cmd, argc, argv, &cmd_argc, &cmd_argv);

  if (!builtin->fn (cmd_argc, cmd_argv, &error))
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
