/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#include "config.h"

#include "otutil.h"

#include "ot-main.h"
#include "ot-remote-builtins.h"

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-remote.xml) when changing the option list.
 */

static GOptionEntry option_entries[] = {
  { NULL }
};

gboolean
ot_remote_builtin_show_url (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *remote_name;
  g_autofree char *remote_url = NULL;
  gboolean ret = FALSE;

  context = g_option_context_new ("NAME");

  if (!ostree_option_context_parse (context, option_entries, &argc, &argv,
                                    invocation, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "NAME must be specified", error);
      goto out;
    }

  remote_name = argv[1];

  if (ostree_repo_remote_get_url (repo, remote_name, &remote_url, error))
    {
      g_print ("%s\n", remote_url);
      ret = TRUE;
    }

 out:
  return ret;
}
