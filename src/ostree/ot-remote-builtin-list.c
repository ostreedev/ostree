/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#include "config.h"

#include "ot-main.h"
#include "ot-remote-builtins.h"

static gboolean opt_show_urls;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-remote.xml) when changing the option list.
 */

static GOptionEntry option_entries[] = {
  { "show-urls", 'u', 0, G_OPTION_ARG_NONE, &opt_show_urls, "Show remote URLs in list", NULL },
  { NULL }
};

gboolean
ot_remote_builtin_list (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  g_auto(GStrv) remotes = NULL;
  guint ii, n_remotes = 0;
  gboolean ret = FALSE;

  context = g_option_context_new ("");

  if (!ostree_option_context_parse (context, option_entries, &argc, &argv,
                                    invocation, &repo, cancellable, error))
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
