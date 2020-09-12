/* vim:set et sw=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e2s: */

/*
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 * Copyright (C) 2019 Denis Pynkin (d4s) <denis.pynkin@collabora.com>
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

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"
#include "ostree-core-private.h"
#include "ostree-sign.h"

static gboolean opt_delete;
static gboolean opt_verify;
static char *opt_sign_name;
static char *opt_filename;
static char *opt_keysdir;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-sign.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "delete", 'd', 0, G_OPTION_ARG_NONE, &opt_delete, "Delete signatures having any of the KEY-IDs", NULL},
  { "verify", 0, 0, G_OPTION_ARG_NONE, &opt_verify, "Verify signatures", NULL},
  { "sign-type", 's', 0, G_OPTION_ARG_STRING, &opt_sign_name, "Signature type to use (defaults to 'ed25519')", "NAME"},
#if defined(HAVE_LIBSODIUM)
  { "keys-file", 0, 0, G_OPTION_ARG_STRING, &opt_filename, "Read key(s) from file", "NAME"},
  { "keys-dir", 0, 0, G_OPTION_ARG_STRING, &opt_keysdir, "Redefine system-wide directories with public and revoked keys for verification", "NAME"},
#endif
  { NULL }
};

static void
usage_error (GOptionContext *context, const char *message, GError **error)
{
  g_autofree char *help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s", help);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
}

gboolean
ostree_builtin_sign (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = NULL;
  g_autoptr (OstreeRepo) repo = NULL;
  g_autoptr (OstreeSign) sign = NULL;
  g_autofree char *resolved_commit = NULL;
  g_autofree char *success_message = NULL;
  const char *commit;
  char **key_ids;
  int n_key_ids, ii;
  gboolean ret = FALSE;

  context = g_option_context_new ("COMMIT KEY-ID...");


  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "Need a COMMIT to sign or verify", error);
      goto out;
    }

  commit = argv[1];

  /* Verification could be done via system files with public keys */
  if (!opt_verify &&
      !opt_filename &&
      argc < 3)
    {
      usage_error (context, "Need at least one KEY-ID to sign with", error);
      goto out;
    }

  key_ids = argv + 2;
  n_key_ids = argc - 2;

  if (!ostree_repo_resolve_rev (repo, commit, FALSE, &resolved_commit, error))
    goto out;

  /* Initialize crypto system */
  opt_sign_name = opt_sign_name ?: OSTREE_SIGN_NAME_ED25519;

  sign = ostree_sign_get_by_name (opt_sign_name, error);
  if (sign == NULL)
    goto out;

  for (ii = 0; ii < n_key_ids; ii++)
    {
      g_autoptr (GVariant) sk = NULL;
      g_autoptr (GVariant) pk = NULL;

      if (opt_verify)
        {
          g_autoptr (GError) local_error = NULL;


          // Pass the key as a string
          pk = g_variant_new_string(key_ids[ii]);

          if (!ostree_sign_set_pk (sign, pk, &local_error))
            continue;

          if (ostree_sign_commit_verify (sign,
                                         repo,
                                         resolved_commit,
                                         &success_message,
                                         cancellable,
                                         &local_error))
            {
              g_assert (success_message);
              g_print ("%s\n", success_message);
              ret = TRUE;
              goto out;
            }
        }
      else
        {
          // Pass the key as a string
          sk = g_variant_new_string(key_ids[ii]);
          if (!ostree_sign_set_sk (sign, sk, error))
            {
              ret = FALSE;
              goto out;
            }

          ret = ostree_sign_commit (sign,
                                    repo,
                                    resolved_commit,
                                    cancellable,
                                    error);
          if (ret != TRUE)
            goto out;
        }
    }

  /* Try to verify with user-provided file or system configuration */
  if (opt_verify)
    {
      if ((n_key_ids == 0) || opt_filename)
        {
          g_autoptr (GVariantBuilder) builder = NULL;
          g_autoptr (GVariant) sign_options = NULL;

          builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
          /* Use custom directory with public and revoked keys instead of system-wide directories */
          if (opt_keysdir)
            g_variant_builder_add (builder, "{sv}", "basedir", g_variant_new_string (opt_keysdir));
          /* The last chance for verification source -- system files */
          if (opt_filename)
            g_variant_builder_add (builder, "{sv}", "filename", g_variant_new_string (opt_filename));
          sign_options = g_variant_builder_end (builder);

          if (!ostree_sign_load_pk (sign, sign_options, error))
            goto out;

          if (ostree_sign_commit_verify (sign,
                                         repo,
                                         resolved_commit,
                                         &success_message,
                                         cancellable,
                                         error))
            {
              g_print ("%s\n", success_message);
              ret = TRUE;
            }
        } /* Check via file */
    }
  else
    {
      /* Sign with keys from provided file */
      if (opt_filename)
        {
          g_autoptr (GFile) keyfile = NULL;
          g_autoptr (GFileInputStream) key_stream_in = NULL;
          g_autoptr (GDataInputStream) key_data_in = NULL;

          if (!g_file_test (opt_filename, G_FILE_TEST_IS_REGULAR))
            {
              g_warning ("Can't open file '%s' with keys", opt_filename);
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "File object '%s' is not a regular file", opt_filename);
              goto out;
            }

          keyfile = g_file_new_for_path (opt_filename);
          key_stream_in = g_file_read (keyfile, NULL, error);
          if (key_stream_in == NULL)
            goto out;

          key_data_in = g_data_input_stream_new (G_INPUT_STREAM(key_stream_in));
          g_assert (key_data_in != NULL);

          /* Use simple file format with just a list of base64 public keys per line */
          while (TRUE)
            {
              gsize len = 0;
              g_autofree char *line = g_data_input_stream_read_line (key_data_in, &len, NULL, error);
              g_autoptr (GVariant) sk = NULL;

              if (*error != NULL)
                goto out;

              if (line == NULL)
                break;


              // Pass the key as a string
              sk = g_variant_new_string(line);
              if (!ostree_sign_set_sk (sign, sk, error))
                {
                  ret = FALSE;
                  goto out;
                }

              ret = ostree_sign_commit (sign,
                                        repo,
                                        resolved_commit,
                                        cancellable,
                                        error);
              if (ret != TRUE)
                goto out;
            }
        }
    }
  // No valid signature found
  if (opt_verify && (ret != TRUE) && (*error == NULL))
    g_set_error_literal (error,
                         G_IO_ERROR, G_IO_ERROR_FAILED,
                         "No valid signatures found");

out:
  /* It is possible to have an error due multiple signatures check */
  if (ret == TRUE)
    g_clear_error (error);
  return ret;
}
