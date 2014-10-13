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
#include "ot-editor.h"
#include "ostree.h"
#include "otutil.h"

static char *opt_subject;
static char *opt_body;
static char *opt_branch;
static char *opt_statoverride_file;
static char **opt_metadata_strings;
static char **opt_detached_metadata_strings;
static gboolean opt_link_checkout_speedup;
static gboolean opt_skip_if_unchanged;
static gboolean opt_tar_autocreate_parents;
static gboolean opt_no_xattrs;
static char **opt_trees;
static gint opt_owner_uid = -1;
static gint opt_owner_gid = -1;
static gboolean opt_table_output;
#ifdef HAVE_GPGME
static char **opt_key_ids;
static char *opt_gpg_homedir;
#endif
static gboolean opt_generate_sizes;
static gboolean opt_disable_fsync;

#define ARG_EQ(x, y) (g_ascii_strcasecmp(x, y) == 0)
/* create a function to parse the --fsync option, and current parse it the
 * same as --disable-fsync. Allows us to add other things later, and not have
 * a double negative. */
static gboolean opt__fsync(const gchar *option_name,
			   const gchar *value,
			   gpointer data,
			   GError **error)
{
  g_assert(g_str_equal(option_name, "--fsync"));

  if (0) {}
  else if (ARG_EQ(value, "1"))
    opt_disable_fsync = 0;
  else if (ARG_EQ(value, "true"))
    opt_disable_fsync = 0;
  else if (ARG_EQ(value, "yes"))
    opt_disable_fsync = 0;
  else if (ARG_EQ(value, "0"))
    opt_disable_fsync = 1;
  else if (ARG_EQ(value, "false"))
    opt_disable_fsync = 1;
  else if (ARG_EQ(value, "none"))
    opt_disable_fsync = 1;
  else if (ARG_EQ(value, "no"))
    opt_disable_fsync = 1;
  else
    /* do we want to complain here? */
    return 0;


  return 1;
}

static GOptionEntry options[] = {
  { "subject", 's', 0, G_OPTION_ARG_STRING, &opt_subject, "One line subject", "subject" },
  { "body", 'm', 0, G_OPTION_ARG_STRING, &opt_body, "Full description", "body" },
  { "branch", 'b', 0, G_OPTION_ARG_STRING, &opt_branch, "Branch", "branch" },
  { "tree", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_trees, "Overlay the given argument as a tree", "NAME" },
  { "add-metadata-string", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_metadata_strings, "Append given key and value (in string format) to metadata", "KEY=VALUE" },
  { "add-detached-metadata-string", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_detached_metadata_strings, "Append given key and value (in string format) to detached metadata", "KEY=VALUE" },
  { "owner-uid", 0, 0, G_OPTION_ARG_INT, &opt_owner_uid, "Set file ownership user id", "UID" },
  { "owner-gid", 0, 0, G_OPTION_ARG_INT, &opt_owner_gid, "Set file ownership group id", "GID" },
  { "no-xattrs", 0, 0, G_OPTION_ARG_NONE, &opt_no_xattrs, "Do not import extended attributes", NULL },
  { "link-checkout-speedup", 0, 0, G_OPTION_ARG_NONE, &opt_link_checkout_speedup, "Optimize for commits of trees composed of hardlinks into the repository", NULL },
  { "tar-autocreate-parents", 0, 0, G_OPTION_ARG_NONE, &opt_tar_autocreate_parents, "When loading tar archives, automatically create parent directories as needed", NULL },
  { "skip-if-unchanged", 0, 0, G_OPTION_ARG_NONE, &opt_skip_if_unchanged, "If the contents are unchanged from previous commit, do nothing", NULL },
  { "statoverride", 0, 0, G_OPTION_ARG_FILENAME, &opt_statoverride_file, "File containing list of modifications to make to permissions", "path" },
  { "table-output", 0, 0, G_OPTION_ARG_NONE, &opt_table_output, "Output more information in a KEY: VALUE format", NULL },
#ifdef HAVE_GPGME
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_key_ids, "GPG Key ID to sign the commit with", "key-id"},
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "homedir"},
#endif
  { "generate-sizes", 0, 0, G_OPTION_ARG_NONE, &opt_generate_sizes, "Generate size information along with commit metadata", NULL },
  { "disable-fsync", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_disable_fsync, "Do not invoke fsync()", NULL },
  { "fsync", 0, 0, G_OPTION_ARG_CALLBACK, opt__fsync, "Specify how to invoke fsync()", NULL },
  { NULL }
};

