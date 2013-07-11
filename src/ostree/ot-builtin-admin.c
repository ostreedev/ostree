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

#include "ot-builtins.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ot-main.h"
#include "ostree.h"
#include "ostree-repo-file.h"

#include <glib/gi18n.h>

typedef struct {
  const char *name;
  gboolean (*fn) (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error);
} OstreeAdminCommand;

static OstreeAdminCommand admin_subcommands[] = {
  { "os-init", ot_admin_builtin_os_init },
  { "init-fs", ot_admin_builtin_init_fs },
  { "deploy", ot_admin_builtin_deploy },
  { "upgrade", ot_admin_builtin_upgrade },
  { "prune", ot_admin_builtin_prune },
  { "status", ot_admin_builtin_status },
  { "config-diff", ot_admin_builtin_diff },
  { NULL, NULL }
};

gboolean
ostree_builtin_admin (int argc, char **argv, GFile *repo_path, GError **error)
{
  gboolean ret = FALSE;
  __attribute__((unused)) GCancellable *cancellable = NULL;
  const char *opt_sysroot = "/";
  const char *subcommand_name;
  OstreeAdminCommand *subcommand;
  int subcmd_argc;
  OtAdminBuiltinOpts admin_opts;
  char **subcmd_argv = NULL;

  if (argc > 1 && g_str_has_prefix (argv[1], "--sysroot="))
    {
      opt_sysroot = argv[1] + strlen ("--sysroot=");
      argc--;
      argv++;
    }
  else if (argc <= 1 || g_str_has_prefix (argv[1], "--help"))
    {
      subcommand = admin_subcommands;
      g_print ("usage: ostree admin --sysroot=PATH COMMAND [options]\n");
      g_print ("Builtin commands:\n");
      while (subcommand->name)
        {
          g_print ("  %s\n", subcommand->name);
          subcommand++;
        }
      return argc <= 1 ? 1 : 0;
    }

  subcommand_name = argv[1];

  subcommand = admin_subcommands;
  while (subcommand->name)
    {
      if (g_strcmp0 (subcommand_name, subcommand->name) == 0)
        break;
      subcommand++;
    }

  if (!subcommand->name)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unknown command '%s'", subcommand_name);
      goto out;
    }

  ostree_prep_builtin_argv (subcommand_name, argc-2, argv+2, &subcmd_argc, &subcmd_argv);

  admin_opts.sysroot = g_file_new_for_path (opt_sysroot);
  if (!subcommand->fn (subcmd_argc, subcmd_argv, &admin_opts, error))
    goto out;
 
  ret = TRUE;
 out:
  return ret;
}
