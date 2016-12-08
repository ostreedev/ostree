/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
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
#include "ot-remote-builtins.h"

static gboolean opt_show_urls;

static GOptionEntry option_entries[] = {
  { "show-urls", 'u', 0, G_OPTION_ARG_NONE, &opt_show_urls, "Show remote URLs in list", NULL },
  { NULL }
};

gboolean
ot_remote_builtin_list (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  g_auto(GStrv) remotes = NULL;
  guint ii, n_remotes = 0;
  gboolean ret = FALSE;

  context = g_option_context_new ("- List remote repository names");

  if (!ostree_option_context_parse (context, option_entries, &argc, &argv,
                                    OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  remotes = ostree_repo_remote_list (repo, &n_remotes);

  if (opt_show_urls)
    {
      int max_length = 0;

      for (ii = 0; ii < n_remotes; ii++)
        max_length = MAX (max_length, strlen (remotes[ii]));

      for (ii = 0; ii < n_remotes; ii++)
        {
          g_autofree char *remote_url = NULL;

          if (!ostree_repo_remote_get_url (repo, remotes[ii], &remote_url, error))
            goto out;

          g_print ("%-*s  %s\n", max_length, remotes[ii], remote_url);
        }
    }
  else
    {
      for (ii = 0; ii < n_remotes; ii++)
        g_print ("%s\n", remotes[ii]);
    }

  ret = TRUE;

 out:
  return ret;
}
