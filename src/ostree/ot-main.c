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

#include "ostree.h"
#include "ot-main.h"
#include "otutil.h"
#include "libgsystem.h"

int
ostree_usage (char **argv,
              OstreeCommand *commands,
              gboolean is_error)
{
  OstreeCommand *command = commands;
  void (*print_func) (const gchar *format, ...);

  if (is_error)
    print_func = g_printerr;
  else
    print_func = g_print;

  print_func ("usage: %s --repo=PATH COMMAND [options]\n",
              argv[0]);
  print_func ("Builtin commands:\n");

  while (command->name)
    {
      print_func ("  %s\n", command->name);
      command++;
    }
  return (is_error ? 1 : 0);
}

static void
message_handler (const gchar *log_domain,
                 GLogLevelFlags log_level,
                 const gchar *message,
                 gpointer user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    g_printerr ("OT: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

int
ostree_run (int    argc,
            char **argv,
            OstreeCommand *commands,
            GError **res_error)
{
  OstreeCommand *command;
  GError *error = NULL;
  GCancellable *cancellable = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  const char *cmd = NULL;
  const char *repo_arg = NULL;
  gboolean want_help = FALSE;
  gboolean skip;
  int in, out, i;

  /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_type_init ();

  g_set_prgname (argv[0]);

  g_log_set_handler (NULL, G_LOG_LEVEL_MESSAGE, message_handler, NULL);

  if (argc < 2)
    return ostree_usage (argv, commands, TRUE);

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
          skip = (cmd == NULL);
          if (cmd == NULL)
              cmd = argv[in];
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
          else if (g_str_equal (argv[in], "--repo") && in + 1 < argc)
            {
              repo_arg = argv[in + 1];
              skip = TRUE;
              in++;
            }
          else if (g_str_has_prefix (argv[in], "--repo="))
            {
              repo_arg = argv[in] + 7;
              skip = TRUE;
            }
          else if (g_str_equal (argv[in], "--verbose"))
            {
              g_log_set_handler (NULL, G_LOG_LEVEL_DEBUG, message_handler, NULL);
              skip = TRUE;
            }
          else if (cmd == NULL && g_str_equal (argv[in], "--version"))
            {
              g_print ("%s\n  %s\n", PACKAGE_STRING, OSTREE_FEATURES);
              return 0;
            }
          else if (cmd == NULL)
            {
              g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unknown or invalid global option: %s", argv[in]);
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
                case 'v':
                  g_log_set_handler (NULL, G_LOG_LEVEL_DEBUG, message_handler, NULL);
                  skip = TRUE;
                  break;
                default:
                  if (cmd == NULL)
                    {
                      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Unknown or invalid global option: %s", argv[in]);
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

  if (cmd == NULL)
    {
      if (!want_help)
        {
          g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "No command specified");
        }
      ostree_usage (argv, commands, TRUE);
      goto out;
    }

  command = commands;
  while (command->name)
    {
      if (g_strcmp0 (cmd, command->name) == 0)
        break;
      command++;
    }

  if (!command->fn)
    {
      gs_free char *msg = g_strdup_printf ("Unknown command '%s'", cmd);
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, msg);
      goto out;
    }

  g_set_prgname (g_strdup_printf ("ostree %s", cmd));

  if (repo_arg == NULL && !want_help &&
      !(command->flags & OSTREE_BUILTIN_FLAG_NO_REPO))
    {
      GError *temp_error = NULL;
      repo = ostree_repo_new_default ();
      if (!ostree_repo_open (repo, cancellable, &temp_error))
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Command requires a --repo argument");
              g_error_free (temp_error);
              ostree_usage (argv, commands, TRUE);
            }
          else
            {
              g_propagate_error (&error, temp_error);
            }
          goto out;
        }
    }
  else if (repo_arg)
    {
      gs_unref_object GFile *repo_file = g_file_new_for_path (repo_arg);
      repo = ostree_repo_new (repo_file);
      if (!(command->flags & OSTREE_BUILTIN_FLAG_NO_CHECK))
        {
          if (!ostree_repo_open (repo, cancellable, &error))
            goto out;
        }
    }
  
  if (!command->fn (argc, argv, repo, cancellable, &error))
    goto out;

 out:
  if (error)
    {
      g_propagate_error (res_error, error);
      return 1;
    }
  return 0;
}
