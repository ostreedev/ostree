/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include "ostree-repo-private.h"
#include "ostree-sign.h"
#include "ostree.h"
#include "ot-builtins.h"
#include "ot-dump.h"
#include "otutil.h"

static gboolean opt_update, opt_view, opt_raw;
static gboolean opt_list_metadata_keys;
static char *opt_print_metadata_key;
static char **opt_gpg_key_ids;
static char *opt_gpg_homedir;
static char **opt_key_ids;
static char *opt_sign_name;
static char **opt_metadata;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-summary.xml) when changing the option list.
 */

static GOptionEntry options[]
    = { { "update", 'u', 0, G_OPTION_ARG_NONE, &opt_update, "Update the summary", NULL },
        { "view", 'v', 0, G_OPTION_ARG_NONE, &opt_view, "View the local summary file", NULL },
        { "raw", 0, 0, G_OPTION_ARG_NONE, &opt_raw, "View the raw bytes of the summary file",
          NULL },
        { "list-metadata-keys", 0, 0, G_OPTION_ARG_NONE, &opt_list_metadata_keys,
          "List the available metadata keys", NULL },
        { "print-metadata-key", 0, 0, G_OPTION_ARG_STRING, &opt_print_metadata_key,
          "Print string value of metadata key", "KEY" },
        { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids,
          "GPG Key ID to sign the summary with", "KEY-ID" },
        { "gpg-homedir", 0, 0, G_OPTION_ARG_FILENAME, &opt_gpg_homedir,
          "GPG Homedir to use when looking for keyrings", "HOMEDIR" },
        { "sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_key_ids, "Key ID to sign the summary with",
          "KEY-ID" },
        { "sign-type", 0, 0, G_OPTION_ARG_STRING, &opt_sign_name,
          "Signature type to use (defaults to 'ed25519')", "NAME" },
        { "add-metadata", 'm', 0, G_OPTION_ARG_STRING_ARRAY, &opt_metadata,
          "Additional metadata field to add to the summary", "KEY=VALUE" },
        { NULL } };

/* Take arguments of the form KEY=VALUE and put them into an a{sv} variant. The
 * value arguments must be parsable using g_variant_parse(). */
static GVariant *
build_additional_metadata (const char *const *args, GError **error)
{
  g_autoptr (GVariantBuilder) builder = NULL;

  builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);

  for (gsize i = 0; args[i] != NULL; i++)
    {
      const gchar *equals = strchr (args[i], '=');
      g_autofree gchar *key = NULL;
      const gchar *value_str;
      g_autoptr (GVariant) value = NULL;

      if (equals == NULL)
        return glnx_null_throw (error, "Missing '=' in KEY=VALUE metadata '%s'", args[i]);

      key = g_strndup (args[i], equals - args[i]);
      value_str = equals + 1;

      value = g_variant_parse (NULL, value_str, NULL, NULL, error);
      if (value == NULL)
        return glnx_prefix_error_null (error, "Error parsing variant ‘%s’: ", value_str);

      g_variant_builder_add (builder, "{sv}", key, value);
    }

  return g_variant_ref_sink (g_variant_builder_end (builder));
}

/* @out_summary_data is guaranteed to return non-NULL, but @out_summary_sig_data
 * may return NULL (with no error) if the summary is unsigned. */
static gboolean
get_summary_data (OstreeRepo *repo, GBytes **out_summary_data, GBytes **out_summary_sig_data,
                  GError **error)
{
  g_assert (out_summary_data != NULL);
  g_assert (out_summary_sig_data != NULL);

  g_autoptr (GBytes) summary_data = NULL;
  glnx_autofd int fd = -1;
  if (!glnx_openat_rdonly (repo->repo_dir_fd, "summary", TRUE, &fd, error))
    return FALSE;
  summary_data = ot_fd_readall_or_mmap (fd, 0, error);
  if (!summary_data)
    return FALSE;

  g_autoptr (GBytes) summary_sig_data = NULL;
  glnx_autofd int sig_fd = -1;
  if (!ot_openat_ignore_enoent (repo->repo_dir_fd, "summary.sig", &sig_fd, error))
    return FALSE;
  if (sig_fd >= 0)
    {
      summary_sig_data = ot_fd_readall_or_mmap (sig_fd, 0, error);
      if (!summary_sig_data)
        return FALSE;
    }

  *out_summary_data = g_steal_pointer (&summary_data);
  *out_summary_sig_data = g_steal_pointer (&summary_sig_data);

  return TRUE;
}

