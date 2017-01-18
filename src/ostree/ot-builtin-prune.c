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
static gboolean opt_static_deltas_only;
static gint opt_depth = -1;
static gboolean opt_refs_only;
static char *opt_delete_commit;
static char *opt_keep_younger_than;
static char **opt_retain_branch_depth;

static GOptionEntry options[] = {
  { "no-prune", 0, 0, G_OPTION_ARG_NONE, &opt_no_prune, "Only display unreachable objects; don't delete", NULL },
  { "refs-only", 0, 0, G_OPTION_ARG_NONE, &opt_refs_only, "Only compute reachability via refs", NULL },
  { "depth", 0, 0, G_OPTION_ARG_INT, &opt_depth, "Only traverse DEPTH parents for each commit (default: -1=infinite)", "DEPTH" },
  { "delete-commit", 0, 0, G_OPTION_ARG_STRING, &opt_delete_commit, "Specify a commit to delete", "COMMIT" },
  { "keep-younger-than", 0, 0, G_OPTION_ARG_STRING, &opt_keep_younger_than, "Prune all commits older than the specified date", "DATE" },
  { "static-deltas-only", 0, 0, G_OPTION_ARG_NONE, &opt_static_deltas_only, "Change the behavior of delete-commit and keep-younger-than to prune only static deltas" },
  { "retain-branch-depth", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_retain_branch_depth, "Additionally retain BRANCH=DEPTH commits", "BRANCH=DEPTH" },
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
traverse_keep_younger_than (OstreeRepo *repo, const char *checksum,
                            struct timespec *ts,
                            GHashTable *reachable,
                            GCancellable *cancellable, GError **error)
{
  g_autofree char *next_checksum = g_strdup (checksum);
  g_autoptr(GVariant) commit = NULL;

  /* This is the first commit in our loop, which has a ref pointing to it. We
   * don't want to auto-prune it.
   */
  if (!ostree_repo_traverse_commit_union (repo, checksum, 0, reachable,
                                          cancellable, error))
    return FALSE;

  while (TRUE)
    {
      guint64 commit_timestamp;

      if (!ostree_repo_load_variant_if_exists (repo, OSTREE_OBJECT_TYPE_COMMIT, next_checksum,
                                               &commit, error))
        return FALSE;

      if (!commit)
        break; /* This commit was pruned, so we're done */

      commit_timestamp = ostree_commit_get_timestamp (commit);
      /* Is this commit newer than our --keep-younger-than spec? */
      if (commit_timestamp >= ts->tv_sec)
        {
          /* It's newer, traverse it */
          if (!ostree_repo_traverse_commit_union (repo, next_checksum, 0, reachable,
                                                  cancellable, error))
            return FALSE;

          g_free (next_checksum);
          next_checksum = ostree_commit_get_parent (commit);
          if (next_checksum)
            g_clear_pointer (&commit, (GDestroyNotify)g_variant_unref);
          else
            break; /* No parent, we're done */
        }
      else
        break; /* It's older than our spec, we're done */
    }

  return TRUE;
}

gboolean
ostree_builtin_prune (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GOptionContext) context = NULL;
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

  /* Special handling for explicit commit deletion here - we do this
   * first.
   */
  if (opt_delete_commit)
    {
      if (opt_no_prune)
        {
          ot_util_usage_error (context, "Cannot specify both --delete-commit and --no-prune", error);
          goto out;
        }
        if (opt_static_deltas_only)
          {
            if(!ostree_repo_prune_static_deltas (repo, opt_delete_commit, cancellable, error))
              goto out;
          }
        else if (!delete_commit (repo, opt_delete_commit, cancellable, error))
          goto out;
    }

  if (opt_refs_only)
    pruneflags |= OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY;
  if (opt_no_prune)
    pruneflags |= OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE;

  /* If no newer more complex options are specified, drop down to the original
   * prune API - both to avoid code duplication, and to keep it run from the
   * test suite.
   */
  if (!(opt_retain_branch_depth || opt_keep_younger_than))
    {
      if (!ostree_repo_prune (repo, pruneflags, opt_depth,
                              &n_objects_total, &n_objects_pruned, &objsize_total,
                              cancellable, error))
        goto out;
    }
  else
    {
      g_autoptr(GHashTable) all_refs = NULL;
      g_autoptr(GHashTable) reachable = ostree_repo_traverse_new_reachable ();
      g_autoptr(GHashTable) retain_branch_depth = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      struct timespec keep_younger_than_ts;
      GHashTableIter hash_iter;
      gpointer key, value;

      /* Otherwise, the default is --refs-only; we set this just as a note */
      opt_refs_only = TRUE;

      if (opt_keep_younger_than)
        {
          if (!parse_datetime (&keep_younger_than_ts, opt_keep_younger_than, NULL))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Could not parse '%s'", opt_keep_younger_than);
              goto out;
            }
        }

      for (char **iter = opt_retain_branch_depth; iter && *iter; iter++)
        {
          /* bd should look like BRANCH=DEPTH where DEPTH is an int */
          const char *bd = *iter;
          const char *eq = strchr (bd, '=');
          const char *depthstr;
          gint64 depth;
          char *endptr;

          if (!eq)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid value %s, must specify BRANCH=DEPTH",
                           bd);
              goto out;
            }
          depthstr = eq + 1;
          errno = EPERM;
          depth = g_ascii_strtoll (depthstr, &endptr, 10);
          if (depth == 0)
            {
              if (errno == EINVAL)
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Out of range depth %s", depthstr);
                  goto out;
                }
              else if (endptr == depthstr)
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Invalid depth %s", depthstr);
                  goto out;
                }
            }
          g_hash_table_insert (retain_branch_depth, g_strndup (bd, eq - bd),
                               GINT_TO_POINTER ((int)depth));
        }

      /* We start from the refs */
      if (!ostree_repo_list_refs (repo, NULL, &all_refs,
                                  cancellable, error))
        return FALSE;

      g_hash_table_iter_init (&hash_iter, all_refs);
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *checksum = value;
          gpointer depthp = g_hash_table_lookup (retain_branch_depth, key);
          gint depth;

          /* Here, we handle a spec like
           * --retain-branch-depth=myos/x86_64/stable=-1
           * --retain-branch-depth=myos/x86_64/dev=5
           */
          if (depthp)
            depth = GPOINTER_TO_INT(depthp);
          else if (opt_keep_younger_than)
            {
              if (!traverse_keep_younger_than (repo, checksum,
                                               &keep_younger_than_ts,
                                               reachable,
                                               cancellable, error))
                goto out;

              /* Okay, we handled the younger-than case; the other
               * two fall through to plain depth-based handling below.
               */
              continue;  /* Note again, we're skipping the below bit */
            }
          else
            depth = opt_depth; /* No --retain-branch-depth for this branch, use
                                  the global default */

          g_debug ("Finding objects to keep for commit %s", checksum);
          if (!ostree_repo_traverse_commit_union (repo, checksum, depth, reachable,
                                                  cancellable, error))
            return FALSE;
        }

      { OstreeRepoPruneOptions opts = { pruneflags, reachable };
        if (!ostree_repo_prune_from_reachable (repo, &opts,
                                               &n_objects_total,
                                               &n_objects_pruned,
                                               &objsize_total,
                                               cancellable, error))
          goto out;
      }
    }

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
  return ret;
}
