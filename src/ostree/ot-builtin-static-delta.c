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
#include "ostree-cmdprivate.h"
#include "ot-main.h"
#include "otutil.h"

static char *opt_from_rev;
static char *opt_to_rev;
static char *opt_min_fallback_size;
static char *opt_max_bsdiff_size;
static char *opt_max_chunk_size;
static char *opt_endianness;
static gboolean opt_empty;
static gboolean opt_swap_endianness;
static gboolean opt_inline;
static gboolean opt_disable_bsdiff;
static gboolean opt_if_not_exists;

#define BUILTINPROTO(name) static gboolean ot_static_delta_builtin_ ## name (int argc, char **argv, GCancellable *cancellable, GError **error)

BUILTINPROTO(list);
BUILTINPROTO(show);
BUILTINPROTO(delete);
BUILTINPROTO(generate);
BUILTINPROTO(apply_offline);

#undef BUILTINPROTO

static OstreeCommand static_delta_subcommands[] = {
  { "list", ot_static_delta_builtin_list },
  { "show", ot_static_delta_builtin_show },
  { "delete", ot_static_delta_builtin_delete },
  { "generate", ot_static_delta_builtin_generate },
  { "apply-offline", ot_static_delta_builtin_apply_offline },
  { NULL, NULL }
};


static GOptionEntry generate_options[] = {
  { "from", 0, 0, G_OPTION_ARG_STRING, &opt_from_rev, "Create delta from revision REV", "REV" },
  { "empty", 0, 0, G_OPTION_ARG_NONE, &opt_empty, "Create delta from scratch", NULL },
  { "inline", 0, 0, G_OPTION_ARG_NONE, &opt_inline, "Inline delta parts into main delta", NULL },
  { "to", 0, 0, G_OPTION_ARG_STRING, &opt_to_rev, "Create delta to revision REV", "REV" },
  { "disable-bsdiff", 0, 0, G_OPTION_ARG_NONE, &opt_disable_bsdiff, "Disable use of bsdiff", NULL },
  { "if-not-exists", 'n', 0, G_OPTION_ARG_NONE, &opt_if_not_exists, "Only generate if a delta does not already exist", NULL },
  { "set-endianness", 0, 0, G_OPTION_ARG_STRING, &opt_endianness, "Choose metadata endianness ('l' or 'B')", "ENDIAN" },
  { "swap-endianness", 0, 0, G_OPTION_ARG_NONE, &opt_swap_endianness, "Swap metadata endianness from host order", NULL },
  { "min-fallback-size", 0, 0, G_OPTION_ARG_STRING, &opt_min_fallback_size, "Minimum uncompressed size in megabytes for individual HTTP request", NULL},
  { "max-bsdiff-size", 0, 0, G_OPTION_ARG_STRING, &opt_max_bsdiff_size, "Maximum size in megabytes to consider bsdiff compression for input files", NULL},
  { "max-chunk-size", 0, 0, G_OPTION_ARG_STRING, &opt_max_chunk_size, "Maximum size of delta chunks in megabytes", NULL},
  { NULL }
};

static GOptionEntry apply_offline_options[] = {
  { NULL }
};

