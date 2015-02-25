/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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

static char *opt_gpg_homedir;

static GOptionEntry options[] = {
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "HOMEDIR"},
};

static void
usage_error (GOptionContext *context, const char *message, GError **error)
{
  gs_free char *help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s", help);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
}

gboolean
ostree_builtin_gpg_sign (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_free char *resolved_commit = NULL;
  const char *commit;
  char **key_ids;
  int n_key_ids, ii;
  gboolean ret = FALSE;

  context = g_option_context_new ("COMMIT KEY-ID... - Sign a commit");

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "Need a COMMIT to sign", error);
      goto out;
    }

  if (argc < 3)
    {
      usage_error (context, "Need at least one GPG KEY-ID to sign with", error);
      goto out;
    }

  commit = argv[1];
  key_ids = argv + 2;
  n_key_ids = argc - 2;

  if (!ostree_repo_resolve_rev (repo, commit, FALSE, &resolved_commit, error))
    goto out;

  for (ii = 0; ii < n_key_ids; ii++)
    {
      if (!ostree_repo_sign_commit (repo, resolved_commit, key_ids[ii],
                                    opt_gpg_homedir, cancellable, error))
        goto out;
    }

  ret = TRUE;

out:
  if (context)
    g_option_context_free (context);

  return ret;
}

