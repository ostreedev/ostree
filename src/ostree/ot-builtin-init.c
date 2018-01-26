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

static char *opt_mode = "bare";
#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
static char *opt_collection_id = NULL;
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-init.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "mode", 0, 0, G_OPTION_ARG_STRING, &opt_mode, "Initialize repository in given mode (bare, archive)", NULL },
#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
  { "collection-id", 0, 0, G_OPTION_ARG_STRING, &opt_collection_id,
    "Globally unique ID for this repository as an collection of refs for redistribution to other repositories", "COLLECTION-ID" },
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */
  { NULL }
};

gboolean
ostree_builtin_init (int argc, char **argv,OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  gboolean ret = FALSE;
  OstreeRepoMode mode;

  context = g_option_context_new ("");

  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    goto out;

  if (!ostree_repo_mode_from_string (opt_mode, &mode, error))
    goto out;
#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
  if (!ostree_repo_set_collection_id (repo, opt_collection_id, error))
    goto out;
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */

  if (!ostree_repo_create (repo, mode, NULL, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
