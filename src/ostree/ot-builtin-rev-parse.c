/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

static char *opt_name;

static GOptionEntry options[] = {
  { "name", 0, 0, G_OPTION_ARG_STRING, &opt_name, "Change name to NAME", "NAME" },
  { NULL }
};

gboolean
ostree_builtin_rev_parse (int argc, char **argv, OstreeRepo *repo, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  const char *rev = "master";
  int i;
  gs_free char *resolved_rev = NULL;

  context = g_option_context_new ("REV - Output the target of a rev");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "REV must be specified", error);
      goto out;
    }
  for (i = 1; i < argc; i++)
    {
      rev = argv[i];
      g_free (resolved_rev);
      resolved_rev = NULL;
      if (!ostree_repo_resolve_rev (repo, rev, FALSE, &resolved_rev, error))
        goto out;
      g_print ("%s\n", resolved_rev);
    }

  if (opt_name)
    {
      gs_unref_object GFile *path_to_customs = ot_gfile_resolve_path_printf (ostree_repo_get_path (repo), "state/custom_names");

      if (!ostree_deployment_set_custom_name (resolved_rev, opt_name, path_to_customs, cancellable, error))
        goto out;
      g_print ("Name of %s successfully changed to %s\n", resolved_rev, opt_name);
    }
 
  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
