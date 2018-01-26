/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-rev-parse.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ostree_builtin_rev_parse (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  gboolean ret = FALSE;
  const char *rev = "master";
  int i;
  g_autofree char *resolved_rev = NULL;

  context = g_option_context_new ("REV");

  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
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
 
  ret = TRUE;
 out:
  return ret;
}