static gboolean
parse_statoverride_file (GHashTable   **out_mode_add,
                         GCancellable  *cancellable,
                         GError        **error)
{
  gboolean ret = FALSE;
  gsize len;
  char **iter = NULL; /* nofree */
  gs_unref_hashtable GHashTable *ret_hash = NULL;
  gs_unref_object GFile *path = NULL;
  gs_free char *contents = NULL;
  char **lines = NULL;

  path = g_file_new_for_path (opt_statoverride_file);

  if (!g_file_load_contents (path, cancellable, &contents, &len, NULL,
                             error))
    goto out;
  
  ret_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  lines = g_strsplit (contents, "\n", -1);

  for (iter = lines; iter && *iter; iter++)
    {
      const char *line = *iter;

      if (*line == '+')
        {
          const char *spc;
          guint mode_add;

          spc = strchr (line + 1, ' ');
          if (!spc)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Malformed statoverride file");
              goto out;
            }
          
          mode_add = (guint32)(gint32)g_ascii_strtod (line + 1, NULL);
          g_hash_table_insert (ret_hash,
                               g_strdup (spc + 1),
                               GUINT_TO_POINTER((gint32)mode_add));
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_mode_add, &ret_hash);
 out:
  g_strfreev (lines);
  return ret;
}

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
commit_editor (OstreeRepo     *repo,
               const char     *branch,
               char          **subject,
               char          **body,
               GCancellable   *cancellable,
               GError        **error)
{
  gs_free char *input = NULL;
  gs_free char *output = NULL;
  gboolean ret = FALSE;
  GString *bodybuf = NULL;
  char **lines = NULL;
  int i;

  *subject = NULL;
  *body = NULL;

  input = g_strdup_printf ("\n"
      "# Please enter the commit message for your changes. The first line will\n"
      "# become the subject, and the remainder the body. Lines starting\n"
      "# with '#' will be ignored, and an empty message aborts the commit.\n"
      "#\n"
      "# Branch: %s\n", branch);

  output = ot_editor_prompt (repo, input, cancellable, error);
  if (output == NULL)
    goto out;

  lines = g_strsplit (output, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      g_strchomp (lines[i]);

      /* Lines starting with # are skipped */
      if (lines[i][0] == '#')
        continue;

      /* Blank lines before body starts are skipped */
      if (lines[i][0] == '\0')
        {
          if (!bodybuf)
            continue;
        }

      if (!*subject)
        {
          *subject = g_strdup (lines[i]);
        }
      else if (!bodybuf)
        {
          bodybuf = g_string_new (lines[i]);
        }
      else
        {
          g_string_append_c (bodybuf, '\n');
          g_string_append (bodybuf, lines[i]);
        }
    }

  if (!*subject)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Aborting commit due to empty commit subject.");
      goto out;
    }

  if (bodybuf)
    {
      *body = g_string_free (bodybuf, FALSE);
      g_strchomp (*body);
      bodybuf = NULL;
    }

  ret = TRUE;

out:
  g_strfreev (lines);
  if (bodybuf)
    g_string_free (bodybuf, TRUE);
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
metadata_version_unique (OstreeRepo  *repo,
                         const char  *checksum,
                         const char  *version,
                         GError     **error)
{
  gs_unref_variant GVariant *variant = NULL;
  gs_unref_variant GVariant *metadata = NULL;
  gs_unref_variant GVariant *value = NULL;
  gs_free gchar *parent = NULL;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                 &variant, error))
    {
      if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (error);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Do not have full history to validate version metadata is unique.");
        }
      goto out;
    }

  metadata = g_variant_get_child_value (variant, 0);
  if ((value = g_variant_lookup_value (metadata, "version", NULL)))
    if (g_str_equal (version, g_variant_get_string (value, NULL)))
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Version already specified in commit %s", checksum);
        goto out;
      }
  
  if (!(parent = ostree_commit_get_parent (variant)))
    return TRUE;
  
  return metadata_version_unique (repo, parent, version, error);

 out:
  return FALSE;
}

