/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
  { "repo", 0, 0, G_OPTION_ARG_FILENAME, &opt_repo, "Path to OSTree repository (defaults to /sysroot/ostree/repo)", "PATH" },
  { NULL }
};

static GOptionEntry global_admin_entries[] = {
  /* No description since it's hidden from --help output. */
  { "print-current-dir", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_print_current_dir, NULL, NULL },
  { "sysroot", 0, 0, G_OPTION_ARG_FILENAME, &opt_sysroot, "Create a new OSTree sysroot at PATH", "PATH" },
  { NULL }
};

static GOptionContext *
ostree_option_context_new_with_commands (OstreeCommand *commands)
{
  GOptionContext *context = g_option_context_new ("COMMAND");

  g_autoptr(GString) summary = g_string_new ("Builtin Commands:");

  while (commands->name != NULL)
    {
      g_string_append_printf (summary, "\n  %-18s", commands->name);

      if (commands->description != NULL )
        g_string_append_printf (summary, "%s", commands->description);

      commands++;
    }

  g_option_context_set_summary (context, summary->str);

  return context;
}

int
ostree_usage (OstreeCommand *commands,
              gboolean is_error)
{
  g_autoptr(GOptionContext) context =
    ostree_option_context_new_with_commands (commands);
  g_option_context_add_main_entries (context, global_entries, NULL);

  g_autofree char *help = g_option_context_get_help (context, FALSE, NULL);
  if (is_error)
    g_printerr ("%s", help);
  else
    g_print ("%s", help);

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
#ifndef BUILDOPT_TSAN
  g_autofree char *prgname = NULL;
#endif
  const char *command_name = NULL;
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
      g_autoptr(GOptionContext) context =
        ostree_option_context_new_with_commands (commands);

      /* This will not return for some options (e.g. --version). */
      if (ostree_option_context_parse (context, NULL, &argc, &argv, NULL, NULL, cancellable, &error))
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
            }
        }

      ostree_usage (commands, TRUE);
      goto out;
    }

#ifndef BUILDOPT_TSAN
  prgname = g_strdup_printf ("%s %s", g_get_prgname (), command_name);
  g_set_prgname (prgname);
#endif
  OstreeCommandInvocation invocation = { .command = command };
  if (!command->fn (argc, argv, &invocation, cancellable, &error))
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

/* Process a --repo arg; used below, and for the remote builtins */
static OstreeRepo *
parse_repo_option (GOptionContext *context,
                   const char     *repo_path,
                   gboolean        skip_repo_open,
                   GCancellable   *cancellable,
                   GError        **error)
{
  g_autoptr(OstreeRepo) repo = NULL;

  if (repo_path == NULL)
    {
      g_autoptr(GError) local_error = NULL;

      repo = ostree_repo_new_default ();
      if (!ostree_repo_open (repo, cancellable, &local_error))
        {
          if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_autofree char *help = NULL;

              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Command requires a --repo argument");

              help = g_option_context_get_help (context, FALSE, NULL);
              g_printerr ("%s", help);
            }
          else
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
            }
          return NULL;
        }
    }
  else
    {
      g_autoptr(GFile) repo_file = g_file_new_for_path (repo_path);

      repo = ostree_repo_new (repo_file);
      if (!skip_repo_open)
        {
          if (!ostree_repo_open (repo, cancellable, error))
            return NULL;
        }
    }

  return g_steal_pointer (&repo);
}

/* Used by the remote builtins which are special in taking --sysroot or --repo */
gboolean
ostree_parse_sysroot_or_repo_option (GOptionContext *context,
                                     const char *sysroot_path,
                                     const char *repo_path,
                                     OstreeSysroot **out_sysroot,
                                     OstreeRepo **out_repo,
                                     GCancellable *cancellable,
                                     GError **error)
{
  g_autoptr(OstreeSysroot) sysroot = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  if (sysroot_path)
    {
      g_autoptr(GFile) sysroot_file = g_file_new_for_path (sysroot_path);
      sysroot = ostree_sysroot_new (sysroot_file);
      if (!ostree_sysroot_load (sysroot, cancellable, error))
        return FALSE;
      if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
        return FALSE;
    }
  else
    {
      repo = parse_repo_option (context, repo_path, FALSE, cancellable, error);
      if (!repo)
        return FALSE;
    }

  ot_transfer_out_value (out_sysroot, &sysroot);
  ot_transfer_out_value (out_repo, &repo);
  return TRUE;
}

