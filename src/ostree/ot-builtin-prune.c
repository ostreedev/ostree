/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
static char **opt_only_branches;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-prune.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "no-prune", 0, 0, G_OPTION_ARG_NONE, &opt_no_prune, "Only display unreachable objects; don't delete", NULL },
  { "refs-only", 0, 0, G_OPTION_ARG_NONE, &opt_refs_only, "Only compute reachability via refs", NULL },
  { "depth", 0, 0, G_OPTION_ARG_INT, &opt_depth, "Only traverse DEPTH parents for each commit (default: -1=infinite)", "DEPTH" },
  { "delete-commit", 0, 0, G_OPTION_ARG_STRING, &opt_delete_commit, "Specify a commit to delete", "COMMIT" },
  { "keep-younger-than", 0, 0, G_OPTION_ARG_STRING, &opt_keep_younger_than, "Prune all commits older than the specified date", "DATE" },
  { "static-deltas-only", 0, 0, G_OPTION_ARG_NONE, &opt_static_deltas_only, "Change the behavior of delete-commit and keep-younger-than to prune only static deltas" },
  { "retain-branch-depth", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_retain_branch_depth, "Additionally retain BRANCH=DEPTH commits", "BRANCH=DEPTH" },
  { "only-branch", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_only_branches, "Only prune BRANCH (may be specified multiple times)", "BRANCH" },
  { NULL }
};

