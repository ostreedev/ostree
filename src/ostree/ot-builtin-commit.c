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

#include <gio/gunixoutputstream.h>

#include <glib/gi18n.h>

static char *metadata_text_path;
static char *metadata_bin_path;
static char *subject;
static char *body;
static char *parent;
static char *branch;
static char **metadata_strings;
static gboolean skip_if_unchanged;
static gboolean tar_autocreate_parents;
static char **trees;
static gint owner_uid = -1;
static gint owner_gid = -1;

static GOptionEntry options[] = {
  { "subject", 's', 0, G_OPTION_ARG_STRING, &subject, "One line subject", "subject" },
  { "body", 'm', 0, G_OPTION_ARG_STRING, &body, "Full description", "body" },
  { "metadata-variant-text", 0, 0, G_OPTION_ARG_FILENAME, &metadata_text_path, "File containing g_variant_print() output", "path" },
  { "metadata-variant", 0, 0, G_OPTION_ARG_FILENAME, &metadata_bin_path, "File containing serialized variant, in host endianness", "path" },
  { "add-metadata-string", 0, 0, G_OPTION_ARG_STRING_ARRAY, &metadata_strings, "Append given key and value (in string format) to metadata", "KEY=VALUE" },
  { "branch", 'b', 0, G_OPTION_ARG_STRING, &branch, "Branch", "branch" },
  { "parent", 'p', 0, G_OPTION_ARG_STRING, &parent, "Parent commit", "commit" },
  { "tree", 0, 0, G_OPTION_ARG_STRING_ARRAY, &trees, "Overlay the given argument as a tree", "NAME" },
  { "owner-uid", 0, 0, G_OPTION_ARG_INT, &owner_uid, "Set file ownership user id", "UID" },
  { "owner-gid", 0, 0, G_OPTION_ARG_INT, &owner_gid, "Set file ownership group id", "GID" },
  { "tar-autocreate-parents", 0, 0, G_OPTION_ARG_NONE, &tar_autocreate_parents, "When loading tar archives, automatically create parent directories as needed", NULL },
  { "skip-if-unchanged", 0, 0, G_OPTION_ARG_NONE, &skip_if_unchanged, "If the contents are unchanged from previous commit, do nothing", NULL },
  { NULL }
};

