/*
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
ostree_builtin_cat (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("COMMIT PATH... - Concatenate contents of files");
  g_autoptr(OstreeRepo) repo = NULL;
  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
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
