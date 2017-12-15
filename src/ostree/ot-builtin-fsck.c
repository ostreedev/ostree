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
#include "ostree-cmdprivate.h"
#include "otutil.h"

static gboolean opt_quiet;
static gboolean opt_delete;
static gboolean opt_add_tombstones;
static gboolean opt_verify_bindings;
static gboolean opt_verify_back_refs;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-fsck.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "add-tombstones", 0, 0, G_OPTION_ARG_NONE, &opt_add_tombstones, "Add tombstones for missing commits", NULL },
  { "quiet", 'q', 0, G_OPTION_ARG_NONE, &opt_quiet, "Only print error messages", NULL },
  { "delete", 0, 0, G_OPTION_ARG_NONE, &opt_delete, "Remove corrupted objects", NULL },
  { "verify-bindings", 0, 0, G_OPTION_ARG_NONE, &opt_verify_bindings, "Verify ref bindings", NULL },
  { "verify-back-refs", 0, 0, G_OPTION_ARG_NONE, &opt_verify_back_refs, "Verify back-references (implies --verify-bindings)", NULL },
  { NULL }
};

static gboolean
fsck_one_object (OstreeRepo            *repo,
                 const char            *checksum,
                 OstreeObjectType       objtype,
                 gboolean              *out_found_corruption,
                 GCancellable          *cancellable,
                 GError               **error)
{
  g_autoptr(GError) temp_error = NULL;
  if (!ostree_repo_fsck_object (repo, objtype, checksum, cancellable, &temp_error))
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
          g_printerr ("Object missing: %s.%s\n", checksum,
                      ostree_object_type_to_string (objtype));
          *out_found_corruption = TRUE;
        }
      else
        {
          if (opt_delete)
            {
              g_printerr ("%s\n", temp_error->message);
              (void) ostree_repo_delete_object (repo, objtype, checksum, cancellable, NULL);
              *out_found_corruption = TRUE;
            }
          else
            {
              g_propagate_error (error, g_steal_pointer (&temp_error));
              return FALSE;
            }
        }
    }

  return TRUE;
}

static gboolean
fsck_reachable_objects_from_commits (OstreeRepo            *repo,
                                     GHashTable            *commits,
                                     gboolean              *out_found_corruption,
                                     GCancellable          *cancellable,
                                     GError               **error)
{
  g_autoptr(GHashTable) reachable_objects = ostree_repo_traverse_new_reachable ();

  GHashTableIter hash_iter;
  gpointer key, value;
  g_hash_table_iter_init (&hash_iter, commits);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      g_assert (objtype == OSTREE_OBJECT_TYPE_COMMIT);

      if (!ostree_repo_traverse_commit_union (repo, checksum, 0, reachable_objects,
                                              cancellable, error))
        return FALSE;
    }

  g_auto(GLnxConsoleRef) console = { 0, };
  glnx_console_lock (&console);

  const guint count = g_hash_table_size (reachable_objects);
  guint i = 0;
  g_hash_table_iter_init (&hash_iter, reachable_objects);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      if (!fsck_one_object (repo, checksum, objtype, out_found_corruption,
                            cancellable, error))
        return FALSE;

      i++;
      glnx_console_progress_n_items ("fsck objects", i, count);
    }

  return TRUE;
}

/* Check that a given commit object is valid for the ref it was looked up via.
 * @collection_id will be %NULL for normal refs, and non-%NULL for collection–refs. */
static gboolean
fsck_commit_for_ref (OstreeRepo    *repo,
                     const char    *checksum,
                     const char    *collection_id,
                     const char    *ref_name,
                     gboolean      *found_corruption,
                     GCancellable  *cancellable,
                     GError       **error)
{
  if (!fsck_one_object (repo, checksum, OSTREE_OBJECT_TYPE_COMMIT,
                        found_corruption,
                        cancellable, error))
    return FALSE;

  /* Check the commit exists. */
  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 checksum, &commit, error))
    {
      if (collection_id != NULL)
        return glnx_prefix_error (error, "Loading commit for ref (%s, %s)",
                                  collection_id, ref_name);
      else
        return glnx_prefix_error (error, "Loading commit for ref %s", ref_name);
    }

  /* Check its bindings. */
  if (opt_verify_bindings)
    {
      if (!ostree_cmd__private__ ()->ostree_repo_verify_bindings (collection_id, ref_name, commit, error))
        return glnx_prefix_error (error, "Commit %s", checksum);
    }

  return TRUE;
}

gboolean
ostree_builtin_fsck (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(OstreeRepo) repo = NULL;
  gboolean found_corruption = FALSE;

  g_autoptr(GOptionContext) context = g_option_context_new ("");
  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    return FALSE;

  if (!opt_quiet)
    g_print ("Validating refs...\n");

  /* Validate that the commit for each ref is available */
  g_autoptr(GHashTable) all_refs = NULL;
  if (!ostree_repo_list_refs (repo, NULL, &all_refs,
                              cancellable, error))
    return FALSE;

  GHashTableIter hash_iter;
  gpointer key, value;
  g_hash_table_iter_init (&hash_iter, all_refs);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *refspec = key;
      const char *checksum = value;
      g_autofree char *ref_name = NULL;
      if (!ostree_parse_refspec (refspec, NULL, &ref_name, error))
        return FALSE;
      if (!fsck_commit_for_ref (repo, checksum, NULL, ref_name,
                                &found_corruption, cancellable, error))
        return FALSE;
    }

