/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include "ostree-repo-private.h"
#include "ot-dump.h"
#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

static gboolean opt_update, opt_view, opt_raw;
static char **opt_key_ids;
static char *opt_gpg_homedir;

static GOptionEntry options[] = {
  { "update", 'u', 0, G_OPTION_ARG_NONE, &opt_update, "Update the summary", NULL },
  { "view", 'v', 0, G_OPTION_ARG_NONE, &opt_view, "View the local summary file", NULL },
  { "raw", 0, 0, G_OPTION_ARG_NONE, &opt_raw, "View the raw bytes of the summary file", NULL },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_key_ids, "GPG Key ID to sign the summary with", "KEY-ID"},
  { "gpg-homedir", 0, 0, G_OPTION_ARG_FILENAME, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "HOMEDIR"},
  { NULL }
};

gboolean
ostree_builtin_summary (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  OstreeDumpFlags flags = OSTREE_DUMP_NONE;

  context = g_option_context_new ("Manage summary metadata");

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (opt_update)
    {
      if (!ostree_ensure_repo_writable (repo, error))
        goto out;

      if (!ostree_repo_regenerate_summary (repo, NULL, cancellable, error))
        goto out;

      if (opt_key_ids)
        {
          if (!ostree_repo_add_gpg_signature_summary (repo,
                                                      (const gchar **) opt_key_ids,
                                                      opt_gpg_homedir,
                                                      cancellable,
                                                      error))
            goto out;
        }
    }
  else if (opt_view)
    {
      g_autoptr(GBytes) summary_data = NULL;

      if (opt_raw)
        flags |= OSTREE_DUMP_RAW;

      summary_data = ot_file_mapat_bytes (repo->repo_dir_fd, "summary", error);
      if (!summary_data)
        goto out;

      ot_dump_summary_bytes (summary_data, flags);
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No option specified; use -u to update summary");
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}
