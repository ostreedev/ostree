/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ostree-cmd-private.h"
#include "ostree.h"
#include "ot-builtins.h"
#include "ot-main.h"
#include "otutil.h"

static char *opt_from_rev;
static char *opt_to_rev;
static char *opt_min_fallback_size;
static char *opt_max_bsdiff_size;
static char *opt_max_chunk_size;
static char *opt_endianness;
static char *opt_filename;
static gboolean opt_empty;
static gboolean opt_swap_endianness;
static gboolean opt_inline;
static gboolean opt_disable_bsdiff;
static gboolean opt_if_not_exists;
static char **opt_key_ids;
static char *opt_sign_name;
static char *opt_keysfilename;
static char *opt_keysdir;

#define BUILTINPROTO(name) \
  static gboolean ot_static_delta_builtin_##name (int argc, char **argv, \
                                                  OstreeCommandInvocation *invocation, \
                                                  GCancellable *cancellable, GError **error)

BUILTINPROTO (list);
BUILTINPROTO (show);
BUILTINPROTO (delete);
BUILTINPROTO (generate);
BUILTINPROTO (apply_offline);
BUILTINPROTO (verify);
BUILTINPROTO (indexes);
BUILTINPROTO (reindex);

#undef BUILTINPROTO

static OstreeCommand static_delta_subcommands[] = {
  { "list", OSTREE_BUILTIN_FLAG_NONE, ot_static_delta_builtin_list, "List static delta files" },
  { "show", OSTREE_BUILTIN_FLAG_NONE, ot_static_delta_builtin_show, "Dump information on a delta" },
  { "delete", OSTREE_BUILTIN_FLAG_NONE, ot_static_delta_builtin_delete, "Remove a delta" },
  { "generate", OSTREE_BUILTIN_FLAG_NONE, ot_static_delta_builtin_generate,
    "Generate static delta files" },
  { "apply-offline", OSTREE_BUILTIN_FLAG_NONE, ot_static_delta_builtin_apply_offline,
    "Apply static delta file" },
  { "verify", OSTREE_BUILTIN_FLAG_NONE, ot_static_delta_builtin_verify,
    "Verify static delta signatures" },
  { "indexes", OSTREE_BUILTIN_FLAG_NONE, ot_static_delta_builtin_indexes,
    "List static delta indexes" },
  { "reindex", OSTREE_BUILTIN_FLAG_NONE, ot_static_delta_builtin_reindex,
    "Regenerate static delta indexes" },
  { NULL, 0, NULL, NULL }
};

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-static-delta.xml) when changing the option list(s).
 */