gboolean
ostree_builtin_summary (int argc, char **argv, OstreeCommandInvocation *invocation,
                        GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = NULL;
  g_autoptr (OstreeRepo) repo = NULL;
  g_autoptr (OstreeSign) sign = NULL;
  OstreeDumpFlags flags = OSTREE_DUMP_NONE;
  g_autoptr (GBytes) summary_data = NULL;
  g_autoptr (GBytes) summary_sig_data = NULL;
  gboolean show_signatures = FALSE;

  context = g_option_context_new ("");

  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable,
                                    error))
    return FALSE;

  /* Initialize crypto system */
  if (opt_key_ids)
    {
      opt_sign_name = opt_sign_name ?: OSTREE_SIGN_NAME_ED25519;

      sign = ostree_sign_get_by_name (opt_sign_name, error);
      if (sign == NULL)
        return FALSE;
    }

  if (opt_update)
    {
      g_autoptr (GVariant) additional_metadata = NULL;

      if (!ostree_ensure_repo_writable (repo, error))
        return FALSE;

      if (opt_metadata != NULL)
        {
          additional_metadata
              = build_additional_metadata ((const char *const *)opt_metadata, error);
          if (additional_metadata == NULL)
            return FALSE;
        }

      /* Regenerate and sign the repo metadata. */
      g_auto (GVariantBuilder) metadata_opts_builder
          = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
      g_autoptr (GVariant) metadata_opts = NULL;
      if (opt_gpg_key_ids != NULL)
        g_variant_builder_add (&metadata_opts_builder, "{sv}", "gpg-key-ids",
                               g_variant_new_strv ((const char *const *)opt_gpg_key_ids, -1));
      if (opt_gpg_homedir != NULL)
        g_variant_builder_add (&metadata_opts_builder, "{sv}", "gpg-homedir",
                               g_variant_new_string (opt_gpg_homedir));
      if (opt_key_ids != NULL)
        {
          g_auto (GVariantBuilder) sk_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_ARRAY);

          /* Currently only strings are used as keys for supported
           * signature types. */
          for (const char *const *iter = (const char *const *)opt_key_ids;
               iter != NULL && *iter != NULL; iter++)
            {
              const char *key_id = *iter;
              g_variant_builder_add (&sk_builder, "v", g_variant_new_string (key_id));
            }

          g_variant_builder_add (&metadata_opts_builder, "{sv}", "sign-keys",
                                 g_variant_builder_end (&sk_builder));
        }
      if (opt_sign_name != NULL)
        g_variant_builder_add (&metadata_opts_builder, "{sv}", "sign-type",
                               g_variant_new_string (opt_sign_name));

      metadata_opts = g_variant_ref_sink (g_variant_builder_end (&metadata_opts_builder));
      if (!ostree_repo_regenerate_metadata (repo, additional_metadata, metadata_opts, cancellable,
                                            error))
        return FALSE;
    }
  else if (opt_view || opt_raw)
    {
      if (opt_raw)
        flags |= OSTREE_DUMP_RAW;

      if (!get_summary_data (repo, &summary_data, &summary_sig_data, error))
        return FALSE;

      ot_dump_summary_bytes (summary_data, flags);
      show_signatures = !opt_raw;
    }
  else if (opt_list_metadata_keys)
    {
      if (!get_summary_data (repo, &summary_data, &summary_sig_data, error))
        return FALSE;

      ot_dump_summary_metadata_keys (summary_data);
      show_signatures = FALSE;
    }
  else if (opt_print_metadata_key)
    {
      if (!get_summary_data (repo, &summary_data, &summary_sig_data, error))
        return FALSE;

      if (!ot_dump_summary_metadata_key (summary_data, opt_print_metadata_key, error))
        return FALSE;

      show_signatures = FALSE;
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No option specified; use -u to update summary");
      return FALSE;
    }

#ifndef OSTREE_DISABLE_GPGME
  if (show_signatures && summary_sig_data == NULL)
    {
      g_print ("Summary is unsigned\n");
    }
  else if (show_signatures)
    {
      g_autoptr (OstreeGpgVerifyResult) result = NULL;
      g_autoptr (GFile) gpg_homedir
          = opt_gpg_homedir ? g_file_new_for_path (opt_gpg_homedir) : NULL;
      g_autoptr (GError) local_error = NULL;

      g_assert (summary_data != NULL && summary_sig_data != NULL);

      result = ostree_repo_verify_local_summary (repo, summary_data, summary_sig_data, gpg_homedir,
                                                 NULL, cancellable, &local_error);

      if (g_error_matches (local_error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_NO_SIGNATURE))
        {
          /* Ignore */
        }
      else if (local_error != NULL)
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
      else
        {
          unsigned int n_sigs = ostree_gpg_verify_result_count_all (result);
          g_print ("\nFound %u signature%s on the summary:\n", n_sigs, n_sigs == 1 ? "" : "s");

          g_autoptr (GString) buffer = g_string_sized_new (256);
          for (unsigned int ii = 0; ii < n_sigs; ii++)
            {
              g_string_append_c (buffer, '\n');
              ostree_gpg_verify_result_describe (result, ii, buffer, "  ",
                                                 OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);
            }

          g_print ("%s", buffer->str);
        }
    }
#endif /* OSTREE_DISABLE_GPGME */

  return TRUE;
}
