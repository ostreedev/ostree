/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

static char *opt_from_rev;
static char *opt_to_rev;
static char *opt_apply;

static GOptionEntry options[] = {
  { "from", 0, 0, G_OPTION_ARG_STRING, &opt_from_rev, "Create delta from revision REV", "REV" },
  { "to", 0, 0, G_OPTION_ARG_STRING, &opt_to_rev, "Create delta to revision REV", "REV" },
  { "apply", 0, 0, G_OPTION_ARG_FILENAME, &opt_apply, "Apply delta from PATH", "PATH" },
  { NULL }
};

gboolean
ostree_builtin_static_delta (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_ptrarray GPtrArray *delta_names = NULL;

  context = g_option_context_new ("Manage static delta files");

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (opt_apply)
    {
      gs_unref_object GFile *path = g_file_new_for_path (opt_apply);

      if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
        goto out;

      if (!ostree_repo_static_delta_execute_offline (repo, path, TRUE, cancellable, error))
        goto out;

      if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
        goto out;
    }
  else
    {
      if (argc >= 2 && opt_to_rev == NULL)
        opt_to_rev = argv[1];

      if (argc < 2 && opt_to_rev == NULL)
        {
          guint i;
          if (!ostree_repo_list_static_delta_names (repo, &delta_names, cancellable, error))
            goto out;

          if (delta_names->len == 0)
            {
              g_print ("(No static deltas)\n");
            }
          else
            {
              for (i = 0; i < delta_names->len; i++)
                {
                  g_print ("%s\n", (char*)delta_names->pdata[i]);
                }
            }
        }
      else if (opt_to_rev != NULL)
        {
          const char *from_source;
          gs_free char *from_resolved = NULL;
          gs_free char *to_resolved = NULL;
          gs_free char *from_parent_str = NULL;

          if (opt_from_rev == NULL)
            {
              from_parent_str = g_strconcat (opt_to_rev, "^", NULL);
              from_source = from_parent_str;
            }
          else
            {
              from_source = opt_from_rev;
            }

          if (!ostree_repo_resolve_rev (repo, from_source, FALSE, &from_resolved, error))
            goto out;
          if (!ostree_repo_resolve_rev (repo, opt_to_rev, FALSE, &to_resolved, error))
            goto out;

          g_print ("Generating static delta:\n");
          g_print ("  From: %s\n", from_resolved);
          g_print ("  To:   %s\n", to_resolved);
          if (!ostree_repo_static_delta_generate (repo, OSTREE_STATIC_DELTA_GENERATE_OPT_MAJOR,
                                                  from_resolved, to_resolved, NULL,
                                                  cancellable, error))
            goto out;
        }
      else
        {
          ot_util_usage_error (context, "--from=REV must be specified", error);
          goto out;
        }
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
