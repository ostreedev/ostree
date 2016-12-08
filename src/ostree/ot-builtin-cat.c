/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
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

static GOptionEntry options[] = {
  { NULL },
};

static gboolean
cat_one_file (GFile         *f,
              GOutputStream *stdout_stream,
              GCancellable  *cancellable,
              GError       **error)
{
  gboolean ret = FALSE;
  g_autoptr(GInputStream) in = NULL;
  
  in = (GInputStream*)g_file_read (f, cancellable, error);
  if (!in)
    goto out;

  {
    gssize n_bytes_written = g_output_stream_splice (stdout_stream, in, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                                     cancellable, error);
    if (n_bytes_written < 0)
      goto out;
  }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_builtin_cat (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  gboolean ret = FALSE;
  int i;
  const char *rev;
  g_autoptr(GOutputStream) stdout_stream = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) f = NULL;

  context = g_option_context_new ("COMMIT PATH... - Concatenate contents of files");

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc <= 2)
    {
      ot_util_usage_error (context, "A COMMIT and at least one PATH argument are required", error);
      goto out;
    }
  rev = argv[1];

  if (!ostree_repo_read_commit (repo, rev, &root, NULL, NULL, error))
    goto out;

  stdout_stream = g_unix_output_stream_new (1, FALSE);

  for (i = 2; i < argc; i++)
    {
      g_clear_object (&f);
      f = g_file_resolve_relative_path (root, argv[i]);

      if (!cat_one_file (f, stdout_stream, cancellable, error))
        goto out;
    }
 
  ret = TRUE;
 out:
  return ret;
}
