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

#include <stdlib.h>
#include <string.h>

#include "ot-main.h"
#include "ostree.h"
#include "ot-admin-functions.h"
#include "otutil.h"

static char *opt_repo;
static char *opt_sysroot;
static gboolean opt_verbose;
static gboolean opt_version;
static gboolean opt_print_current_dir;

static GOptionEntry global_entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version information and exit", NULL },
  { NULL }
};

static GOptionEntry repo_entry[] = {
  { "repo", 0, 0, G_OPTION_ARG_STRING, &opt_repo, "Path to OSTree repository (defaults to /sysroot/ostree/repo)", "PATH" },
  { NULL }
};

static GOptionEntry global_admin_entries[] = {
  /* No description since it's hidden from --help output. */
  { "print-current-dir", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_print_current_dir, NULL, NULL },
  { "sysroot", 0, 0, G_OPTION_ARG_STRING, &opt_sysroot, "Create a new OSTree sysroot at PATH", "PATH" },
  { NULL }
};

static GOptionContext *
ostree_option_context_new_with_commands (OstreeCommand *commands)
{
  GOptionContext *context;
  GString *summary;

  context = g_option_context_new ("COMMAND");

  summary = g_string_new ("Builtin Commands:");

  while (commands->name != NULL)
    {
      g_string_append_printf (summary, "\n  %s", commands->name);
      commands++;
    }

  g_option_context_set_summary (context, summary->str);

  g_string_free (summary, TRUE);

  return context;
}

int
ostree_usage (OstreeCommand *commands,
              gboolean is_error)
{
  GOptionContext *context;
  g_autofree char *help;

  context = ostree_option_context_new_with_commands (commands);

  g_option_context_add_main_entries (context, global_entries, NULL);

  help = g_option_context_get_help (context, FALSE, NULL);

  if (is_error)
    g_printerr ("%s", help);
  else
    g_print ("%s", help);

  g_option_context_free (context);

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
  const char *command_name = NULL;
  g_autofree char *prgname = NULL;
  gboolean success = FALSE;
  int in, out;

  /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, message_handler, NULL);

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
          if (command_name == NULL)
            {
              command_name = argv[in];
              out--;
              continue;
            }
        }

      else if (g_str_equal (argv[in], "--"))
        {
          break;
        }

      argv[out] = argv[in];
    }

  argc = out;

  command = commands;
  while (command->name)
    {
      if (g_strcmp0 (command_name, command->name) == 0)
        break;
      command++;
    }

  if (!command->fn)
    {
      GOptionContext *context;
      g_autofree char *help;

      context = ostree_option_context_new_with_commands (commands);

      /* This will not return for some options (e.g. --version). */
      if (ostree_option_context_parse (context, NULL, &argc, &argv, OSTREE_BUILTIN_FLAG_NO_REPO, NULL, cancellable, &error))
        {
          if (command_name == NULL)
            {
              g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "No command specified");
            }
          else
            {
              g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unknown command '%s'", command_name);
              ostree_usage (commands, TRUE);
            }
        }

      help = g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);

      g_option_context_free (context);

      goto out;
    }

  prgname = g_strdup_printf ("%s %s", g_get_prgname (), command_name);
  g_set_prgname (prgname);

  
  if (!command->fn (argc, argv, cancellable, &error))
    goto out;

  success = TRUE;
 out:
  g_assert (success || error);

  if (error)
    {
      g_propagate_error (res_error, error);
      return 1;
    }
  return 0;
}

gboolean
ostree_option_context_parse (GOptionContext *context,
                             const GOptionEntry *main_entries,
                             int *argc,
                             char ***argv,
                             OstreeBuiltinFlags flags,
                             OstreeRepo **out_repo,
                             GCancellable *cancellable,
                             GError **error)
{
  glnx_unref_object OstreeRepo *repo = NULL;
  gboolean success = FALSE;

  /* Entries are listed in --help output in the order added.  We add the
   * main entries ourselves so that we can add the --repo entry first. */

  if (!(flags & OSTREE_BUILTIN_FLAG_NO_REPO))
    g_option_context_add_main_entries (context, repo_entry, NULL);

  if (main_entries != NULL)
    g_option_context_add_main_entries (context, main_entries, NULL);

  g_option_context_add_main_entries (context, global_entries, NULL);

  if (!g_option_context_parse (context, argc, argv, error))
    return FALSE;

  if (opt_version)
    {
      g_print ("%s\n  %s\n", PACKAGE_STRING, OSTREE_FEATURES);
      exit (EXIT_SUCCESS);
    }

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  if (opt_repo == NULL && !(flags & OSTREE_BUILTIN_FLAG_NO_REPO))
    {
      GError *local_error = NULL;

      repo = ostree_repo_new_default ();
      if (!ostree_repo_open (repo, cancellable, &local_error))
        {
          if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_autofree char *help = NULL;

              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Command requires a --repo argument");
              g_error_free (local_error);

              help = g_option_context_get_help (context, FALSE, NULL);
              g_printerr ("%s", help);
            }
          else
            {
              g_propagate_error (error, local_error);
            }
          goto out;
        }
    }
  else if (opt_repo != NULL)
    {
      g_autoptr(GFile) repo_file = g_file_new_for_path (opt_repo);

      repo = ostree_repo_new (repo_file);
      if (!(flags & OSTREE_BUILTIN_FLAG_NO_CHECK))
        {
          if (!ostree_repo_open (repo, cancellable, error))
            goto out;
        }
    }

  if (out_repo)
    *out_repo = g_steal_pointer (&repo);

  success = TRUE;

