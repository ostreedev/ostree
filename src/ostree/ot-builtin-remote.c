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

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

static void
usage_error (GOptionContext *context, const char *message, GError **error)
{
  gs_free gchar *help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s", help);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
}

static gboolean
parse_keyvalue (const char  *keyvalue,
                char       **out_key,
                char       **out_value,
                GError     **error)
{
  const char *eq = strchr (keyvalue, '=');
  if (!eq)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Missing '=' in KEY=VALUE for --set");
      return FALSE;
    }
  *out_key = g_strndup (keyvalue, eq - keyvalue);
  *out_value = g_strdup (eq + 1);
  return TRUE;
}

static char **opt_set;
static gboolean opt_no_gpg_verify;

static GOptionEntry add_option_entries[] = {
  { "set", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_set, "Set config option KEY=VALUE for remote", "KEY=VALUE" },
  { "no-gpg-verify", 0, 0, G_OPTION_ARG_NONE, &opt_no_gpg_verify, "Disable GPG verification", NULL },
  { NULL }
};

static gboolean
ostree_remote_builtin_add (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;
  const char *remote_name;
  const char *remote_url;
  char **iter;
  gs_free char *target_name = NULL;
  gs_unref_object GFile *target_conf = NULL;
  gs_unref_variant_builder GVariantBuilder *optbuilder = NULL;
  gboolean ret = FALSE;

  context = g_option_context_new ("NAME URL [BRANCH...] - Add a remote repository");

  if (!ostree_option_context_parse (context, add_option_entries, &argc, &argv,
                                    OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc < 3)
    {
      usage_error (context, "NAME and URL must be specified", error);
      goto out;
    }

  remote_name = argv[1];
  remote_url  = argv[2];

  optbuilder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

  if (argc > 3)
    {
      gs_unref_ptrarray GPtrArray *branchesp = g_ptr_array_new ();
      int i;

      for (i = 3; i < argc; i++)
        g_ptr_array_add (branchesp, argv[i]);
      g_ptr_array_add (branchesp, NULL);

      g_variant_builder_add (optbuilder, "{s@v}",
                             "branches",
                             g_variant_new_variant (g_variant_new_strv ((const char*const*)branchesp->pdata, -1)));
    }

  for (iter = opt_set; iter && *iter; iter++)
    {
      const char *keyvalue = *iter;
      gs_free char *subkey = NULL;
      gs_free char *subvalue = NULL;

      if (!parse_keyvalue (keyvalue, &subkey, &subvalue, error))
        goto out;

      g_variant_builder_add (optbuilder, "{s@v}",
                             subkey, g_variant_new_variant (g_variant_new_string (subvalue)));
    }

  if (opt_no_gpg_verify)
    g_variant_builder_add (optbuilder, "{s@v}",
                           "gpg-verify",
                           g_variant_new_variant (g_variant_new_boolean (FALSE)));

  ret = ostree_repo_remote_add (repo, remote_name, remote_url,
                                g_variant_builder_end (optbuilder),
                                cancellable, error);

 out:
  g_option_context_free (context);

  return ret;
}

static GOptionEntry delete_option_entries[] = {
  { NULL }
};

static gboolean
ostree_remote_builtin_delete (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;
  const char *remote_name;
  gboolean ret = FALSE;

  context = g_option_context_new ("NAME - Delete a remote repository");

  if (!ostree_option_context_parse (context, delete_option_entries, &argc, &argv,
                                    OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "NAME must be specified", error);
      goto out;
    }

  remote_name = argv[1];

  ret = ostree_repo_remote_delete (repo, remote_name, cancellable, error);

 out:
  g_option_context_free (context);

  return ret;
}

static GOptionEntry show_url_option_entries[] = {
  { NULL }
};

static gboolean
ostree_remote_builtin_show_url (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;
  const char *remote_name;
  gs_free char *remote_url = NULL;
  gboolean ret = FALSE;

  context = g_option_context_new ("NAME - Show remote repository URL");

  if (!ostree_option_context_parse (context, show_url_option_entries, &argc, &argv,
                                    OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "NAME must be specified", error);
      goto out;
    }

  remote_name = argv[1];

  if (ostree_repo_remote_get_url (repo, remote_name, &remote_url, error))
    {
      g_print ("%s\n", remote_url);
      ret = TRUE;
    }

 out:
  return ret;
}

typedef struct {
  const char *name;
  gboolean (*fn) (int argc, char **argv, GCancellable *cancellable, GError **error);
} OstreeRemoteCommand;

static OstreeRemoteCommand remote_subcommands[] = {
  { "add", ostree_remote_builtin_add },
  { "delete", ostree_remote_builtin_delete },
  { "show-url", ostree_remote_builtin_show_url },
  { NULL, NULL }
};

static GOptionContext *
remote_option_context_new_with_commands (void)
{
  OstreeRemoteCommand *subcommand = remote_subcommands;
  GOptionContext *context;
  GString *summary;

  context = g_option_context_new ("COMMAND");

  summary = g_string_new ("Builtin \"remote\" Commands:");

  while (subcommand->name != NULL)
    {
      g_string_append_printf (summary, "\n  %s", subcommand->name);
      subcommand++;
    }

  g_option_context_set_summary (context, summary->str);

  g_string_free (summary, TRUE);

  return context;
}

gboolean
ostree_builtin_remote (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  OstreeRemoteCommand *subcommand;
  const char *subcommand_name = NULL;
  gs_free char *prgname = NULL;
  gboolean ret = FALSE;
  int in, out;

  for (in = 1, out = 1; in < argc; in++, out++)
    {
      /* The non-option is the command, take it out of the arguments */
      if (argv[in][0] != '-')
        {
          if (subcommand_name == NULL)
            {
              subcommand_name = argv[in];
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

  subcommand = remote_subcommands;
  while (subcommand->name)
    {
      if (g_strcmp0 (subcommand_name, subcommand->name) == 0)
        break;
      subcommand++;
    }

  if (!subcommand->name)
    {
      GOptionContext *context;
      gs_free char *help;

      context = remote_option_context_new_with_commands ();

      /* This will not return for some options (e.g. --version). */
      if (ostree_option_context_parse (context, NULL, &argc, &argv,
                                       OSTREE_BUILTIN_FLAG_NONE, NULL, cancellable, error))
        {
          if (subcommand_name == NULL)
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "No \"remote\" subcommand specified");
            }
          else
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unknown \"remote\" subcommand '%s'", subcommand_name);
            }
        }

      help = g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);

      g_option_context_free (context);

      goto out;
    }

  prgname = g_strdup_printf ("%s %s", g_get_prgname (), subcommand_name);
  g_set_prgname (prgname);

  if (!subcommand->fn (argc, argv, cancellable, error))
    goto out;

  ret = TRUE;

 out:
  return ret;
}

