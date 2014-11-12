/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 * Copyright (C) 2014 James Antill <james@fedoraproject.org>
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
 */

#include "config.h"

#include "ot-builtins.h"
#include "ot-builtins-common.h"
#include "ostree.h"
#include "otutil.h"

static char *opt_subject;
static char *opt_body;
static char *opt_branch;
static char *opt_statoverride_file;
static char **opt_metadata_strings;
static char **opt_detached_metadata_strings;
static gboolean opt_skip_if_unchanged;
static gboolean opt_no_xattrs;
static gint opt_owner_uid = -1;
static gint opt_owner_gid = -1;
static gboolean opt_table_output;
#ifdef HAVE_GPGME
static char **opt_key_ids;
static char *opt_gpg_homedir;
#endif
static gboolean opt_generate_sizes;
static char *opt_old_parent;

#define ARG_EQ(x, y) (g_ascii_strcasecmp(x, y) == 0)

static GOptionEntry options[] = {
  { "subject", 's', 0, G_OPTION_ARG_STRING, &opt_subject, "One line subject", "subject" },
  { "body", 'm', 0, G_OPTION_ARG_STRING, &opt_body, "Full description", "body" },
  { "branch", 'b', 0, G_OPTION_ARG_STRING, &opt_branch, "Branch", "branch" },
  { "add-metadata-string", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_metadata_strings, "Append given key and value (in string format) to metadata", "KEY=VALUE" },
  { "add-detached-metadata-string", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_detached_metadata_strings, "Append given key and value (in string format) to detached metadata", "KEY=VALUE" },
  { "owner-uid", 0, 0, G_OPTION_ARG_INT, &opt_owner_uid, "Set file ownership user id", "UID" },
  { "owner-gid", 0, 0, G_OPTION_ARG_INT, &opt_owner_gid, "Set file ownership group id", "GID" },
  { "no-xattrs", 0, 0, G_OPTION_ARG_NONE, &opt_no_xattrs, "Do not import extended attributes", NULL },
  { "skip-if-unchanged", 0, 0, G_OPTION_ARG_NONE, &opt_skip_if_unchanged, "If the contents are unchanged from previous commit, do nothing", NULL },
  { "statoverride", 0, 0, G_OPTION_ARG_FILENAME, &opt_statoverride_file, "File containing list of modifications to make to permissions", "path" },
  { "table-output", 0, 0, G_OPTION_ARG_NONE, &opt_table_output, "Output more information in a KEY: VALUE format", NULL },
#ifdef HAVE_GPGME
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_key_ids, "GPG Key ID to sign the commit with", "key-id"},
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "homedir"},
#endif
  { "generate-sizes", 0, 0, G_OPTION_ARG_NONE, &opt_generate_sizes, "Generate size information along with commit metadata", NULL },
  { "old-parent", 0, 0, G_OPTION_ARG_STRING, &opt_old_parent, "Use an older parent", NULL },
  { NULL }
};

// Squash in ostree land is basically a cherry-pick with a higher parent.
static GOptionEntry squash_options[] = {
  { "table-output", 0, 0, G_OPTION_ARG_NONE, &opt_table_output, "Output more information in a KEY: VALUE format", NULL },
  { "branch", 'b', 0, G_OPTION_ARG_STRING, &opt_branch, "Branch", "branch" },
#ifdef HAVE_GPGME
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_key_ids, "GPG Key ID to sign the commit with", "key-id"},
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "homedir"},
#endif
  { "generate-sizes", 0, 0, G_OPTION_ARG_NONE, &opt_generate_sizes, "Generate size information along with commit metadata", NULL },
  { NULL }
};

static OstreeRepoCommitFilterResult
commit_filter (OstreeRepo         *self,
               const char         *path,
               GFileInfo          *file_info,
               gpointer            user_data)
{
  GHashTable *mode_adds = user_data;
  gpointer value;

  if (opt_owner_uid >= 0)
    g_file_info_set_attribute_uint32 (file_info, "unix::uid", opt_owner_uid);
  if (opt_owner_gid >= 0)
    g_file_info_set_attribute_uint32 (file_info, "unix::gid", opt_owner_gid);

  if (mode_adds && g_hash_table_lookup_extended (mode_adds, path, NULL, &value))
    {
      guint current_mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
      guint mode_add = GPOINTER_TO_UINT (value);
      g_file_info_set_attribute_uint32 (file_info, "unix::mode",
                                        current_mode | mode_add);
      g_hash_table_remove (mode_adds, path);
    }

  return OSTREE_REPO_COMMIT_FILTER_ALLOW;
}

