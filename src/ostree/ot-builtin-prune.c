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

#include "ot-builtins.h"
#include "ostree.h"

static gboolean opt_no_prune;
static gint opt_depth = -1;
static gboolean opt_refs_only;

static GOptionEntry options[] = {
  { "no-prune", 0, 0, G_OPTION_ARG_NONE, &opt_no_prune, "Only display unreachable objects; don't delete", NULL },
  { "refs-only", 0, 0, G_OPTION_ARG_NONE, &opt_refs_only, "Only compute reachability via refs", NULL },
  { "depth", 0, 0, G_OPTION_ARG_INT, &opt_depth, "Only traverse DEPTH parents for each commit (default: -1=infinite)", "DEPTH" },
  { NULL }
};

gboolean
ostree_builtin_prune (int argc, char **argv, GFile *repo_path, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_free char *formatted_freed_size = NULL;
  OstreeRepoPruneFlags pruneflags = 0;
  gint n_objects_total;
  gint n_objects_pruned;
  guint64 objsize_total;

  context = g_option_context_new ("- Search for unreachable objects");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (opt_refs_only)
    pruneflags |= OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY;
  if (opt_no_prune)
    pruneflags |= OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE;

  if (!ostree_repo_prune (repo, pruneflags, opt_depth,
                          &n_objects_total, &n_objects_pruned, &objsize_total,
                          cancellable, error))
    goto out;

  formatted_freed_size = g_format_size_full (objsize_total, 0);

  g_print ("Total objects: %u\n", n_objects_total);
  if (n_objects_pruned == 0)
    g_print ("No unreachable objects\n");
  else if (pruneflags & OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE)
    g_print ("Would delete: %u objects, freeing %s bytes\n",
             n_objects_pruned, formatted_freed_size);
  else
    g_print ("Deleted %u objects, %s bytes freed\n",
             n_objects_pruned, formatted_freed_size);

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