#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
  if (!opt_quiet)
    g_print ("Validating refs in collections...\n");

  g_autoptr(GHashTable) all_collection_refs = NULL;  /* (element-type OstreeCollectionRef utf8) */
  if (!ostree_repo_list_collection_refs (repo, NULL, &all_collection_refs,
                                         OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_REMOTES,
                                         cancellable, error))
    return FALSE;

  g_hash_table_iter_init (&hash_iter, all_collection_refs);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const OstreeCollectionRef *ref = key;
      if (!fsck_commit_for_ref (repo, value, ref->collection_id, ref->ref_name,
                                &found_corruption, cancellable, error))
        return FALSE;
    }
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */

  if (!opt_quiet)
    g_print ("Enumerating objects...\n");

  g_autoptr(GHashTable) objects = NULL;
  if (!ostree_repo_list_objects (repo, OSTREE_REPO_LIST_OBJECTS_ALL,
                                 &objects, cancellable, error))
    return FALSE;

  g_autoptr(GHashTable) commits = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                                         (GDestroyNotify)g_variant_unref, NULL);


  g_autoptr(GPtrArray) tombstones = NULL;
  if (opt_add_tombstones)
    tombstones = g_ptr_array_new_with_free_func (g_free);

  if (opt_verify_back_refs)
    opt_verify_bindings = TRUE;

  guint n_partial = 0;
  g_hash_table_iter_init (&hash_iter, objects);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;
      OstreeRepoCommitState commitstate = 0;
      g_autoptr(GVariant) commit = NULL;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
        {
          if (!ostree_repo_load_commit (repo, checksum, &commit, &commitstate, error))
            return FALSE;

          /* If requested, check that all the refs listed in the ref-bindings
           * for this commit resolve back to this commit. */
          if (opt_verify_back_refs)
            {
              g_autoptr(GVariant) metadata = g_variant_get_child_value (commit, 0);

              const char *collection_id = NULL;
#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
              if (!g_variant_lookup (metadata,
                                     OSTREE_COMMIT_META_KEY_COLLECTION_BINDING,
                                     "&s",
                                     &collection_id))
                collection_id = NULL;
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */

              g_autofree const char **refs = NULL;
              if (g_variant_lookup (metadata,
                                    OSTREE_COMMIT_META_KEY_REF_BINDING,
                                    "^a&s",
                                    &refs))
                {
                  for (const char **iter = refs; *iter != NULL; ++iter)
                    {
                      g_autofree char *checksum_for_ref = NULL;

#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
                      if (collection_id != NULL)
                        {
                          const OstreeCollectionRef collection_ref = { (char *) collection_id, (char *) *iter };
                          if (!ostree_repo_resolve_collection_ref (repo, &collection_ref,
                                                                   TRUE,
                                                                   OSTREE_REPO_RESOLVE_REV_EXT_NONE,
                                                                   &checksum_for_ref,
                                                                   cancellable,
                                                                   error))
                            return FALSE;
                        }
                      else
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */
                        {
                          if (!ostree_repo_resolve_rev (repo, *iter, TRUE,
                                                        &checksum_for_ref, error))
                            return FALSE;
                        }

                      if (checksum_for_ref == NULL)
                        {
                          if (collection_id != NULL)
                            return glnx_throw (error,
                                               "Collection–ref (%s, %s) in bindings for commit %s does not exist",
                                               collection_id, *iter, checksum);
                          else
                            return glnx_throw (error,
                                               "Ref ‘%s’ in bindings for commit %s does not exist",
                                               *iter, checksum);
                        }
                      else if (g_strcmp0 (checksum_for_ref, checksum) != 0)
                        {
                          if (collection_id != NULL)
                            return glnx_throw (error,
                                               "Collection–ref (%s, %s) in bindings for commit %s does not resolve to that commit",
                                               collection_id, *iter, checksum);
                          else
                            return glnx_throw (error,
                                               "Ref ‘%s’ in bindings for commit %s does not resolve to that commit",
                                               *iter, checksum);
                        }
                    }
                }
            }

          if (opt_add_tombstones)
            {
              GError *local_error = NULL;
              g_autofree char *parent = ostree_commit_get_parent (commit);
              if (parent)
                {
                  g_autoptr(GVariant) parent_commit = NULL;
                  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, parent,
                                                 &parent_commit, &local_error))
                    {
                      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                        {
                          g_ptr_array_add (tombstones, g_strdup (checksum));
                          g_clear_error (&local_error);
                        }
                      else
                        {
                          g_propagate_error (error, local_error);
                          return FALSE;
                        }
                    }
                }
            }

          if (commitstate & OSTREE_REPO_COMMIT_STATE_PARTIAL)
            n_partial++;
          else
            g_hash_table_add (commits, g_variant_ref (serialized_key));
        }
    }

  g_clear_pointer (&objects, (GDestroyNotify) g_hash_table_unref);

  if (!opt_quiet)
    g_print ("Verifying content integrity of %u commit objects...\n",
             (guint)g_hash_table_size (commits));

  if (!fsck_reachable_objects_from_commits (repo, commits, &found_corruption,
                                            cancellable, error))
    return FALSE;

  if (opt_add_tombstones)
    {
      guint i;
      if (tombstones->len)
        {
          if (!ot_enable_tombstone_commits (repo, error))
            return FALSE;
        }
      for (i = 0; i < tombstones->len; i++)
        {
          const char *checksum = tombstones->pdata[i];
          g_print ("Adding tombstone for commit %s\n", checksum);
          if (!ostree_repo_delete_object (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum, cancellable, error))
            return FALSE;
        }
    }
  else if (n_partial > 0)
    {
      g_print ("%u partial commits not verified\n", n_partial);
    }

  if (found_corruption)
    return glnx_throw (error, "Repository corruption encountered");

  return TRUE;
}