gboolean
ostree_builtin_commit (int argc, char **argv, OstreeRepo *repo, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  gboolean skip_commit = FALSE;
  gs_unref_object GFile *arg = NULL;
  gs_free char *parent = NULL;
  gs_free char *commit_checksum = NULL;
  gs_unref_object GFile *root = NULL;
  gs_unref_variant GVariant *metadata = NULL;
  gs_unref_variant GVariant *detached_metadata = NULL;
  gs_unref_object OstreeMutableTree *mtree = NULL;
  gs_free char *tree_type = NULL;
  gs_unref_hashtable GHashTable *mode_adds = NULL;
  OstreeRepoCommitModifierFlags flags = 0;
  OstreeRepoCommitModifier *modifier = NULL;
  OstreeRepoTransactionStats stats;

  context = g_option_context_new ("[ARG] - Commit a new revision");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (opt_statoverride_file)
    {
      if (!parse_statoverride_file (&mode_adds, cancellable, error))
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
  if (opt_disable_fsync)
    ostree_repo_set_disable_fsync (repo, TRUE);

  if (flags != 0
      || opt_owner_uid >= 0
      || opt_owner_gid >= 0
      || opt_statoverride_file != NULL
      || opt_no_xattrs)
    {
      modifier = ostree_repo_commit_modifier_new (flags, commit_filter, mode_adds, NULL);
    }

  if (!ostree_repo_resolve_rev (repo, opt_branch, TRUE, &parent, error))
    goto out;

  if (metadata && parent)
    {
      gs_unref_variant GVariant *md_version = g_variant_lookup_value (metadata,
                                                                      "version",
                                                                      NULL);
      const char *version = g_variant_get_string (md_version, NULL);

      if (!metadata_version_unique (repo, parent, version, error))
        goto out;
    }

  if (!opt_subject && !opt_body)
    {
      if (!commit_editor (repo, opt_branch, &opt_subject, &opt_body, cancellable, error))
        goto out;
    }

  if (!opt_subject)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "A subject must be specified with --subject");
      goto out;
    }

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  if (opt_link_checkout_speedup && !ostree_repo_scan_hardlinks (repo, cancellable, error))
    goto out;

  mtree = ostree_mutable_tree_new ();

  if (argc <= 1 && (opt_trees == NULL || opt_trees[0] == NULL))
    {
      char *current_dir = g_get_current_dir ();
      arg = g_file_new_for_path (current_dir);
      g_free (current_dir);

      if (!ostree_repo_write_directory_to_mtree (repo, arg, mtree, modifier,
                                                 cancellable, error))
        goto out;
    }
  else if (opt_trees != NULL)
    {
      const char *const*tree_iter;
      const char *tree;
      const char *eq;

      for (tree_iter = (const char *const*)opt_trees; *tree_iter; tree_iter++)
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
              arg = g_file_new_for_path (tree);
              if (!ostree_repo_write_directory_to_mtree (repo, arg, mtree, modifier,
                                                         cancellable, error))
                goto out;
            }
          else if (strcmp (tree_type, "tar") == 0)
            {
              arg = g_file_new_for_path (tree);
              if (!ostree_repo_write_archive_to_mtree (repo, arg, mtree, modifier,
                                                       opt_tar_autocreate_parents,
                                                       cancellable, error))
                goto out;
            }
          else if (strcmp (tree_type, "ref") == 0)
            {
              if (!ostree_repo_read_commit (repo, tree, &arg, NULL, cancellable, error))
                goto out;

              if (!ostree_repo_write_directory_to_mtree (repo, arg, mtree, modifier,
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
  else
    {
      g_assert (argc > 1);
      arg = g_file_new_for_path (argv[1]);
      if (!ostree_repo_write_directory_to_mtree (repo, arg, mtree, modifier,
                                                 cancellable, error))
        goto out;
    }

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
  if (context)
    g_option_context_free (context);
  if (modifier)
    ostree_repo_commit_modifier_unref (modifier);
  return ret;
}
