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
#include "parse-datetime.h"

static gboolean opt_no_prune;
static gint opt_depth = -1;
static gboolean opt_refs_only;
static char *opt_delete_commit;
static char *opt_before;

static GOptionEntry options[] = {
  { "no-prune", 0, 0, G_OPTION_ARG_NONE, &opt_no_prune, "Only display unreachable objects; don't delete", NULL },
  { "refs-only", 0, 0, G_OPTION_ARG_NONE, &opt_refs_only, "Only compute reachability via refs", NULL },
  { "depth", 0, 0, G_OPTION_ARG_INT, &opt_depth, "Only traverse DEPTH parents for each commit (default: -1=infinite)", "DEPTH" },
  { "delete-commit", 0, 0, G_OPTION_ARG_STRING, &opt_delete_commit, "Specify a commit to delete", "COMMIT" },
  { "before", 0, 0, G_OPTION_ARG_STRING, &opt_before, "Prune all commits older than the specified date", "DATE" },
  { NULL }
};

static gboolean
delete_commit (OstreeRepo *repo, const char *commit_to_delete, GCancellable *cancellable, GError **error)
{
  g_autoptr(GHashTable) refs = NULL;
  GHashTableIter hashiter;
  gpointer hashkey, hashvalue;
  gboolean ret = FALSE;

  if (!ostree_repo_list_refs (repo, NULL, &refs, cancellable, error))
    goto out;

  g_hash_table_iter_init (&hashiter, refs);
  while (g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue))
    {
      const char *ref = hashkey;
      const char *commit = hashvalue;
      if (g_strcmp0 (commit_to_delete, commit) == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Commit '%s' is referenced by '%s'", commit_to_delete, ref);
          goto out;
        }
    }

  if (!ot_enable_tombstone_commits (repo, error))
    goto out;

  if (!ostree_repo_delete_object (repo, OSTREE_OBJECT_TYPE_COMMIT, commit_to_delete, cancellable, error))
    goto out;

  ret = TRUE;

 out:
  return ret;
}

static gboolean
prune_commits_before_date (OstreeRepo *repo, const char *date, GCancellable *cancellable, GError **error)
{
  g_autoptr(GHashTable) objects = NULL;
  GHashTableIter hash_iter;
  gpointer key, value;
  struct timespec ts;
  gboolean ret = FALSE;

  if (!parse_datetime (&ts, date, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not parse '%s'", date);
      goto out;
    }

  if (!ot_enable_tombstone_commits (repo, error))
    goto out;

  if (!ostree_repo_list_objects (repo, OSTREE_REPO_LIST_OBJECTS_ALL, &objects,
                                 cancellable, error))
    goto out;

  g_hash_table_iter_init (&hash_iter, objects);

  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;
      guint64 commit_timestamp;
      g_autoptr(GVariant) commit = NULL;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      if (objtype != OSTREE_OBJECT_TYPE_COMMIT)
        continue;

      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                     &commit, error))
        goto out;

      commit_timestamp = ostree_commit_get_timestamp (commit);
      if (commit_timestamp < ts.tv_sec)
        {
          if (!ostree_repo_delete_object (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;

 out:
  return ret;
}

gboolean
ostree_builtin_prune (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  glnx_unref_object OstreeRepo *repo = NULL;
  g_autofree char *formatted_freed_size = NULL;
  OstreeRepoPruneFlags pruneflags = 0;
  gint n_objects_total;
  gint n_objects_pruned;
  guint64 objsize_total;

  context = g_option_context_new ("- Search for unreachable objects");

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (!opt_no_prune && !ostree_ensure_repo_writable (repo, error))
    goto out;

  if (opt_delete_commit)
    {
      if (opt_no_prune)
        {
          ot_util_usage_error (context, "Cannot specify both --delete-commit and --no-prune", error);
          goto out;
        }
      if (!delete_commit (repo, opt_delete_commit, cancellable, error))
        goto out;
    }
  if (opt_before)
    {
      if (opt_no_prune)
        {
          ot_util_usage_error (context, "Cannot specify both --before and --no-prune", error);
          goto out;
        }
      if (!prune_commits_before_date (repo, opt_before, cancellable, error))
        goto out;
    }

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
    g_print ("Would delete: %u objects, freeing %s\n",
             n_objects_pruned, formatted_freed_size);
  else
    g_print ("Deleted %u objects, %s freed\n",
             n_objects_pruned, formatted_freed_size);

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
