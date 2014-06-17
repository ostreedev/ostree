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
#include "libgsystem.h"

#include <glib/gi18n.h>

typedef struct {
  const char *name;
  gboolean (*fn) (int argc, char **argv, OstreeSysroot *sysroot, GCancellable *cancellable, GError **error);
} OstreeAdminCommand;

static OstreeAdminCommand admin_subcommands[] = {
  { "cleanup", ot_admin_builtin_cleanup },
  { "config-diff", ot_admin_builtin_diff },
  { "deploy", ot_admin_builtin_deploy }, 
  { "init-fs", ot_admin_builtin_init_fs },
  { "instutil", ot_admin_builtin_instutil },
  { "os-init", ot_admin_builtin_os_init },
  { "status", ot_admin_builtin_status },
  { "switch", ot_admin_builtin_switch },
  { "undeploy", ot_admin_builtin_undeploy },
  { "upgrade", ot_admin_builtin_upgrade },
  { NULL, NULL }
};

gboolean
ostree_builtin_admin (int argc, char **argv, OstreeRepo *repo, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  const char *opt_sysroot = "/";
  const char *subcommand_name = NULL;
  OstreeAdminCommand *subcommand;
  gs_unref_object GFile *sysroot_path = NULL;
  gs_unref_object OstreeSysroot *sysroot = NULL;
  gboolean want_help = FALSE;
  int in, out, i;
  gboolean skip;

  /*
   * Parse the global options. We rearrange the options as
   * necessary, in order to pass relevant options through
   * to the commands, but also have them take effect globally.
   */

  for (in = 1, out = 1; in < argc; in++, out++)
    {
      /* The non-option is the command, take it out of the arguments */
      if (argv[in][0] != '-')
        {
          skip = (subcommand_name == NULL);
          if (subcommand_name == NULL)
            subcommand_name = argv[in];
        }

      /* The global long options */
      else if (argv[in][1] == '-')
        {
          skip = FALSE;

          if (g_str_equal (argv[in], "--"))
            {
              break;
            }
          else if (g_str_equal (argv[in], "--help"))
            {
              want_help = TRUE;
            }
          else if (g_str_equal (argv[in], "--sysroot") && in + 1 < argc)
            {
              opt_sysroot = argv[in + 1];
              skip = TRUE;
              in++;
            }
          else if (g_str_has_prefix (argv[in], "--sysroot="))
            {
              opt_sysroot = argv[in] + 10;
              skip = TRUE;
            }
          else if (subcommand_name == NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unknown or invalid admin option: %s", argv[in]);
              goto out;
            }
        }

      /* The global short options */
      else
        {
          skip = FALSE;
          for (i = 1; argv[in][i] != '\0'; i++)
            {
              switch (argv[in][i])
              {
                case 'h':
                  want_help = TRUE;
                  break;

                default:
                  if (subcommand_name == NULL)
                    {
                      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Unknown or invalid admin option: %s", argv[in]);
                      goto out;
                    }
                  break;
              }
            }
        }

      /* Skipping this argument? */
      if (skip)
        out--;
      else
        argv[out] = argv[in];
    }

  argc = out;

  if (subcommand_name == NULL || want_help)
    {
      subcommand = admin_subcommands;
      g_print ("usage: ostree admin --sysroot=PATH COMMAND [options]\n");
      g_print ("Builtin commands:\n");
      while (subcommand->name)
        {
          g_print ("  %s\n", subcommand->name);
          subcommand++;
        }
      return subcommand_name == NULL ? 1 : 0;
    }

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
                   "Unknown admin command '%s'", subcommand_name);
      goto out;
    }

  sysroot_path = g_file_new_for_path (opt_sysroot);
  sysroot = ostree_sysroot_new (sysroot_path);
  if (!subcommand->fn (argc, argv, sysroot, cancellable, error))
    goto out;
 
  ret = TRUE;
 out:
  return ret;
}