static gboolean
delete_commit (OstreeRepo *repo, const char *commit_to_delete, GCancellable *cancellable, GError **error)
{
  g_autoptr(GHashTable) refs = NULL;  /* (element-type utf8 utf8) */
#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
  g_autoptr(GHashTable) collection_refs = NULL;  /* (element-type OstreeCollectionRef utf8) */
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */

  /* Check refs which are not in a collection. */
  if (!ostree_repo_list_refs (repo, NULL, &refs, cancellable, error))
    return FALSE;

  GLNX_HASH_TABLE_FOREACH_KV(refs, const char *, ref, const char *, commit)
    {
      if (g_strcmp0 (commit_to_delete, commit) == 0)
        return glnx_throw (error, "Commit '%s' is referenced by '%s'", commit_to_delete, ref);
    }

#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
  /* And check refs which *are* in a collection. */
  if (!ostree_repo_list_collection_refs (repo, NULL, &collection_refs,
                                         OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_REMOTES,
                                         cancellable, error))
    return FALSE;

  GLNX_HASH_TABLE_FOREACH_KV (collection_refs, const OstreeCollectionRef*, ref,
                              const char *, commit)
    {
      if (g_strcmp0 (commit_to_delete, commit) == 0)
        return glnx_throw (error, "Commit '%s' is referenced by (%s, %s)",
                           commit_to_delete, ref->collection_id, ref->ref_name);
    }
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */

  if (!ot_enable_tombstone_commits (repo, error))
    return FALSE;

  if (!ostree_repo_delete_object (repo, OSTREE_OBJECT_TYPE_COMMIT, commit_to_delete, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
traverse_keep_younger_than (OstreeRepo *repo, const char *checksum,
                            struct timespec *ts,
                            GHashTable *reachable,
                            GCancellable *cancellable, GError **error)
{
  g_autofree char *next_checksum = g_strdup (checksum);

  /* This is the first commit in our loop, which has a ref pointing to it. We
   * don't want to auto-prune it.
   */
  if (!ostree_repo_traverse_commit_union (repo, checksum, 0, reachable,
                                          cancellable, error))
    return FALSE;

  while (TRUE)
    {
      g_autoptr(GVariant) commit = NULL;
      if (!ostree_repo_load_variant_if_exists (repo, OSTREE_OBJECT_TYPE_COMMIT, next_checksum,
                                               &commit, error))
        return FALSE;
      if (!commit)
        break; /* This commit was pruned, so we're done */

      guint64 commit_timestamp = ostree_commit_get_timestamp (commit);
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
ostree_builtin_prune (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("");
  g_autoptr(OstreeRepo) repo = NULL;
  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    return FALSE;

  if (!opt_no_prune && !ostree_ensure_repo_writable (repo, error))
    return FALSE;

  /* Special handling for explicit commit deletion here - we do this
   * first.
   */
  if (opt_delete_commit)
    {
      if (opt_no_prune)
        {
          ot_util_usage_error (context, "Cannot specify both --delete-commit and --no-prune", error);
          return FALSE;
        }
        if (opt_static_deltas_only)
          {
            if(!ostree_repo_prune_static_deltas (repo, opt_delete_commit, cancellable, error))
              return FALSE;
          }
        else if (!delete_commit (repo, opt_delete_commit, cancellable, error))
          return FALSE;
    }
  else
    {
      /* In the future we should make this useful, but for now let's
       * error out since what we were doing before was very misleading.
       * https://github.com/ostreedev/ostree/issues/1479
       */
      if (opt_static_deltas_only)
        return glnx_throw (error, "--static-deltas-only requires --delete-commit; see https://github.com/ostreedev/ostree/issues/1479");
    }

  OstreeRepoPruneFlags pruneflags = 0;
  if (opt_refs_only)
    pruneflags |= OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY;
  if (opt_no_prune)
    pruneflags |= OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE;

  /* If no newer more complex options are specified, drop down to the original
   * prune API - both to avoid code duplication, and to keep it run from the
   * test suite.
   */
  gint n_objects_total;
  gint n_objects_pruned;
  guint64 objsize_total;
  if (!(opt_retain_branch_depth || opt_keep_younger_than || opt_only_branches))
    {
      if (!ostree_repo_prune (repo, pruneflags, opt_depth,
                              &n_objects_total, &n_objects_pruned, &objsize_total,
                              cancellable, error))
        return FALSE;
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
            return glnx_throw (error, "Could not parse '%s'", opt_keep_younger_than);
        }

      /* Process --retain-branch-depth */
      for (char **iter = opt_retain_branch_depth; iter && *iter; iter++)
        {
          /* bd should look like BRANCH=DEPTH where DEPTH is an int */
          const char *bd = *iter;
          const char *eq = strchr (bd, '=');
          if (!eq)
            return glnx_throw (error, "Invalid value %s, must specify BRANCH=DEPTH", bd);

          const char *depthstr = eq + 1;
          errno = EPERM;
          char *endptr;
          gint64 depth = g_ascii_strtoll (depthstr, &endptr, 10);
          if (depth == 0)
            {
              if (errno == EINVAL)
                return glnx_throw (error, "Out of range depth %s", depthstr);
              else if (endptr == depthstr)
                return glnx_throw (error, "Invalid depth %s", depthstr);
            }
          g_hash_table_insert (retain_branch_depth, g_strndup (bd, eq - bd),
                               GINT_TO_POINTER ((int)depth));
        }

      /* We start from the refs */
      /* FIXME: Do we also want to look at ostree_repo_list_collection_refs()? */
      if (!ostree_repo_list_refs (repo, NULL, &all_refs,
                                  cancellable, error))
        return FALSE;

      /* Process --only-branch. Note this combines with --retain-branch-depth; one
       * could do e.g.:
       *  * --only-branch exampleos/x86_64/foo
       *  * --only-branch exampleos/x86_64/bar
       *  * --retain-branch-depth exampleos/x86_64/foo=0
       *  * --depth 5
       * to prune exampleos/x86_64/foo to just the latest commit, and
       * exampleos/x86_64/bar to a depth of 5.
       */
      if (opt_only_branches)
        {
          /* Turn --only-branch into a set */
          g_autoptr(GHashTable) only_branches_set = g_hash_table_new (g_str_hash, g_str_equal);
          for (char **iter = opt_only_branches; iter && *iter; iter++)
            {
              const char *ref = *iter;
              /* Ensure the specified branch exists */
              if (!ostree_repo_resolve_rev (repo, ref, FALSE, NULL, error))
                return FALSE;
              g_hash_table_add (only_branches_set, (char*)ref);
            }

          /* Iterate over all refs, add equivalent of --retain-branch-depth=$ref=-1
           * if the ref isn't in --only-branch set and there wasn't already a
           * --retain-branch-depth specified for it.
           */
          GLNX_HASH_TABLE_FOREACH (all_refs, const char *, ref)
            {
              if (!g_hash_table_contains (only_branches_set, ref) &&
                  !g_hash_table_contains (retain_branch_depth, ref))
                {
                  g_hash_table_insert (retain_branch_depth, g_strdup (ref), GINT_TO_POINTER ((int)-1));
                }
            }
        }

      /* Traverse each ref, and gather all objects pointed to by it up to a
       * specific depth (if configured).
       */
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
                return FALSE;

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

      /* We've gathered the reachable set; start the prune ✀ */
      { OstreeRepoPruneOptions opts = { pruneflags, reachable };
        if (!ostree_repo_prune_from_reachable (repo, &opts,
                                               &n_objects_total,
                                               &n_objects_pruned,
                                               &objsize_total,
                                               cancellable, error))
          return FALSE;
      }
    }

  g_autofree char *formatted_freed_size = g_format_size_full (objsize_total, 0);
  g_print ("Total objects: %u\n", n_objects_total);
  if (n_objects_pruned == 0)
    g_print ("No unreachable objects\n");
  else if (pruneflags & OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE)
    g_print ("Would delete: %u objects, freeing %s\n",
             n_objects_pruned, formatted_freed_size);
  else
    g_print ("Deleted %u objects, %s freed\n",
             n_objects_pruned, formatted_freed_size);

  return TRUE;
}