static GOptionEntry generate_options[] = {
  { "from", 0, 0, G_OPTION_ARG_STRING, &opt_from_rev, "Create delta from revision REV", "REV" },
  { "empty", 0, 0, G_OPTION_ARG_NONE, &opt_empty, "Create delta from scratch", NULL },
  { "inline", 0, 0, G_OPTION_ARG_NONE, &opt_inline, "Inline delta parts into main delta", NULL },
  { "to", 0, 0, G_OPTION_ARG_STRING, &opt_to_rev, "Create delta to revision REV", "REV" },
  { "disable-bsdiff", 0, 0, G_OPTION_ARG_NONE, &opt_disable_bsdiff, "Disable use of bsdiff", NULL },
  { "if-not-exists", 'n', 0, G_OPTION_ARG_NONE, &opt_if_not_exists,
    "Only generate if a delta does not already exist", NULL },
  { "set-endianness", 0, 0, G_OPTION_ARG_STRING, &opt_endianness,
    "Choose metadata endianness ('l' or 'B')", "ENDIAN" },
  { "swap-endianness", 0, 0, G_OPTION_ARG_NONE, &opt_swap_endianness,
    "Swap metadata endianness from host order", NULL },
  { "min-fallback-size", 0, 0, G_OPTION_ARG_STRING, &opt_min_fallback_size,
    "Minimum uncompressed size in megabytes for individual HTTP request", NULL },
  { "max-bsdiff-size", 0, 0, G_OPTION_ARG_STRING, &opt_max_bsdiff_size,
    "Maximum size in megabytes to consider bsdiff compression for input files", NULL },
  { "max-chunk-size", 0, 0, G_OPTION_ARG_STRING, &opt_max_chunk_size,
    "Maximum size of delta chunks in megabytes", NULL },
  { "filename", 0, 0, G_OPTION_ARG_FILENAME, &opt_filename,
    "Write the delta content to PATH (a directory).  If not specified, the OSTree repository is "
    "used",
    "PATH" },
  { "sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_key_ids, "Sign the delta with", "KEY_ID" },
  { "sign-type", 0, 0, G_OPTION_ARG_STRING, &opt_sign_name,
    "Signature type to use (defaults to 'ed25519')", "NAME" },
#if defined(HAVE_LIBSODIUM)
  { "keys-file", 0, 0, G_OPTION_ARG_STRING, &opt_keysfilename, "Read key(s) from file", "NAME" },
#endif
  { NULL }
};

static GOptionEntry apply_offline_options[] = {
  { "sign-type", 0, 0, G_OPTION_ARG_STRING, &opt_sign_name,
    "Signature type to use (defaults to 'ed25519')", "NAME" },
#if defined(HAVE_LIBSODIUM)
  { "keys-file", 0, 0, G_OPTION_ARG_STRING, &opt_keysfilename, "Read key(s) from file", "NAME" },
  { "keys-dir", 0, 0, G_OPTION_ARG_STRING, &opt_keysdir,
    "Redefine system-wide directories with public and revoked keys for verification", "NAME" },
#endif
  { NULL }
};

static GOptionEntry list_options[] = { { NULL } };

static GOptionEntry verify_options[] = {
  { "sign-type", 0, 0, G_OPTION_ARG_STRING, &opt_sign_name,
    "Signature type to use (defaults to 'ed25519')", "NAME" },
#if defined(HAVE_LIBSODIUM)
  { "keys-file", 0, 0, G_OPTION_ARG_STRING, &opt_keysfilename, "Read key(s) from file", "NAME" },
  { "keys-dir", 0, 0, G_OPTION_ARG_STRING, &opt_keysdir,
    "Redefine system-wide directories with public and revoked keys for verification", "NAME" },
#endif
  { NULL }
};

static GOptionEntry indexes_options[] = { { NULL } };

static GOptionEntry reindex_options[] = { { "to", 0, 0, G_OPTION_ARG_STRING, &opt_to_rev,
                                            "Only update delta index to revision REV", "REV" },
                                          { NULL } };

static void
static_delta_usage (char **argv, gboolean is_error)
{
  OstreeCommand *command = static_delta_subcommands;
  void (*print_func) (const gchar *format, ...);

  if (is_error)
    print_func = g_printerr;
  else
    print_func = g_print;

  print_func ("Usage:\n");
  print_func ("  ostree static-delta [OPTION...] COMMAND\n\n");
  print_func ("Builtin \"static-delta\" Commands:\n");

  while (command->name)
    {
      if ((command->flags & OSTREE_BUILTIN_FLAG_HIDDEN) == 0)
        print_func ("  %-17s%s\n", command->name, command->description ?: "");
      command++;
    }

  print_func ("\n");
}

static gboolean
ot_static_delta_builtin_list (int argc, char **argv, OstreeCommandInvocation *invocation,
                              GCancellable *cancellable, GError **error)
{
  g_autoptr (OstreeRepo) repo = NULL;
  g_autoptr (GOptionContext) context = g_option_context_new ("");
  if (!ostree_option_context_parse (context, list_options, &argc, &argv, invocation, &repo,
                                    cancellable, error))
    return FALSE;

  g_autoptr (GPtrArray) delta_names = NULL;
  if (!ostree_repo_list_static_delta_names (repo, &delta_names, cancellable, error))
    return FALSE;

  if (delta_names->len == 0)
    g_print ("(No static deltas)\n");
  else
    {
      for (guint i = 0; i < delta_names->len; i++)
        g_print ("%s\n", (char *)delta_names->pdata[i]);
    }

  return TRUE;
}

static gboolean
ot_static_delta_builtin_indexes (int argc, char **argv, OstreeCommandInvocation *invocation,
                                 GCancellable *cancellable, GError **error)
{
  g_autoptr (OstreeRepo) repo = NULL;
  g_autoptr (GOptionContext) context = g_option_context_new ("");
  if (!ostree_option_context_parse (context, indexes_options, &argc, &argv, invocation, &repo,
                                    cancellable, error))
    return FALSE;

  g_autoptr (GPtrArray) indexes = NULL;
  if (!ostree_repo_list_static_delta_indexes (repo, &indexes, cancellable, error))
    return FALSE;

  if (indexes->len == 0)
    g_print ("(No static deltas indexes)\n");
  else
    {
      for (guint i = 0; i < indexes->len; i++)
        g_print ("%s\n", (char *)indexes->pdata[i]);
    }

  return TRUE;
}

static gboolean
ot_static_delta_builtin_reindex (int argc, char **argv, OstreeCommandInvocation *invocation,
                                 GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("");

  g_autoptr (OstreeRepo) repo = NULL;
  if (!ostree_option_context_parse (context, reindex_options, &argc, &argv, invocation, &repo,
                                    cancellable, error))
    return FALSE;

  if (!ostree_repo_static_delta_reindex (repo, 0, opt_to_rev, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
ot_static_delta_builtin_show (int argc, char **argv, OstreeCommandInvocation *invocation,
                              GCancellable *cancellable, GError **error)
{

  g_autoptr (GOptionContext) context = g_option_context_new ("");

  g_autoptr (OstreeRepo) repo = NULL;
  if (!ostree_option_context_parse (context, list_options, &argc, &argv, invocation, &repo,
                                    cancellable, error))
    return FALSE;

  if (argc < 3)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "DELTA must be specified");
      return FALSE;
    }

  const char *delta_id = argv[2];

  if (!ostree_cmd__private__ ()->ostree_static_delta_dump (repo, delta_id, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
ot_static_delta_builtin_delete (int argc, char **argv, OstreeCommandInvocation *invocation,
                                GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("");

  g_autoptr (OstreeRepo) repo = NULL;
  if (!ostree_option_context_parse (context, list_options, &argc, &argv, invocation, &repo,
                                    cancellable, error))
    return FALSE;

  if (argc < 3)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "DELTA must be specified");
      return FALSE;
    }

  const char *delta_id = argv[2];

  if (!ostree_cmd__private__ ()->ostree_static_delta_delete (repo, delta_id, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
ot_static_delta_builtin_generate (int argc, char **argv, OstreeCommandInvocation *invocation,
                                  GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("[TO]");
  g_autoptr (OstreeRepo) repo = NULL;
  if (!ostree_option_context_parse (context, generate_options, &argc, &argv, invocation, &repo,
                                    cancellable, error))
    return FALSE;

  if (!ostree_ensure_repo_writable (repo, error))
    return FALSE;

  if (argc >= 3 && opt_to_rev == NULL)
    opt_to_rev = argv[2];

  if (argc < 3 && opt_to_rev == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "TO revision must be specified");
      return FALSE;
    }
  else
    {
      const char *from_source;
      g_autofree char *from_resolved = NULL;
      g_autofree char *to_resolved = NULL;
      g_autofree char *from_parent_str = NULL;
      g_autoptr (GVariantBuilder) parambuilder = NULL;
      int endianness;

      g_assert (opt_to_rev);

      if (opt_empty)
        {
          if (opt_from_rev)
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Cannot specify both --empty and --from=REV");
              return FALSE;
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
            return FALSE;
        }
      if (!ostree_repo_resolve_rev (repo, opt_to_rev, FALSE, &to_resolved, error))
        return FALSE;

      if (opt_if_not_exists)
        {
          gboolean does_exist;
          g_autofree char *delta_id = from_resolved
                                          ? g_strconcat (from_resolved, "-", to_resolved, NULL)
                                          : g_strdup (to_resolved);
          if (!ostree_cmd__private__ ()->ostree_static_delta_query_exists (
                  repo, delta_id, &does_exist, cancellable, error))
            return FALSE;
          if (does_exist)
            {
              g_print ("Delta %s already exists.\n", delta_id);
              return TRUE;
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
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid endianness '%s'",
                           opt_endianness);
              return FALSE;
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
        g_variant_builder_add (
            parambuilder, "{sv}", "min-fallback-size",
            g_variant_new_uint32 (g_ascii_strtoull (opt_min_fallback_size, NULL, 10)));
      if (opt_max_bsdiff_size)
        g_variant_builder_add (
            parambuilder, "{sv}", "max-bsdiff-size",
            g_variant_new_uint32 (g_ascii_strtoull (opt_max_bsdiff_size, NULL, 10)));
      if (opt_max_chunk_size)
        g_variant_builder_add (
            parambuilder, "{sv}", "max-chunk-size",
            g_variant_new_uint32 (g_ascii_strtoull (opt_max_chunk_size, NULL, 10)));
      if (opt_disable_bsdiff)
        g_variant_builder_add (parambuilder, "{sv}", "bsdiff-enabled",
                               g_variant_new_boolean (FALSE));
      if (opt_inline)
        g_variant_builder_add (parambuilder, "{sv}", "inline-parts", g_variant_new_boolean (TRUE));
      if (opt_filename)
        g_variant_builder_add (parambuilder, "{sv}", "filename",
                               g_variant_new_bytestring (opt_filename));

      g_variant_builder_add (parambuilder, "{sv}", "verbose", g_variant_new_boolean (TRUE));
      if (opt_endianness || opt_swap_endianness)
        g_variant_builder_add (parambuilder, "{sv}", "endianness",
                               g_variant_new_uint32 (endianness));

      if (opt_key_ids || opt_keysfilename)
        {
          g_autoptr (GPtrArray) key_ids = g_ptr_array_new ();

          for (char **iter = opt_key_ids; iter != NULL && *iter != NULL; ++iter)
            g_ptr_array_add (key_ids, *iter);

          if (opt_keysfilename)
            {
              g_autoptr (GFile) keyfile = NULL;
              g_autoptr (GFileInputStream) key_stream_in = NULL;
              g_autoptr (GDataInputStream) key_data_in = NULL;

              if (!g_file_test (opt_keysfilename, G_FILE_TEST_IS_REGULAR))
                {
                  g_warning ("Can't open file '%s' with keys", opt_keysfilename);
                  return glnx_throw (error, "File object '%s' is not a regular file",
                                     opt_keysfilename);
                }

              keyfile = g_file_new_for_path (opt_keysfilename);
              key_stream_in = g_file_read (keyfile, NULL, error);
              if (key_stream_in == NULL)
                return FALSE;

              key_data_in = g_data_input_stream_new (G_INPUT_STREAM (key_stream_in));
              g_assert (key_data_in != NULL);

              /* Use simple file format with just a list of base64 public keys per line */
              while (TRUE)
                {
                  gsize len = 0;
                  g_autofree char *line
                      = g_data_input_stream_read_line (key_data_in, &len, NULL, error);
                  g_autoptr (GVariant) sk = NULL;

                  if (*error != NULL)
                    return FALSE;

                  if (line == NULL)
                    break;

                  // Pass the key as a string
                  g_ptr_array_add (key_ids, g_strdup (line));
                }
            }

          g_autoptr (GVariant) key_ids_v
              = g_variant_new_strv ((const char *const *)key_ids->pdata, key_ids->len);
          g_variant_builder_add (parambuilder, "{s@v}", "sign-key-ids",
                                 g_variant_new_variant (g_steal_pointer (&key_ids_v)));
        }
      opt_sign_name = opt_sign_name ?: OSTREE_SIGN_NAME_ED25519;
      g_variant_builder_add (parambuilder, "{sv}", "sign-name",
                             g_variant_new_bytestring (opt_sign_name));

      g_print ("Generating static delta:\n");
      g_print ("  From: %s\n", from_resolved ? from_resolved : "empty");
      g_print ("  To:   %s\n", to_resolved);
      {
        g_autoptr (GVariant) params = g_variant_ref_sink (g_variant_builder_end (parambuilder));
        if (!ostree_repo_static_delta_generate (repo, OSTREE_STATIC_DELTA_GENERATE_OPT_MAJOR,
                                                from_resolved, to_resolved, NULL, params,
                                                cancellable, error))
          return FALSE;
      }
    }

  return TRUE;
}

static gboolean
ot_static_delta_builtin_apply_offline (int argc, char **argv, OstreeCommandInvocation *invocation,
                                       GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = NULL;
  g_autoptr (OstreeRepo) repo = NULL;
  g_autoptr (OstreeSign) sign = NULL;
  char **key_ids;
  int n_key_ids;

  context = g_option_context_new ("");
  if (!ostree_option_context_parse (context, apply_offline_options, &argc, &argv, invocation, &repo,
                                    cancellable, error))
    return FALSE;

  if (!ostree_ensure_repo_writable (repo, error))
    return FALSE;

  if (argc < 3)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "PATH must be specified");
      return FALSE;
    }

#if defined(HAVE_LIBSODIUM)
  /* Initialize crypto system */
  opt_sign_name = opt_sign_name ?: OSTREE_SIGN_NAME_ED25519;
#endif

  if (opt_sign_name)
    {
      sign = ostree_sign_get_by_name (opt_sign_name, error);
      if (!sign)
        return glnx_throw (error, "Signing type %s is not supported", opt_sign_name);

      key_ids = argv + 3;
      n_key_ids = argc - 3;
      for (int i = 0; i < n_key_ids; i++)
        {
          g_autoptr (GVariant) pk = g_variant_new_string (key_ids[i]);
          if (!ostree_sign_add_pk (sign, pk, error))
            return FALSE;
        }
      if ((n_key_ids == 0) || opt_keysfilename)
        {
          g_autoptr (GVariantBuilder) builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
          g_autoptr (GVariant) options = NULL;

          /* Use custom directory with public and revoked keys instead of system-wide directories
           */
          if (opt_keysdir)
            g_variant_builder_add (builder, "{sv}", "basedir", g_variant_new_string (opt_keysdir));
          /* The last chance for verification source -- system files */
          if (opt_keysfilename)
            g_variant_builder_add (builder, "{sv}", "filename",
                                   g_variant_new_string (opt_keysfilename));
          options = g_variant_builder_end (builder);

          if (!ostree_sign_load_pk (sign, options, error))
            {
              /* If it fails to load system default public keys, consider there no signature engine
               */
              if (!opt_keysdir && !opt_keysfilename)
                {
                  g_clear_error (error);
                  g_clear_object (&sign);
                }
              else
                return FALSE;
            }
        }
    }

  const char *patharg = argv[2];
  g_autoptr (GFile) path = g_file_new_for_path (patharg);

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return FALSE;

  if (!ostree_repo_static_delta_execute_offline_with_signature (repo, path, sign, FALSE,
                                                                cancellable, error))
    return FALSE;

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
ot_static_delta_builtin_verify (int argc, char **argv, OstreeCommandInvocation *invocation,
                                GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("STATIC-DELTA-FILE [KEY-ID...]");
  g_autoptr (OstreeRepo) repo = NULL;
  gboolean verified;
  char **key_ids;
  int n_key_ids;

  if (!ostree_option_context_parse (context, verify_options, &argc, &argv, invocation, &repo,
                                    cancellable, error))
    return FALSE;

  if (argc < 3)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "DELTA must be specified");
      return FALSE;
    }

  opt_sign_name = opt_sign_name ?: OSTREE_SIGN_NAME_ED25519;

  const char *delta_id = argv[2];

  g_autoptr (OstreeSign) sign = ostree_sign_get_by_name (opt_sign_name, error);
  if (!sign)
    {
      g_print ("Sign-type not supported\n");
      return FALSE;
    }

  key_ids = argv + 3;
  n_key_ids = argc - 3;
  for (int i = 0; i < n_key_ids; i++)
    {
      g_autoptr (GVariant) pk = g_variant_new_string (key_ids[i]);
      if (!ostree_sign_add_pk (sign, pk, error))
        return FALSE;
    }
  if ((n_key_ids == 0) || opt_keysfilename)
    {
      g_autoptr (GVariantBuilder) builder = NULL;
      g_autoptr (GVariant) options = NULL;

      builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
      /* Use custom directory with public and revoked keys instead of system-wide directories */
      if (opt_keysdir)
        g_variant_builder_add (builder, "{sv}", "basedir", g_variant_new_string (opt_keysdir));
      /* The last chance for verification source -- system files */
      if (opt_keysfilename)
        g_variant_builder_add (builder, "{sv}", "filename",
                               g_variant_new_string (opt_keysfilename));
      options = g_variant_builder_end (builder);

      if (!ostree_sign_load_pk (sign, options, error))
        return FALSE;
    }

  verified = ostree_repo_static_delta_verify_signature (repo, delta_id, sign, NULL, error);
  g_print ("Verification %s\n", verified ? "OK" : "fails");

  return verified;
}

gboolean
ostree_builtin_static_delta (int argc, char **argv, OstreeCommandInvocation *invocation,
                             GCancellable *cancellable, GError **error)
{
  gboolean want_help = FALSE;
  const char *cmdname = NULL;
  for (int i = 1; i < argc; i++)
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
      return glnx_throw (error, "No command specified");
    }

  OstreeCommand *command = NULL;
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
      return TRUE; /* Note early return */
    }

  if (!command || !command->fn)
    {
      static_delta_usage (argv, TRUE);
      return glnx_throw (error, "Unknown \"static-delta\" subcommand '%s'", cmdname);
    }

  g_autofree char *prgname = g_strdup_printf ("%s %s", g_get_prgname (), cmdname);
  g_set_prgname (prgname);

  OstreeCommandInvocation sub_invocation = { .command = command };
  return command->fn (argc, argv, &sub_invocation, cancellable, error);
}
