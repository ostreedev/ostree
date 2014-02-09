/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "ot-main.h"
#include "otutil.h"

static char *opt_from_rev;
static char *opt_to_rev;
static char **opt_key_ids;
static char *opt_gpg_homedir;
static char *opt_max_usize;

#define BUILTINPROTO(name) static gboolean ot_static_delta_builtin_ ## name (int argc, char **argv, GCancellable *cancellable, GError **error)

BUILTINPROTO(list);
BUILTINPROTO(generate);
BUILTINPROTO(apply_offline);

#undef BUILTINPROTO

static OstreeCommand static_delta_subcommands[] = {
  { "list", ot_static_delta_builtin_list },
  { "generate", ot_static_delta_builtin_generate },
  { "apply-offline", ot_static_delta_builtin_apply_offline },
  { NULL, NULL }
};

static GOptionEntry generate_options[] = {
  { "from", 0, 0, G_OPTION_ARG_STRING, &opt_from_rev, "Create delta from revision REV", "REV" },
  { "to", 0, 0, G_OPTION_ARG_STRING, &opt_to_rev, "Create delta to revision REV", "REV" },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_key_ids, "GPG Key ID to sign the delta with", "key-id"},
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "homedir"},
  { "max-usize", 'u', 0, G_OPTION_ARG_STRING, &opt_max_usize, "Maximum uncompressed size in megabytes", NULL},
  { NULL }
};

static void
static_delta_usage (char    **argv,
                    gboolean  is_error)
{
  OstreeCommand *command = static_delta_subcommands;
  void (*print_func) (const gchar *format, ...);

  if (is_error)
    print_func = g_printerr;
  else
    print_func = g_print;

  print_func ("usage: %s --repo=PATH static-delta\n",
              argv[0]);
  print_func ("Builtin commands:\n");

  while (command->name)
    {
      print_func ("  %s\n", command->name);
      command++;
    }
}

static gboolean
ot_static_delta_builtin_list (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  gs_unref_ptrarray GPtrArray *delta_names = NULL;
  guint i;
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;

  context = g_option_context_new ("LIST - list delta files");
  if (!ostree_option_context_parse (context, NULL, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (!ostree_repo_list_static_delta_names (repo, &delta_names, cancellable, error))
    goto out;
      
  if (delta_names->len == 0)
    {
      g_print ("(No static deltas)\n");
    }
  else
    {
      for (i = 0; i < delta_names->len; i++)
        {
          g_print ("%s\n", (char*)delta_names->pdata[i]);
        }
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}

static gboolean
ot_static_delta_builtin_generate (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;

  context = g_option_context_new ("Generate static delta files");
  if (!ostree_option_context_parse (context, generate_options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc >= 2 && opt_to_rev == NULL)
    opt_to_rev = argv[1];

  if (argc < 2 && opt_to_rev == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "TO revision must be specified");
      goto out;
    }
  else
    {
      const char *from_source;
      gs_free char *from_resolved = NULL;
      gs_free char *to_resolved = NULL;
      gs_free char *from_parent_str = NULL;
      gs_unref_variant_builder GVariantBuilder *parambuilder = NULL;

      g_assert (opt_to_rev);

      if (opt_from_rev == NULL)
        {
          from_parent_str = g_strconcat (opt_to_rev, "^", NULL);
          from_source = from_parent_str;
        }
      else
        {
          from_source = opt_from_rev;
        }

      if (!ostree_repo_resolve_rev (repo, from_source, FALSE, &from_resolved, error))
        goto out;
      if (!ostree_repo_resolve_rev (repo, opt_to_rev, FALSE, &to_resolved, error))
        goto out;

      parambuilder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
      if (opt_max_usize)
        g_variant_builder_add (parambuilder, "{sv}",
                               "max-usize", g_variant_new_uint32 (g_ascii_strtoull (opt_max_usize, NULL, 10)));

      g_print ("Generating static delta:\n");
      g_print ("  From: %s\n", from_resolved);
      g_print ("  To:   %s\n", to_resolved);
      if (!ostree_repo_static_delta_generate (repo, OSTREE_STATIC_DELTA_GENERATE_OPT_MAJOR,
                                              from_resolved, to_resolved, NULL,
                                              g_variant_builder_end (parambuilder),
                                              cancellable, error))
        goto out;

      if (opt_key_ids)
        {
          char **iter;

          for (iter = opt_key_ids; iter && *iter; iter++)
            {
              const char *keyid = *iter;

              if (!ostree_repo_sign_delta (repo,
                                           from_resolved, to_resolved,
                                           keyid,
                                           opt_gpg_homedir,
                                           cancellable,
                                           error))
                goto out;
            }
        }
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}

static gboolean
ot_static_delta_builtin_apply_offline (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  const char *patharg;
  gs_unref_object GFile *path = NULL;
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;

  context = g_option_context_new ("DELTA - Apply delta file");
  if (!ostree_option_context_parse (context, NULL, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "PATH must be specified");
      goto out;
    }

  patharg = argv[1];
  path = g_file_new_for_path (patharg);

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  if (!ostree_repo_static_delta_execute_offline (repo, path, TRUE, cancellable, error))
    goto out;

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}

gboolean
ostree_builtin_static_delta (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  OstreeCommand *command;
  const char *cmdname;
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;

  context = g_option_context_new ("DELTA - Apply delta file");
  if (!ostree_option_context_parse (context, NULL, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      static_delta_usage (argv, TRUE);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "No command specified");
      goto out;
    }
  
  cmdname = argv[1];
  command = static_delta_subcommands;
  while (command->name)
    {
      if (g_strcmp0 (cmdname, command->name) == 0)
        break;
      command++;
    }

  if (!command->fn)
    {
      gs_free char *msg = g_strdup_printf ("Unknown command '%s'", cmdname);
      static_delta_usage (argv, TRUE);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, msg);
      goto out;
    }

  if (!command->fn (argc-1, argv+1, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
