/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2014 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include "ot-builtins.h"
#include "ot-admin-instutil-builtins.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ot-main.h"
#include "ostree.h"
#include "libgsystem.h"

#include <glib/gi18n.h>

typedef struct {
  const char *name;
  gboolean (*fn) (int argc, char **argv, OstreeSysroot *sysroot, GCancellable *cancellable, GError **error);
} OstreeAdminInstUtilCommand;

static OstreeAdminInstUtilCommand admin_instutil_subcommands[] = {
#ifdef HAVE_SELINUX
  { "selinux-ensure-labeled", ot_admin_instutil_builtin_selinux_ensure_labeled },
#endif
  { "set-kargs", ot_admin_instutil_builtin_set_kargs },
  { "grub2-generate", ot_admin_instutil_builtin_grub2_generate },
  { NULL, NULL }
};

gboolean
ot_admin_builtin_instutil (int argc, char **argv, OstreeSysroot *sysroot, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  OstreeAdminInstUtilCommand *subcommand;
  const char *subcommand_name = NULL;
  gboolean want_help = FALSE;
  int in, out, i;
  gboolean skip;

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
          else if (subcommand_name == NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unknown or invalid admin instutil option: %s", argv[in]);
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
                                   "Unknown or invalid admin instutil option: %s", argv[in]);
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

  if (subcommand_name == NULL)
    {
      void (*print_func) (const gchar *format, ...) = want_help ? g_print : g_printerr;

      subcommand = admin_instutil_subcommands;
      print_func ("usage: ostree admin instutil COMMAND [options]\n");
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

  subcommand = admin_instutil_subcommands;
  while (subcommand->name)
    {
      if (g_strcmp0 (subcommand_name, subcommand->name) == 0)
        break;
      subcommand++;
    }

  if (!subcommand->name)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unknown admin instutil command '%s'", subcommand_name);
      goto out;
    }

  g_set_prgname (g_strdup_printf ("ostree admin instutil %s", subcommand_name));

  if (!subcommand->fn (argc, argv, sysroot, cancellable, error))
    goto out;
 
  ret = TRUE;
 out:
  return ret;
}
