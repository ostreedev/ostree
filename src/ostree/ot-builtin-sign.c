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
#include "ostree-sign-dummy.h"
#if defined(HAVE_LIBSODIUM)
#include "ostree-sign-ed25519.h"
#include <sodium.h>
#endif

static gboolean opt_delete;
static gboolean opt_verify;
static char *opt_sign_name;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-sign.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "delete", 'd', 0, G_OPTION_ARG_NONE, &opt_delete, "Delete signatures having any of the KEY-IDs", NULL},
  { "verify", 0, 0, G_OPTION_ARG_NONE, &opt_verify, "Verify signatures", NULL},
  { "sign-type", 's', 0, G_OPTION_ARG_STRING, &opt_sign_name, "Signature type to use (defaults to 'ed25519')", "NAME"},
#if defined(HAVE_LIBSODIUM)
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
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  g_autoptr (OstreeSign) sign = NULL;
  g_autofree char *resolved_commit = NULL;
  const char *commit;
  char **key_ids;
  int n_key_ids, ii;
  gboolean ret = FALSE;
#if defined(HAVE_LIBSODIUM)
  g_autoptr (GVariant) ed25519_sk = NULL;
  g_autoptr (GVariant) ed25519_pk = NULL;
#endif


  context = g_option_context_new ("COMMIT KEY-ID...");


  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "Need a COMMIT to sign or verify", error);
      goto out;
    }

  commit = argv[1];

  if (!opt_verify && argc < 3)
    {
      usage_error (context, "Need at least one KEY-ID to sign with", error);
      goto out;
    }

  key_ids = argv + 2;
  n_key_ids = argc - 2;

  if (!ostree_repo_resolve_rev (repo, commit, FALSE, &resolved_commit, error))
    goto out;

  /* Initialize crypto system */
  if (!opt_sign_name)
    opt_sign_name = "ed25519";

  sign = ostree_sign_get_by_name (opt_sign_name, error);
  if (sign == NULL)
    {
      ret = FALSE;
      goto out;
    }

  for (ii = 0; ii < n_key_ids; ii++)
    {
      g_autoptr (GVariant) sk = NULL;
      g_autoptr (GVariant) pk = NULL;
      g_autofree guchar *key = NULL;

      if (!g_strcmp0(ostree_sign_get_name(sign), "dummy"))
        {
          // Just use the string as signature
          sk = g_variant_new_string(key_ids[ii]);
          pk = g_variant_new_string(key_ids[ii]);
        }
      if (opt_verify)
        {
#if defined(HAVE_LIBSODIUM)
          if (!g_strcmp0(ostree_sign_get_name(sign), "ed25519"))
            {
              gsize key_len = 0;
              key = g_malloc0 (crypto_sign_PUBLICKEYBYTES);
              if (sodium_hex2bin (key, crypto_sign_PUBLICKEYBYTES,
                                  key_ids[ii], strlen (key_ids[ii]),
                                  NULL, &key_len, NULL) != 0)
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Invalid KEY '%s'", key_ids[ii]);

                  goto out;
                }

              pk = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, key, key_len, sizeof(guchar));
            }
#endif

          if (!ostree_sign_set_pk (sign, pk, error))
            {
              ret = FALSE;
              goto out;
            }

          if (ostree_sign_commit_verify (sign,
                                         repo,
                                         resolved_commit,
                                         cancellable,
                                         error))
            ret = TRUE;
        }
      else
        {
#if defined(HAVE_LIBSODIUM)
          if (!g_strcmp0(ostree_sign_get_name(sign), "ed25519"))
            {
              gsize key_len = 0;
              key = g_malloc0 (crypto_sign_SECRETKEYBYTES);
              if (sodium_hex2bin (key, crypto_sign_SECRETKEYBYTES,
                                  key_ids[ii], strlen (key_ids[ii]),
                                  NULL, &key_len, NULL) != 0)
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Invalid KEY '%s'", key_ids[ii]);

                  goto out;
                }

              sk = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, key, key_len, sizeof(guchar));
            }
#endif
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

  // No valid signature found
  if (opt_verify && (ret != TRUE))
    g_set_error_literal (error,
                         G_IO_ERROR, G_IO_ERROR_FAILED,
                         "No valid signatures found");

out:
  return ret;
}
