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

static char *opt_mode = "bare";

static GOptionEntry options[] = {
  { "mode", 0, 0, G_OPTION_ARG_STRING, &opt_mode, "Initialize repository in given mode (bare, archive-z2)", NULL },
  { NULL }
};

gboolean
ostree_builtin_init (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  gboolean ret = FALSE;
  OstreeRepoMode mode;

  context = g_option_context_new ("- Initialize a new empty repository");

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NO_CHECK, &repo, cancellable, error))
    goto out;

  if (!ostree_repo_mode_from_string (opt_mode, &mode, error))
    goto out;

  if (!ostree_repo_create (repo, mode, NULL, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
