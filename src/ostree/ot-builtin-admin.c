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
  gboolean want_current_dir = FALSE;
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
          else if (g_str_equal (argv[in], "--print-current-dir"))
            {
              want_current_dir = TRUE;
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

  if (subcommand_name == NULL && (want_help || !want_current_dir))
    {
      void (*print_func) (const gchar *format, ...) = want_help ? g_print : g_printerr;

      subcommand = admin_subcommands;
      print_func ("usage: ostree admin [--sysroot=PATH] [--print-current-dir|COMMAND] [options]\n");
      print_func ("Builtin commands:\n");
      while (subcommand->name)
        {
          print_func ("  %s\n", subcommand->name);
          subcommand++;
        }

      if (want_help)
        ret = TRUE;
      else
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "No command specified");
      goto out;
    }

  sysroot_path = g_file_new_for_path (opt_sysroot);
  sysroot = ostree_sysroot_new (sysroot_path);

  if (want_current_dir)
    {
      gs_unref_ptrarray GPtrArray *deployments = NULL;
      OstreeDeployment *first_deployment;
      gs_unref_object GFile *deployment_file = NULL;
      gs_free char *deployment_path = NULL;

      if (!ostree_sysroot_load (sysroot, cancellable, error))
        goto out;

      deployments = ostree_sysroot_get_deployments (sysroot);
      if (deployments->len == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unable to find a deployment in sysroot");
          goto out;
        }
      first_deployment = deployments->pdata[0];
      deployment_file = ostree_sysroot_get_deployment_directory (sysroot, first_deployment);
      deployment_path = g_file_get_path (deployment_file);

      g_print ("%s\n", deployment_path);
      ret = TRUE;
      goto out;
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

  g_set_prgname (g_strdup_printf ("ostree admin %s", subcommand_name));

  if (!subcommand->fn (argc, argv, sysroot, cancellable, error))
    goto out;
 
  ret = TRUE;
 out:
  return ret;
}