gboolean
ostree_option_context_parse (GOptionContext *context,
                             const GOptionEntry *main_entries,
                             int *argc,
                             char ***argv,
                             OstreeCommandInvocation *invocation,
                             OstreeRepo **out_repo,
                             GCancellable *cancellable,
                             GError **error)
{
  g_autoptr(OstreeRepo) repo = NULL;
  /* When invocation is NULL,  do not fetch repo */
  const OstreeBuiltinFlags flags = invocation ? invocation->command->flags : OSTREE_BUILTIN_FLAG_NO_REPO;

  if (invocation && invocation->command->description != NULL)
    {
      const char *context_summary = g_option_context_get_summary (context);

      /* If the summary is originally empty, we set the description, but
       * for root commands(command with subcommands), we want to prepend
       * the description to the existing summary string
       */
      if (context_summary == NULL)
        g_option_context_set_summary (context, invocation->command->description);
      else
        {
          /* TODO: remove this part once we deduplicate the ostree_option_context_new_with_commands
           * function from other root commands( command with subcommands). Because
           * we can directly add the summary inside the ostree_option_context_new_with_commands function.
           */
          g_autoptr(GString) new_summary_string = g_string_new (context_summary);

          g_string_prepend (new_summary_string, "\n\n");
          g_string_prepend (new_summary_string, invocation->command->description);

          g_option_context_set_summary (context, new_summary_string->str);
        }
    }
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
      /* This should now be YAML, like `docker version`, so it's both nice to read
       * possible to parse */
      g_auto(GStrv) features = g_strsplit (OSTREE_FEATURES, " ", -1);
      g_print ("%s:\n", PACKAGE_NAME);
      g_print (" Version: %s\n", PACKAGE_VERSION);
      if (strlen (OSTREE_GITREV) > 0)
        g_print (" Git: %s\n", OSTREE_GITREV);
#ifdef BUILDOPT_IS_DEVEL_BUILD
      g_print (" DevelBuild: yes\n");
#endif
      g_print (" Features:\n");
      for (char **iter = features; iter && *iter; iter++)
        g_print ("  - %s\n", *iter);
      exit (EXIT_SUCCESS);
    }

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  if (!(flags & OSTREE_BUILTIN_FLAG_NO_REPO))
    {
      repo = parse_repo_option (context, opt_repo, (flags & OSTREE_BUILTIN_FLAG_NO_CHECK) > 0,
                                cancellable, error);
      if (!repo)
        return FALSE;
    }

  if (out_repo)
    *out_repo = g_steal_pointer (&repo);

  return TRUE;
}

static void
on_sysroot_journal_msg (OstreeSysroot *sysroot,
                        const char    *msg,
                        void          *dummy)
{
  g_print ("%s\n", msg);
}

gboolean
ostree_admin_option_context_parse (GOptionContext *context,
                                   const GOptionEntry *main_entries,
                                   int *argc,
                                   char ***argv,
                                   OstreeAdminBuiltinFlags flags,
                                   OstreeCommandInvocation *invocation,
                                   OstreeSysroot **out_sysroot,
                                   GCancellable *cancellable,
                                   GError **error)
{
  /* Entries are listed in --help output in the order added.  We add the
   * main entries ourselves so that we can add the --sysroot entry first. */

  g_option_context_add_main_entries (context, global_admin_entries, NULL);

  if (!ostree_option_context_parse (context, main_entries, argc, argv,
                                    invocation, NULL, cancellable, error))
    return FALSE;

  if (!opt_print_current_dir && (flags & OSTREE_ADMIN_BUILTIN_FLAG_NO_SYSROOT))
    {
      g_assert_null (out_sysroot);
      /* Early return if no sysroot is requested */
      return TRUE;
    }

  g_autoptr(GFile) sysroot_path = NULL;
  if (opt_sysroot != NULL)
    sysroot_path = g_file_new_for_path (opt_sysroot);

  g_autoptr(OstreeSysroot) sysroot = ostree_sysroot_new (sysroot_path);
  g_signal_connect (sysroot, "journal-msg", G_CALLBACK (on_sysroot_journal_msg), NULL);

  if ((flags & OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED) == 0)
    {
      /* Released when sysroot is finalized, or on process exit */
      if (!ot_admin_sysroot_lock (sysroot, error))
        return FALSE;
    }

  if (!ostree_sysroot_load (sysroot, cancellable, error))
    return FALSE;

  if (flags & OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER)
    {
      OstreeDeployment *booted = ostree_sysroot_get_booted_deployment (sysroot);

      /* Only require root if we're manipulating a booted sysroot. (Mostly
       * useful for the test suite)
       */
      if (booted && getuid () != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                       "You must be root to perform this command");
          return FALSE;
        }
    }

  if (opt_print_current_dir)
    {
      g_autoptr(GPtrArray) deployments = NULL;
      OstreeDeployment *first_deployment;
      g_autoptr(GFile) deployment_file = NULL;
      g_autofree char *deployment_path = NULL;

      deployments = ostree_sysroot_get_deployments (sysroot);
      if (deployments->len == 0)
        return glnx_throw (error, "Unable to find a deployment in sysroot");
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

  if (out_sysroot)
    *out_sysroot = g_steal_pointer (&sysroot);

  return TRUE;
}

gboolean
ostree_ensure_repo_writable (OstreeRepo *repo,
                             GError **error)
{
  if (!ostree_repo_is_writable (repo, error))
    return glnx_prefix_error (error, "Cannot write to repository");
  return TRUE;
}

void
ostree_print_gpg_verify_result (OstreeGpgVerifyResult *result)
{
  guint n_sigs = ostree_gpg_verify_result_count_all (result);

  /* XXX If we ever add internationalization, use ngettext() here. */
  g_print ("GPG: Verification enabled, found %u signature%s:\n",
           n_sigs, n_sigs == 1 ? "" : "s");

  g_autoptr(GString) buffer = g_string_sized_new (256);

  for (guint ii = 0; ii < n_sigs; ii++)
    {
      g_string_append_c (buffer, '\n');
      ostree_gpg_verify_result_describe (result, ii, buffer, "  ",
                                         OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);
    }

  g_print ("%s", buffer->str);
}

gboolean
ot_enable_tombstone_commits (OstreeRepo *repo, GError **error)
{
  gboolean tombstone_commits = FALSE;
  GKeyFile *config = ostree_repo_get_config (repo);

  tombstone_commits = g_key_file_get_boolean (config, "core", "tombstone-commits", NULL);
  /* tombstone_commits is FALSE either if it is not found or it is really set to FALSE in the config file.  */
  if (!tombstone_commits)
    {
      g_key_file_set_boolean (config, "core", "tombstone-commits", TRUE);
      if (!ostree_repo_write_config (repo, config, error))
        return FALSE;
    }

  return TRUE;
}
