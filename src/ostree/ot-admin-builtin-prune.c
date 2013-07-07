/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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

#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"

#include <glib/gi18n.h>

static gboolean opt_no_repo_prune;

static GOptionEntry options[] = {
  { "no-repo-prune", 0, 0, G_OPTION_ARG_NONE, &opt_no_repo_prune, "Only prune deployment checkouts; don't prune repository", NULL },
  { NULL }
};


gboolean
ot_admin_builtin_prune (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  const char *osname;
  gs_unref_object GFile *repo_path = NULL;
  gs_unref_object GFile *deploy_dir = NULL;
  gs_unref_object GFile *current_deployment = NULL;
  gs_unref_object GFile *previous_deployment = NULL;
  gs_unref_object GFile *active_deployment = NULL;
  gs_free char *active_osname = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  context = g_option_context_new ("OSNAME - Delete untagged deployments and repository objects");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "OSNAME must be specified", error);
      goto out;
    }

  osname = argv[1];

  if (!ot_admin_cleanup (admin_opts->sysroot, cancellable, error))
    goto out;

  repo_path = g_file_resolve_relative_path (admin_opts->sysroot, "ostree/repo");

  if (!opt_no_repo_prune)
    {
      gs_free char *repo_arg = NULL;

      repo_arg = g_strconcat ("--repo=", gs_file_get_path_cached (repo_path), NULL);
      
      if (!gs_subprocess_simple_run_sync (NULL,
                                          GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                          cancellable, error,
                                          "ostree", repo_arg, "prune", "--refs-only",
                                          "--depth=0", NULL))
        goto out;
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
