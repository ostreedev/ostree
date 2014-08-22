/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Anne LoVers <anne.loverso@students.olin.edu>
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

#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

static char *opt_rm;

static GOptionEntry options[] = {
  { "rm", 0, 0, G_OPTION_ARG_NONE, &opt_rm, "Remove a custom name", NULL },
  { NULL }
};

gboolean
ostree_builtin_name (int argc, char **argv, OstreeRepo *repo, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  const char *rev = "master";
  const char *newname = NULL;
  gs_free char *resolved_rev = NULL;
  gs_free char *name = NULL;
  gs_free char *custom_name = NULL;
  gs_unref_object GFile *path_to_customs = NULL;

  context = g_option_context_new ("Change the name of a deployment");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "REV must be specified", error);
      goto out;
    }
    
    rev = argv[1];
    if (!ostree_repo_resolve_rev (repo, rev, FALSE, &resolved_rev, error))
      goto out;
    path_to_customs = ot_gfile_resolve_path_printf (ostree_repo_get_path (repo), "state/custom_names");
    
    if (argc < 3 && !opt_rm)
      {
        if (!ostree_deployment_get_name (resolved_rev, path_to_customs, &name, error))
          goto out;
        g_print ("%s\n", name);
      }

    else if (!opt_rm)
      {
        newname = argv[2];
        custom_name = g_strdup (newname);
        if (!ostree_deployment_set_custom_name (resolved_rev, custom_name, path_to_customs, cancellable, error))
          goto out;
        g_print ("Name of %s successfully changed to %s\n", resolved_rev, newname);
      }

    else if (opt_rm)
      {
        if (!ostree_deployment_rm_custom_name (resolved_rev, path_to_customs, cancellable, error))
          goto out;
      }

  ret = TRUE;
 out:
  if (context) {
    g_option_context_free (context);
  }
  return ret;
}