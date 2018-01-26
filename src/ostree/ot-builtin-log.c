/*
 * Copyright (C) 2013 Stef Walter <stefw@redhat.com>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Author: Stef Walter <stefw@redhat.com>
 */

#include "config.h"

#include "ot-main.h"
#include "ot-builtins.h"
#include "ot-dump.h"
#include "ostree.h"
#include "otutil.h"

static gboolean opt_raw;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-log.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "raw", 0, 0, G_OPTION_ARG_NONE, &opt_raw, "Show raw variant data" },
  { NULL }
};

static gboolean
log_commit (OstreeRepo     *repo,
            const gchar    *checksum,
            gboolean        is_recurse,
            OstreeDumpFlags flags,
            GError        **error)
{
  g_autoptr(GVariant) variant = NULL;
  g_autofree char *parent = NULL;
  gboolean ret = FALSE;
  GError *local_error = NULL;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                 &variant, &local_error))
    {
      if (is_recurse && g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_print ("<< History beyond this commit not fetched >>\n");
          g_clear_error (&local_error);
          ret = TRUE;
        }
      else
        {
          g_propagate_error (error, local_error);
        }
      goto out;
    }

  ot_dump_object (OSTREE_OBJECT_TYPE_COMMIT, checksum, variant, flags);

  /* Get the parent of this commit */
  parent = ostree_commit_get_parent (variant);
  if (parent && !log_commit (repo, parent, TRUE, flags, error))
    goto out;

  ret = TRUE;
out:
  return ret;
}

gboolean
ostree_builtin_log (int           argc,
                    char        **argv,
                    OstreeCommandInvocation *invocation,
                    GCancellable *cancellable,
                    GError      **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  gboolean ret = FALSE;
  const char *rev;
  g_autofree char *checksum = NULL;
  OstreeDumpFlags flags = OSTREE_DUMP_NONE;

  context = g_option_context_new ("REF");

  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    goto out;

  if (opt_raw)
    flags |= OSTREE_DUMP_RAW;

  if (argc <= 1)
    {
      ot_util_usage_error (context, "A ref argument is required", error);
      goto out;
    }
  rev = argv[1];

  if (!ostree_repo_resolve_rev (repo, rev, FALSE, &checksum, error))
    goto out;

  if (!log_commit (repo, checksum, FALSE, flags, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