out:
  return success;
}

gboolean
ostree_admin_option_context_parse (GOptionContext *context,
                                   const GOptionEntry *main_entries,
                                   int *argc,
                                   char ***argv,
                                   OstreeAdminBuiltinFlags flags,
                                   OstreeSysroot **out_sysroot,
                                   GCancellable *cancellable,
                                   GError **error)
{
  g_autoptr(GFile) sysroot_path = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  gboolean success = FALSE;

  /* Entries are listed in --help output in the order added.  We add the
   * main entries ourselves so that we can add the --sysroot entry first. */

  g_option_context_add_main_entries (context, global_admin_entries, NULL);

  if (!ostree_option_context_parse (context, main_entries, argc, argv, OSTREE_BUILTIN_FLAG_NO_REPO, NULL, cancellable, error))
    goto out;

  if (opt_sysroot != NULL)
    sysroot_path = g_file_new_for_path (opt_sysroot);

  sysroot = ostree_sysroot_new (sysroot_path);

  if (flags & OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER)
    {
      GFile *path = ostree_sysroot_get_path (sysroot);

      /* If sysroot path is "/" then user must be root. */
      if (!g_file_has_parent (path, NULL) && getuid () != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                       "You must be root to perform this command");
          goto out;
        }
    }

  if (opt_print_current_dir)
    {
      g_autoptr(GPtrArray) deployments = NULL;
      OstreeDeployment *first_deployment;
      g_autoptr(GFile) deployment_file = NULL;
      g_autofree char *deployment_path = NULL;

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

      /* The g_autoptr, g_autofree etc. don't happen when we explicitly
       * exit, making valgrind complain about leaks */
      g_clear_object (&sysroot);
      g_clear_object (&sysroot_path);
      g_clear_object (&deployment_file);
      g_clear_pointer (&deployments, g_ptr_array_unref);
      g_clear_pointer (&deployment_path, g_free);
      exit (EXIT_SUCCESS);
    }

  if ((flags & OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED) == 0)
    {
      /* Released when sysroot is finalized, or on process exit */
      if (!ot_admin_sysroot_lock (sysroot, error))
        goto out;
    }

  if (out_sysroot)
    *out_sysroot = g_steal_pointer (&sysroot);

  success = TRUE;

out:
  return success;
}

gboolean
ostree_ensure_repo_writable (OstreeRepo *repo,
                             GError **error)
{
  gboolean ret;

  ret = ostree_repo_is_writable (repo, error);

  g_prefix_error (error, "Cannot write to repository: ");

  return ret;
}

void
ostree_print_gpg_verify_result (OstreeGpgVerifyResult *result)
{
  GString *buffer;
  guint n_sigs, ii;

  n_sigs = ostree_gpg_verify_result_count_all (result);

  /* XXX If we ever add internationalization, use ngettext() here. */
  g_print ("GPG: Verification enabled, found %u signature%s:\n",
           n_sigs, n_sigs == 1 ? "" : "s");

  buffer = g_string_sized_new (256);

  for (ii = 0; ii < n_sigs; ii++)
    {
      g_string_append_c (buffer, '\n');
      ostree_gpg_verify_result_describe (result, ii, buffer, "  ",
                                         OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);
    }

  g_print ("%s", buffer->str);
  g_string_free (buffer, TRUE);
}

gboolean
ot_enable_tombstone_commits (OstreeRepo *repo, GError **error)
{
  gboolean ret = FALSE;
  gboolean tombstone_commits = FALSE;
  GKeyFile *config = ostree_repo_get_config (repo);

  tombstone_commits = g_key_file_get_boolean (config, "core", "tombstone-commits", NULL);
  /* tombstone_commits is FALSE either if it is not found or it is really set to FALSE in the config file.  */
  if (!tombstone_commits)
    {
      g_key_file_set_boolean (config, "core", "tombstone-commits", TRUE);
      if (!ostree_repo_write_config (repo, config, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}