static gboolean
check_revision_is_parent (OstreeRepo   *repo,
                          const char   *descendant,
                          const char   *ancestor,
                          GCancellable *cancellable,
                          GError      **error)
{
  gs_free char *parent = NULL;
  gs_unref_variant GVariant *variant = NULL;
  gboolean ret = FALSE;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 descendant, &variant, error))
    goto out;

  parent = ostree_commit_get_parent (variant);
  if (!parent)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "The ref does not have this commit as an ancestor: %s", ancestor);
      goto out;
    }

  if (!g_str_equal (parent, ancestor) &&
      !check_revision_is_parent (repo, parent, ancestor, cancellable, error))
    goto out;

  ret = TRUE;
out:
  return ret;
}

static gboolean
parse_keyvalue_strings (char             **strings,
                        GVariant         **out_metadata,
                        GError           **error)
{
  gboolean ret = FALSE;
  char **iter;
  gs_unref_variant_builder GVariantBuilder *builder = NULL;

  builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

  for (iter = strings; *iter; iter++)
    {
      const char *s;
      const char *eq;
      gs_free char *key = NULL;

      s = *iter;

      eq = strchr (s, '=');
      if (!eq)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Missing '=' in KEY=VALUE metadata '%s'", s);
          goto out;
        }

      key = g_strndup (s, eq - s);
      g_variant_builder_add (builder, "{sv}", key,
                             g_variant_new_string (eq + 1));
    }

  ret = TRUE;
  *out_metadata = g_variant_builder_end (builder);
  g_variant_ref_sink (*out_metadata);
 out:
  return ret;
}

