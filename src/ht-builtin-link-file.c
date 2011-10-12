/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ht-builtins.h"
#include "hacktree.h"

#include <glib/gi18n.h>

static char *repo_path;
static GOptionEntry options[] = {
  { "repo", 0, 0, G_OPTION_ARG_FILENAME, &repo_path, "Repository path", NULL },
  { NULL }
};

gboolean
hacktree_builtin_link_file (int argc, const char **argv, const char *prefix, GError **error)
{
  gboolean ret = FALSE;
  HacktreeRepo *repo = NULL;
  int i;

  if (repo_path == NULL)
    repo_path = ".";

  repo = hacktree_repo_new (repo_path);
  if (!hacktree_repo_check (repo, error))
    goto out;

  for (i = 0; i < argc; i++)
    {
      if (!hacktree_repo_link_file (repo, argv[i], error))
        goto out;
    }
 
  ret = TRUE;
 out:
  g_clear_object (&repo);
  return ret;
}