gboolean
ostree_builtin_commit (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  GFile *arg = NULL;
  char *parent = NULL;
  char *commit_checksum = NULL;
  GVariant *parent_commit = NULL;
  GVariant *metadata = NULL;
  GMappedFile *metadata_mappedf = NULL;
  GFile *metadata_f = NULL;
  OstreeRepoCommitModifier *modifier = NULL;
  char *contents_checksum = NULL;
  GCancellable *cancellable = NULL;
  OstreeMutableTree *mtree = NULL;
  char *tree_type = NULL;
  GVariantBuilder metadata_builder;
  gboolean metadata_builder_initialized = FALSE;
  gboolean skip_commit = FALSE;

  context = g_option_context_new ("[ARG] - Commit a new revision");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (metadata_text_path || metadata_bin_path)
    {
      metadata_mappedf = g_mapped_file_new (metadata_text_path ? metadata_text_path : metadata_bin_path, FALSE, error);
      if (!metadata_mappedf)
        goto out;
      if (metadata_text_path)
        {
          metadata = g_variant_parse (G_VARIANT_TYPE ("a{sv}"),
                                      g_mapped_file_get_contents (metadata_mappedf),
                                      g_mapped_file_get_contents (metadata_mappedf) + g_mapped_file_get_length (metadata_mappedf),
                                      NULL, error);
          if (!metadata)
            goto out;
        }
      else if (metadata_bin_path)
        {
          metadata_f = ot_gfile_new_for_path (metadata_bin_path);
          if (!ot_util_variant_map (metadata_f, G_VARIANT_TYPE ("a{sv}"), &metadata, error))
            goto out;
        }
      else
        g_assert_not_reached ();
    }
  else if (metadata_strings)
    {
      char **iter;

      metadata_builder_initialized = TRUE;
      g_variant_builder_init (&metadata_builder, G_VARIANT_TYPE ("a{sv}"));

      for (iter = metadata_strings; *iter; iter++)
        {
          const char *s;
          const char *eq;
          char *key;

          s = *iter;

          eq = strchr (s, '=');
          if (!eq)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Missing '=' in KEY=VALUE metadata '%s'", s);
              goto out;
            }
          
          key = g_strndup (s, eq - s);
          g_variant_builder_add (&metadata_builder, "{sv}", key,
                                 g_variant_new_string (eq + 1));
        }
      metadata = g_variant_builder_end (&metadata_builder);
      metadata_builder_initialized = FALSE;
    }

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (!branch)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "A branch must be specified with --branch");
      goto out;
    }

  if (!subject)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "A subject must be specified with --subject");
      goto out;
    }

  if (owner_uid >= 0 || owner_gid >= 0)
    {
      modifier = ostree_repo_commit_modifier_new ();
      modifier->uid = owner_uid;
      modifier->gid = owner_gid;
    }

  if (!ostree_repo_resolve_rev (repo, branch, TRUE, &parent, error))
    goto out;

  if (skip_if_unchanged && parent)
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                     parent, &parent_commit, error))
        goto out;
    }

  if (!ostree_repo_prepare_transaction (repo, cancellable, error))
    goto out;

  mtree = ostree_mutable_tree_new ();

  if (argc == 1 && (trees == NULL || trees[0] == NULL))
    {
      char *current_dir = g_get_current_dir ();
      arg = ot_gfile_new_for_path (current_dir);
      g_free (current_dir);

      if (!ostree_repo_stage_directory_to_mtree (repo, arg, mtree, modifier,
                                                 cancellable, error))
        goto out;
    }
  else
    {
      const char *const*tree_iter;
      const char *tree;
      const char *eq;

      for (tree_iter = (const char *const*)trees; *tree_iter; tree_iter++)
        {
          tree = *tree_iter;

          eq = strchr (tree, '=');
          if (!eq)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Missing type in tree specification '%s'", tree);
              goto out;
            }
          g_free (tree_type);
          tree_type = g_strndup (tree, eq - tree);
          tree = eq + 1;

          g_clear_object (&arg);
          if (strcmp (tree_type, "dir") == 0)
            {
              arg = ot_gfile_new_for_path (tree);
              if (!ostree_repo_stage_directory_to_mtree (repo, arg, mtree, modifier,
                                                         cancellable, error))
                goto out;
            }
          else if (strcmp (tree_type, "tar") == 0)
            {
              arg = ot_gfile_new_for_path (tree);
              if (!ostree_repo_stage_archive_to_mtree (repo, arg, mtree, modifier,
                                                       tar_autocreate_parents,
                                                       cancellable, error))
                goto out;
            }
          else if (strcmp (tree_type, "ref") == 0)
            {
              if (!ostree_repo_read_commit (repo, tree, &arg, cancellable, error))
                goto out;

              if (!ostree_repo_stage_directory_to_mtree (repo, arg, mtree, modifier,
                                                         cancellable, error))
                goto out;
            }
          else
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid tree type specification '%s'", tree_type);
              goto out;
            }
        }
    }
          
  if (!ostree_repo_stage_mtree (repo, mtree, &contents_checksum, cancellable, error))
    goto out;

  if (skip_if_unchanged && parent_commit)
    {
      const char *parent_contents_checksum;
      const char *parent_metadata_checksum;

      g_variant_get_child (parent_commit, 6, "&s", &parent_contents_checksum);
      g_variant_get_child (parent_commit, 7, "&s", &parent_metadata_checksum);

      if (strcmp (contents_checksum, parent_contents_checksum) == 0
          && strcmp (ostree_mutable_tree_get_metadata_checksum (mtree),
                     parent_metadata_checksum) == 0)
        skip_commit = TRUE;
    }

  if (!skip_commit)
    {
      const char *root_metadata = ostree_mutable_tree_get_metadata_checksum (mtree);
      
      if (!root_metadata)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Can't commit an empty tree");
          goto out;
        }

      if (!ostree_repo_stage_commit (repo, branch, parent, subject, body, metadata,
                                     contents_checksum, root_metadata,
                                     &commit_checksum, cancellable, error))
        goto out;

      if (!ostree_repo_commit_transaction (repo, cancellable, error))
        goto out;
      
      if (!ostree_repo_write_ref (repo, NULL, branch, commit_checksum, error))
        goto out;

      g_print ("%s\n", commit_checksum);
    }
  else
    {
      if (!ostree_repo_abort_transaction (repo, cancellable, error))
        goto out;

      g_print ("%s\n", parent);
    }

  ret = TRUE;
 out:
  if (metadata_builder_initialized)
    g_variant_builder_clear (&metadata_builder);
  g_clear_object (&arg);
  g_clear_object (&mtree);
  g_free (contents_checksum);
  g_free (parent);
  ot_clear_gvariant(&parent_commit);
  g_free (tree_type);
  if (metadata_mappedf)
    g_mapped_file_unref (metadata_mappedf);
  if (context)
    g_option_context_free (context);
  if (modifier)
    ostree_repo_commit_modifier_unref (modifier);
  g_clear_object (&repo);
  g_free (commit_checksum);
  return ret;
}