static GOptionEntry list_options[] = {
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

  print_func ("usage: ostree static-delta\n");
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
  g_autoptr(GPtrArray) delta_names = NULL;
  guint i;
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;

  context = g_option_context_new ("LIST - list static delta files");

  if (!ostree_option_context_parse (context, list_options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
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
  return ret;
}

static gboolean
ot_static_delta_builtin_show (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  const char *delta_id = NULL;

  context = g_option_context_new ("SHOW - Dump information on a delta");

  if (!ostree_option_context_parse (context, list_options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc < 3)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "DELTA must be specified");
      goto out;
    }

  delta_id = argv[2];

  if (!ostree_cmd__private__ ()->ostree_static_delta_dump (repo, delta_id, cancellable, error))
    goto out;
      
  ret = TRUE;
 out:
  return ret;
}

static gboolean
ot_static_delta_builtin_delete (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  const char *delta_id = NULL;

  context = g_option_context_new ("DELETE - Remove a delta");

  if (!ostree_option_context_parse (context, list_options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc < 3)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "DELTA must be specified");
      goto out;
    }

  delta_id = argv[2];

  if (!ostree_cmd__private__ ()->ostree_static_delta_delete (repo, delta_id, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}


static gboolean
ot_static_delta_builtin_generate (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;

  context = g_option_context_new ("GENERATE [TO] - Generate static delta files");
  if (!ostree_option_context_parse (context, generate_options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (!ostree_ensure_repo_writable (repo, error))
    goto out;

  if (argc >= 3 && opt_to_rev == NULL)
    opt_to_rev = argv[2];

  if (argc < 3 && opt_to_rev == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "TO revision must be specified");
      goto out;
    }
  else
    {
      const char *from_source;
      g_autofree char *from_resolved = NULL;
      g_autofree char *to_resolved = NULL;
      g_autofree char *from_parent_str = NULL;
      g_autoptr(GVariantBuilder) parambuilder = NULL;
      int endianness;

      g_assert (opt_to_rev);

      if (opt_empty)
        {
          if (opt_from_rev)
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Cannot specify both --empty and --from=REV");
              goto out;
            }
          from_source = NULL;
        }
      else if (opt_from_rev == NULL)
        {
          from_parent_str = g_strconcat (opt_to_rev, "^", NULL);
          from_source = from_parent_str;
        }
      else
        {
          from_source = opt_from_rev;
        }

      if (from_source)
        {
          if (!ostree_repo_resolve_rev (repo, from_source, FALSE, &from_resolved, error))
            goto out;
        }
      if (!ostree_repo_resolve_rev (repo, opt_to_rev, FALSE, &to_resolved, error))
        goto out;

      if (opt_if_not_exists)
        {
          gboolean does_exist;
          g_autofree char *delta_id = from_resolved ? g_strconcat (from_resolved, "-", to_resolved, NULL) : g_strdup (to_resolved);
          if (!ostree_cmd__private__ ()->ostree_static_delta_query_exists (repo, delta_id, &does_exist, cancellable, error))
            goto out;
          if (does_exist)
            {
              g_print ("Delta %s already exists.\n", delta_id);
              ret = TRUE;
              goto out;
            }
        }
      
      if (opt_endianness)
        {
          if (strcmp (opt_endianness, "l") == 0)
            endianness = G_LITTLE_ENDIAN;
          else if (strcmp (opt_endianness, "B") == 0)
            endianness = G_BIG_ENDIAN;
          else
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid endianness '%s'", opt_endianness);
              goto out;
            }
        }
      else
        endianness = G_BYTE_ORDER;
          
      if (opt_swap_endianness)
        {
          switch (endianness)
            {
            case G_LITTLE_ENDIAN:
              endianness = G_BIG_ENDIAN;
              break;
            case G_BIG_ENDIAN:
              endianness = G_LITTLE_ENDIAN;
              break;
            default:
              g_assert_not_reached ();
            }
        }

      parambuilder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
      if (opt_min_fallback_size)
        g_variant_builder_add (parambuilder, "{sv}",
                               "min-fallback-size", g_variant_new_uint32 (g_ascii_strtoull (opt_min_fallback_size, NULL, 10)));
      if (opt_max_bsdiff_size)
        g_variant_builder_add (parambuilder, "{sv}",
                               "max-bsdiff-size", g_variant_new_uint32 (g_ascii_strtoull (opt_max_bsdiff_size, NULL, 10)));
      if (opt_max_chunk_size)
        g_variant_builder_add (parambuilder, "{sv}",
                               "max-chunk-size", g_variant_new_uint32 (g_ascii_strtoull (opt_max_chunk_size, NULL, 10)));
      if (opt_disable_bsdiff)
        g_variant_builder_add (parambuilder, "{sv}",
                               "bsdiff-enabled", g_variant_new_boolean (FALSE));
      if (opt_inline)
        g_variant_builder_add (parambuilder, "{sv}",
                               "inline-parts", g_variant_new_boolean (TRUE));

      g_variant_builder_add (parambuilder, "{sv}", "verbose", g_variant_new_boolean (TRUE));
      if (opt_endianness || opt_swap_endianness)
        g_variant_builder_add (parambuilder, "{sv}", "endianness", g_variant_new_uint32 (endianness));

      g_print ("Generating static delta:\n");
      g_print ("  From: %s\n", from_resolved ? from_resolved : "empty");
      g_print ("  To:   %s\n", to_resolved);
      { g_autoptr(GVariant) params = g_variant_ref_sink (g_variant_builder_end (parambuilder));
        if (!ostree_repo_static_delta_generate (repo, OSTREE_STATIC_DELTA_GENERATE_OPT_MAJOR,
                                                from_resolved, to_resolved, NULL,
                                                params,
                                                cancellable, error))
          goto out;
      }

    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
ot_static_delta_builtin_apply_offline (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  const char *patharg;
  g_autoptr(GFile) path = NULL;
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;

  context = g_option_context_new ("APPLY-OFFLINE - Apply static delta file");
  if (!ostree_option_context_parse (context, apply_offline_options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (!ostree_ensure_repo_writable (repo, error))
    goto out;

  if (argc < 3)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "PATH must be specified");
      goto out;
    }

  patharg = argv[2];
  path = g_file_new_for_path (patharg);

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  if (!ostree_repo_static_delta_execute_offline (repo, path, FALSE, cancellable, error))
    goto out;

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_builtin_static_delta (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  OstreeCommand *command = NULL;
  const char *cmdname = NULL;
  int i;
  gboolean want_help = FALSE;

  for (i = 1; i < argc; i++)
    {
      if (argv[i][0] != '-')
        {
          cmdname = argv[i];
          break;
        }
      else if (g_str_equal (argv[i], "--help") || g_str_equal (argv[i], "-h"))
        {
          want_help = TRUE;
          break;
        }
    }

  if (!cmdname && !want_help)
    {
      static_delta_usage (argv, TRUE);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "No command specified");
      goto out;
    }

  if (cmdname)
    {
      command = static_delta_subcommands;
      while (command->name)
        {
          if (g_strcmp0 (cmdname, command->name) == 0)
            break;
          command++;
        }
    }

  if (want_help && command == NULL)
    {
      static_delta_usage (argv, FALSE);
      ret = TRUE;
      goto out;
    }

  if (!command->fn)
    {
      g_autofree char *msg = g_strdup_printf ("Unknown command '%s'", cmdname);
      static_delta_usage (argv, TRUE);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, msg);
      goto out;
    }

  if (!command->fn (argc, argv, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