static gboolean
ostree_builtin_cherry_pick_int (int argc, char **argv, OstreeRepo *repo,
                                GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  gboolean skip_commit = FALSE;
  gs_unref_object GFile *arg = NULL;
  gs_free char *found_parent = NULL;
  const char *parent = NULL;
  gs_free char *commit_checksum = NULL;
  gs_unref_object GFile *root = NULL;
  gs_unref_variant GVariant *metadata = NULL;
  gs_unref_variant GVariant *detached_metadata = NULL;
  gs_unref_object OstreeMutableTree *mtree = NULL;
  gs_unref_hashtable GHashTable *mode_adds = NULL;
  OstreeRepoCommitModifierFlags flags = 0;
  OstreeRepoCommitModifier *modifier = NULL;
  OstreeRepoTransactionStats stats;
  const char *cherry_rev = NULL;
  gs_free char *cherry_commit = NULL;
  gs_free gchar *subject_dup = NULL;
  gs_free gchar *body_dup = NULL;

  if (opt_statoverride_file)
    {
      if (!ot_common_parse_statoverride_file (opt_statoverride_file,
                                              &mode_adds, cancellable, error))
        goto out;
    }

  if (opt_metadata_strings)
    {
      if (!parse_keyvalue_strings (opt_metadata_strings,
                                   &metadata, error))
        goto out;
    }
  if (opt_detached_metadata_strings)
    {
      if (!parse_keyvalue_strings (opt_detached_metadata_strings,
                                   &detached_metadata, error))
        goto out;
    }

  if (!opt_branch)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "A branch must be specified with --branch");
      goto out;
    }

  if (opt_no_xattrs)
    flags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS;
  if (opt_generate_sizes)
    flags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_GENERATE_SIZES;
  if (FALSE)
    ostree_repo_set_disable_fsync (repo, TRUE);

  if (flags != 0
      || opt_owner_uid >= 0
      || opt_owner_gid >= 0
      || opt_statoverride_file != NULL
      || opt_no_xattrs)
    {
      modifier = ostree_repo_commit_modifier_new (flags, commit_filter,
                                                  mode_adds, NULL);
    }

  if (!ostree_repo_resolve_rev (repo, opt_branch, TRUE, &found_parent, error))
    goto out;

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  if ((argc <= 1) && opt_old_parent)
    cherry_rev = opt_branch;
  else if (argc <= 1)
  {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "A REFSPEC must be specified");
    goto out;
  }
  else
    cherry_rev = argv[1];

  if (!ostree_repo_read_commit (repo, cherry_rev, &arg, &cherry_commit,
                                cancellable, error))
    goto out;

  if (!opt_metadata_strings)
    { // Load from cherry-pick commit.
      gs_unref_variant GVariant *variant = NULL;

      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                     cherry_commit,
                                     &variant, error))
        goto out;

      metadata = g_variant_get_child_value (variant, 0);
    }
  if (!opt_detached_metadata_strings)
    { // Load from cherry-pick commit.
      if (!ostree_repo_read_commit_detached_metadata (repo, cherry_commit,
                                                      &detached_metadata,
                                                      NULL, error))
        goto out;
    }

  if (!opt_subject)
  {
    gs_unref_variant GVariant *variant = NULL;
    const gchar *subject;
    const gchar *body;
    guint64 timestamp;

    if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                   cherry_commit,
                                   &variant, error))
      goto out;

    /* See OSTREE_COMMIT_GVARIANT_FORMAT */
    g_variant_get (variant, "(a{sv}aya(say)&s&stayay)", NULL, NULL, NULL,
                   &subject, &body, &timestamp, NULL, NULL);

    opt_subject = g_strdup (subject);
    opt_body    = g_strdup (body);
    // FIXME: Can't use timestamp as API doesn't allow it.
  }

  if (!opt_subject && !opt_body)
    {
      if (!ot_common_commit_editor (repo, opt_branch, &opt_subject, &opt_body,
                                    cancellable, error))
        goto out;
    }

  if (!opt_subject)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "A subject must be specified with --subject");
      goto out;
    }

  mtree = ostree_mutable_tree_new ();
  if (!ostree_repo_write_directory_to_mtree (repo, arg, mtree, modifier,
                                             cancellable, error))
    goto out;

  if (mode_adds && g_hash_table_size (mode_adds) > 0)
    {
      GHashTableIter hash_iter;
      gpointer key, value;

      g_hash_table_iter_init (&hash_iter, mode_adds);

      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          g_printerr ("Unmatched statoverride path: %s\n", (char*)key);
        }
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unmatched statoverride paths");
      goto out;
    }

  if (found_parent && opt_old_parent)
    {
      if (!check_revision_is_parent (repo, found_parent, opt_old_parent, 
                                     cancellable, error))
        goto out;

      parent = opt_old_parent;
    }
  else
    parent = found_parent;

  if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
    goto out;

  if (opt_skip_if_unchanged && parent)
    {
      gs_unref_object GFile *parent_root;

      if (!ostree_repo_read_commit (repo, parent, &parent_root, NULL, cancellable, error))
        goto out;

      if (g_file_equal (root, parent_root))
        skip_commit = TRUE;
    }

  if (!skip_commit)
    {
      if (!ostree_repo_write_commit (repo, parent, opt_subject, opt_body, metadata,
                                     OSTREE_REPO_FILE (root),
                                     &commit_checksum, cancellable, error))
        goto out;

      if (detached_metadata)
        {
          if (!ostree_repo_write_commit_detached_metadata (repo, commit_checksum,
                                                           detached_metadata,
                                                           cancellable, error))
            goto out;
        }

#ifdef HAVE_GPGME
      if (opt_key_ids)
        {
          char **iter;

          for (iter = opt_key_ids; iter && *iter; iter++)
            {
              const char *keyid = *iter;

              if (!ostree_repo_sign_commit (repo,
                                            commit_checksum,
                                            keyid,
                                            opt_gpg_homedir,
                                            cancellable,
                                            error))
                goto out;
            }
        }
#endif

      ostree_repo_transaction_set_ref (repo, NULL, opt_branch, commit_checksum);

      if (!ostree_repo_commit_transaction (repo, &stats, cancellable, error))
        goto out;
    }
  else
    {
      commit_checksum = g_strdup (parent);
    }

  if (opt_table_output)
    {
      g_print ("Commit: %s\n", commit_checksum);
      g_print ("Metadata Total: %u\n", stats.metadata_objects_total);
      g_print ("Metadata Written: %u\n", stats.metadata_objects_written);
      g_print ("Content Total: %u\n", stats.content_objects_total);
      g_print ("Content Written: %u\n", stats.content_objects_written);
      g_print ("Content Bytes Written: %" G_GUINT64_FORMAT "\n", stats.content_bytes_written);
    }
  else
    {
      g_print ("%s\n", commit_checksum);
    }

  ret = TRUE;
 out:
  ostree_repo_abort_transaction (repo, cancellable, NULL);
  if (modifier)
    ostree_repo_commit_modifier_unref (modifier);
  return ret;
}

gboolean
ostree_builtin_cherry_pick (int argc, char **argv, OstreeRepo *repo,
                            GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;

  context = g_option_context_new ("[ARG] - Commit a new revision");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  ret = ostree_builtin_cherry_pick_int (argc, argv, repo, cancellable, error);

 out:
  if (context)
    g_option_context_free (context);
  return ret;
}

gboolean
ostree_builtin_squash (int argc, char **argv, OstreeRepo *repo,
                       GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;

  context = g_option_context_new ("[ARG] - Commit a new revision");
  g_option_context_add_main_entries (context, squash_options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc <= 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "A REFSPEC must be specified");
      goto out;
    }
  opt_old_parent = argv[1];
  argc = 0; // Don't allow extra arguments, as it'll be confusing. Use cherry.
  ++argv;

  ret = ostree_builtin_cherry_pick_int (0, argv, repo, cancellable, error);

 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
