/*
 * Copyright (C) 2015 Red Hat, Inc.
 * Copyright (C) 2016 Sjoerd Simons <sjoerd@luon.net>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#include "config.h"

#include "otutil.h"

#include "ot-main.h"
#include "ot-remote-builtins.h"
#include "ostree-repo-private.h"
#include "ot-remote-cookie-util.h"

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-remote.xml) when changing the option list.
 */

static GOptionEntry option_entries[] = {
  { NULL }
};

gboolean
ot_remote_builtin_list_cookies (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(OstreeRepo) repo = NULL;
  g_autoptr(GOptionContext) context = g_option_context_new ("NAME");

  if (!ostree_option_context_parse (context, option_entries, &argc, &argv,
                                    invocation, &repo, cancellable, error))
    return FALSE;

  if (argc < 2)
    {
      ot_util_usage_error (context, "NAME must be specified", error);
      return FALSE;
    }

  const char *remote_name = argv[1];
  g_autofree char *cookie_file = g_strdup_printf ("%s.cookies.txt", remote_name);
  if (!ot_list_cookies_at (ostree_repo_get_dfd (repo), cookie_file, error))
    return FALSE;

  return TRUE;
}
