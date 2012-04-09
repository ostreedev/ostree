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

#include <glib/gi18n.h>

static char *subject;
static char *body;
static char *branch;
static char *from_file_path;

static GOptionEntry options[] = {
  { "subject", 's', 0, G_OPTION_ARG_STRING, &subject, "One line subject", "subject" },
  { "body", 'm', 0, G_OPTION_ARG_STRING, &body, "Full description", "body" },
  { "branch", 'b', 0, G_OPTION_ARG_STRING, &branch, "Branch", "branch" },
  { "from-file", 'F', 0, G_OPTION_ARG_STRING, &from_file_path, "Take list of branches to compose from FILE", "FILE" },
  { NULL }
};

static gboolean
add_branch (OstreeRepo          *repo,
            OstreeMutableTree   *mtree,
            const char          *branch_path,
            GVariantBuilder     *metadata_builder,
            GError             **error)
{
  gboolean ret = FALSE;
  const char *branch_rev;
  const char *branch_name;
  const char *path;
  ot_lobj GFile *branchf = NULL;
  ot_lobj GFile *subdir = NULL;
  char **components = NULL;

  components = g_strsplit (branch_path, ":", 2);

  if (g_strv_length (components) != 2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid compose specification '%s'; missing ':'",
                   branch_path);
      goto out;
    }
  branch_name = components[0];
  path = components[1];

  if (!ostree_repo_read_commit (repo, branch_name, &branchf, NULL, error))
    goto out;

  branch_rev = ostree_repo_file_get_commit ((OstreeRepoFile*)branchf);
  subdir = g_file_resolve_relative_path (branchf, path);

  if (!ostree_repo_stage_directory_to_mtree (repo, subdir, mtree, NULL,
                                             NULL, error))
    {
      g_prefix_error (error, "Error composing %s: ", branch_path);
      goto out;
    }  

  if (metadata_builder)
    g_variant_builder_add (metadata_builder, "(ss)", branch_path, branch_rev);

  ret = TRUE;
 out:
  g_strfreev (components);
  return ret;
}

gboolean
ostree_builtin_compose (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  GCancellable *cancellable = NULL;
  int i;
  gboolean skip_commit = FALSE;
  gboolean in_transaction = FALSE;
  ot_lobj OstreeRepo *repo = NULL;
  ot_lfree char *parent = NULL;
  ot_lobj GFile *destf = NULL;
  ot_lhash GHashTable *seen_branches = NULL;
  ot_lvariant GVariant *parent_commit = NULL;
  ot_lvariant GVariant *parent_commit_metadata = NULL;
  ot_lvariant GVariant *parent_commit_compose = NULL;
  ot_lvariant GVariant *commit_metadata = NULL;
  ot_lfree char *contents_checksum = NULL;
  ot_lfree char *commit_checksum = NULL;
  ot_lobj GFile *metadata_f = NULL;
  ot_lobj GFile *from_file = NULL;
  ot_lfree char *from_file_contents = NULL;
  gboolean compose_metadata_builder_initialized = FALSE;
  GVariantBuilder compose_metadata_builder;
  gboolean commit_metadata_builder_initialized = FALSE;
  GVariantBuilder commit_metadata_builder;
  GVariantIter *parent_commit_compose_iter = NULL;
  char **from_file_args = NULL;
  OstreeMutableTree *mtree = NULL;

  context = g_option_context_new ("BRANCH1 BRANCH2 ... - Merge multiple commits into a single commit tree");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

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

  compose_metadata_builder_initialized = TRUE;
  g_variant_builder_init (&compose_metadata_builder, G_VARIANT_TYPE ("a(ss)"));

  if (!ostree_repo_resolve_rev (repo, branch, TRUE, &parent, error))
    goto out;

  if (parent)
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                     parent, &parent_commit, error))
        goto out;
    }

  if (!ostree_repo_prepare_transaction (repo, cancellable, error))
    goto out;

  in_transaction = TRUE;

  mtree = ostree_mutable_tree_new ();

  if (from_file_path)
    {
      char **iter;

      from_file = ot_gfile_new_for_path (from_file_path);
      if (!ot_gfile_load_contents_utf8 (from_file,
                                        &from_file_contents, NULL, NULL, error))
        goto out;
      
      from_file_args = g_strsplit_set (from_file_contents, "\n", -1);

      for (iter = from_file_args; *iter && **iter; iter++)
        {
          const char *src_branch = *iter;

          if (seen_branches && g_hash_table_lookup (seen_branches, src_branch))
            continue;
          
          if (!add_branch (repo, mtree, src_branch,
                           &compose_metadata_builder,
                           error))
            goto out;
        }
    }
  
  for (i = 1; i < argc; i++)
    {
      const char *src_branch = argv[i];

      if (seen_branches && g_hash_table_lookup (seen_branches, src_branch))
        continue;
      
      if (!add_branch (repo, mtree, src_branch,
                       &compose_metadata_builder,
                       error))
        goto out;
    }

  commit_metadata_builder_initialized = TRUE;
  g_variant_builder_init (&commit_metadata_builder, G_VARIANT_TYPE ("a{sv}"));

  g_variant_builder_add (&commit_metadata_builder, "{sv}",
                         "ostree-compose", g_variant_builder_end (&compose_metadata_builder));
  commit_metadata = g_variant_builder_end (&commit_metadata_builder);
  g_variant_ref_sink (commit_metadata);

  if (!ostree_repo_stage_mtree (repo, mtree, &contents_checksum, cancellable, error))
    goto out;

  if (parent_commit)
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

      if (!ostree_repo_stage_commit (repo, branch, parent, subject, body, commit_metadata,
                                     contents_checksum,
                                     root_metadata,
                                     &commit_checksum, cancellable, error))
        goto out;
      
      if (!ostree_repo_commit_transaction (repo, cancellable, error))
        goto out;

      in_transaction = FALSE;

      if (!ostree_repo_write_ref (repo, NULL, branch, commit_checksum, error))
        goto out;

      g_print ("%s\n", commit_checksum);
    }
  else
    {
      if (!ostree_repo_abort_transaction (repo, cancellable, error))
        goto out;

      in_transaction = FALSE;
      
      g_print ("%s\n", parent);
    }

  ret = TRUE;
 out:
  if (in_transaction)
    {
      (void) ostree_repo_abort_transaction (repo, cancellable, NULL);
    }
  if (compose_metadata_builder_initialized)
    g_variant_builder_clear (&compose_metadata_builder);
  if (commit_metadata_builder_initialized)
    g_variant_builder_clear (&commit_metadata_builder);
  if (context)
    g_option_context_free (context);
  if (parent_commit_compose_iter)
    g_variant_iter_free (parent_commit_compose_iter);
  g_strfreev (from_file_args);
  return ret;
}
