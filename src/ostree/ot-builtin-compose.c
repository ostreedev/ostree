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
static gboolean recompose;

static GOptionEntry options[] = {
  { "subject", 's', 0, G_OPTION_ARG_STRING, &subject, "One line subject", "subject" },
  { "body", 'm', 0, G_OPTION_ARG_STRING, &body, "Full description", "body" },
  { "branch", 'b', 0, G_OPTION_ARG_STRING, &branch, "Branch", "branch" },
  { "recompose", 0, 0, G_OPTION_ARG_NONE, &recompose, "Regenerate compose from existing branches", NULL },
  { NULL }
};

static gboolean
add_branch (OstreeRepo          *repo,
            OstreeMutableTree   *mtree,
            const char          *branch,
            GVariantBuilder     *metadata_builder,
            GError             **error)
{
  gboolean ret = FALSE;
  GFile *branchf = NULL;
  const char *branch_rev;

  if (!ostree_repo_read_commit (repo, branch, &branchf, NULL, error))
    goto out;

  branch_rev = ostree_repo_file_get_commit ((OstreeRepoFile*)branchf);

  if (!ostree_repo_stage_directory_to_mtree (repo, branchf, mtree, NULL,
                                             NULL, error))
    goto out;
  
  if (metadata_builder)
    g_variant_builder_add (metadata_builder, "(ss)", branch, branch_rev);

  ret = TRUE;
 out:
  g_clear_object (&branchf);
  return ret;
}

gboolean
ostree_builtin_compose (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  OstreeCheckout *checkout = NULL;
  char *parent = NULL;
  GFile *destf = NULL;
  gboolean compose_metadata_builder_initialized = FALSE;
  GVariantBuilder compose_metadata_builder;
  gboolean commit_metadata_builder_initialized = FALSE;
  GVariantBuilder commit_metadata_builder;
  GVariant *parent_commit = NULL;
  GVariant *parent_commit_metadata = NULL;
  GVariant *parent_commit_compose = NULL;
  GVariant *commit_metadata = NULL;
  GVariantIter *parent_commit_compose_iter = NULL;
  char *contents_checksum = NULL;
  char *commit_checksum = NULL;
  GCancellable *cancellable = NULL;
  GFile *metadata_f = NULL;
  OstreeMutableTree *mtree = NULL;
  int i;

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

  if (!ostree_repo_resolve_rev (repo, branch, recompose ? FALSE : TRUE, &parent, error))
    goto out;

  if (!ostree_repo_prepare_transaction (repo, cancellable, error))
    goto out;

  mtree = ostree_mutable_tree_new ();

  if (recompose)
    {
      const char *branch_name;
      const char *branch_rev;

      g_assert (parent);
      
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, parent,
                                     &parent_commit, error))
        goto out;

      g_variant_get_child (parent_commit, 1, "@a{sv}", &parent_commit_metadata);

      parent_commit_compose = g_variant_lookup_value (parent_commit_metadata,
                                                      "ostree-compose", G_VARIANT_TYPE ("a(ss)"));

      if (!parent_commit_compose)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Commit '%s' doesn't have ostree-compose metadata",
                       parent);
          goto out;
        }

      parent_commit_compose_iter = g_variant_iter_new (parent_commit_compose);

      while (g_variant_iter_loop (parent_commit_compose_iter, "(&s&s)",
                                  &branch_name, &branch_rev))
        {
          if (!add_branch (repo, mtree, branch_name,
                           &compose_metadata_builder,
                           error))
            goto out;
        }
    }
  
  for (i = 1; i < argc; i++)
    {
      const char *src_branch = argv[i];
      
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

  if (!ostree_repo_stage_commit (repo, branch, parent, subject, body, commit_metadata,
                                 contents_checksum,
                                 ostree_mutable_tree_get_metadata_checksum (mtree),
                                 &commit_checksum, cancellable, error))
    goto out;

  if (!ostree_repo_commit_transaction (repo, cancellable, error))
    goto out;

  if (!ostree_repo_write_ref (repo, NULL, branch, commit_checksum, error))
    goto out;

  ret = TRUE;
  g_print ("%s\n", commit_checksum);
 out:
  if (compose_metadata_builder_initialized)
    g_variant_builder_clear (&compose_metadata_builder);
  if (commit_metadata_builder_initialized)
    g_variant_builder_clear (&commit_metadata_builder);
  if (context)
    g_option_context_free (context);
  g_free (parent);
  g_free (contents_checksum);
  g_free (commit_checksum);
  ot_clear_gvariant (&commit_metadata);
  ot_clear_gvariant (&parent_commit);
  ot_clear_gvariant (&parent_commit_metadata);
  ot_clear_gvariant (&parent_commit_compose);
  if (parent_commit_compose_iter)
    g_variant_iter_free (parent_commit_compose_iter);
  g_clear_object (&repo);
  g_clear_object (&checkout);
  g_clear_object (&destf);
  g_clear_object (&metadata_f);
  g_clear_object (&mtree);
  return ret;
}
