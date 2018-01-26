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

#include <gio/gunixoutputstream.h>

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-cat.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { NULL },
};

static gboolean
cat_one_file (GFile         *f,
              GOutputStream *stdout_stream,
              GCancellable  *cancellable,
              GError       **error)
{
  g_autoptr(GInputStream) in = (GInputStream*)g_file_read (f, cancellable, error);
  if (!in)
    return FALSE;

  if (g_output_stream_splice (stdout_stream, in, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                              cancellable, error) < 0)
    return FALSE;

  return TRUE;
}

gboolean
ostree_builtin_cat (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("COMMIT PATH...");
  g_autoptr(OstreeRepo) repo = NULL;
  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    return FALSE;

  if (argc <= 2)
    {
      ot_util_usage_error (context, "A COMMIT and at least one PATH argument are required", error);
      return FALSE;
    }
  const char *rev = argv[1];

  g_autoptr(GFile) root = NULL;
  if (!ostree_repo_read_commit (repo, rev, &root, NULL, NULL, error))
    return FALSE;

  g_autoptr(GOutputStream) stdout_stream = g_unix_output_stream_new (1, FALSE);

  for (int i = 2; i < argc; i++)
    {
      g_autoptr(GFile) f = g_file_resolve_relative_path (root, argv[i]);

      if (!cat_one_file (f, stdout_stream, cancellable, error))
        return FALSE;
    }

  return TRUE;
}
