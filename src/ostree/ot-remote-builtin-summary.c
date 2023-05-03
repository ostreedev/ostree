/*
 * Copyright (C) 2015 Red Hat, Inc.
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

#include "otutil.h"

#include "ot-dump.h"
#include "ot-main.h"
#include "ot-remote-builtins.h"

static gboolean opt_list_metadata_keys;
static gboolean opt_raw;

static char *opt_print_metadata_key;
static char *opt_cache_dir;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-remote.xml) when changing the option list.
 */

static GOptionEntry option_entries[]
    = { { "list-metadata-keys", 0, 0, G_OPTION_ARG_NONE, &opt_list_metadata_keys,
          "List the available metadata keys", NULL },
        { "print-metadata-key", 0, 0, G_OPTION_ARG_STRING, &opt_print_metadata_key,
          "Print string value of metadata key", "KEY" },
        { "cache-dir", 0, 0, G_OPTION_ARG_FILENAME, &opt_cache_dir, "Use custom cache dir", NULL },
        { "raw", 0, 0, G_OPTION_ARG_NONE, &opt_raw, "Show raw variant data", NULL },
        { NULL } };

gboolean
ot_remote_builtin_summary (int argc, char **argv, OstreeCommandInvocation *invocation,
                           GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = NULL;
  g_autoptr (OstreeRepo) repo = NULL;
  const char *remote_name;
  g_autoptr (GBytes) summary_bytes = NULL;
  g_autoptr (GBytes) signature_bytes = NULL;
  OstreeDumpFlags flags = OSTREE_DUMP_NONE;
#ifndef OSTREE_DISABLE_GPGME
  gboolean gpg_verify_summary;
#endif
  gboolean ret = FALSE;

  context = g_option_context_new ("NAME");

  if (!ostree_option_context_parse (context, option_entries, &argc, &argv, invocation, &repo,
                                    cancellable, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "NAME must be specified", error);
      goto out;
    }

  remote_name = argv[1];

  if (opt_cache_dir)
    {
      if (!ostree_repo_set_cache_dir (repo, AT_FDCWD, opt_cache_dir, cancellable, error))
        goto out;
    }

  if (opt_raw)
    flags |= OSTREE_DUMP_RAW;

  if (!ostree_repo_remote_fetch_summary (repo, remote_name, &summary_bytes, &signature_bytes,
                                         cancellable, error))
    goto out;

  if (summary_bytes == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Remote server has no summary file");
      goto out;
    }

  if (opt_list_metadata_keys)
    {
      ot_dump_summary_metadata_keys (summary_bytes);
    }
  else if (opt_print_metadata_key)
    {
      if (!ot_dump_summary_metadata_key (summary_bytes, opt_print_metadata_key, error))
        goto out;
    }
  else
    {
      ot_dump_summary_bytes (summary_bytes, flags);

#ifndef OSTREE_DISABLE_GPGME
      if (!ostree_repo_remote_get_gpg_verify_summary (repo, remote_name, &gpg_verify_summary,
                                                      error))
        goto out;

      if (!gpg_verify_summary)
        g_clear_pointer (&signature_bytes, g_bytes_unref);

      /* XXX Note we don't show signatures for "--raw".  My intuition is
       *     if someone needs to see or parse raw summary data, including
       *     signatures in the output would probably just interfere.
       *     If there's demand for it I suppose we could introduce a new
       *     option for raw signature data like "--raw-signatures". */
      if (signature_bytes != NULL && !opt_raw)
        {
          g_autoptr (OstreeGpgVerifyResult) result = NULL;

          /* The actual signed summary verification happens above in
           * ostree_repo_remote_fetch_summary().  Here we just parse
           * the signatures again for the purpose of printing. */
          result = ostree_repo_verify_summary (repo, remote_name, summary_bytes, signature_bytes,
                                               cancellable, error);
          if (result == NULL)
            goto out;

          g_print ("\n");
          ostree_print_gpg_verify_result (result);
        }
#endif /* OSTREE_DISABLE_GPGME */
    }

  ret = TRUE;
out:
  return ret;
}
