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
#include "ot-editor.h"
#include "ostree.h"
#include "otutil.h"
#include "ot-tool-util.h"
#include "parse-datetime.h"
#include "ostree-repo-private.h"
#include "ostree-libarchive-private.h"

static char *opt_subject;
static char *opt_body;
static char *opt_body_file;
static gboolean opt_editor;
static char *opt_parent;
static gboolean opt_orphan;
static char **opt_bind_refs;
static char *opt_branch;
static char *opt_statoverride_file;
static char *opt_skiplist_file;
static char **opt_metadata_strings;
static char **opt_detached_metadata_strings;
static gboolean opt_link_checkout_speedup;
static gboolean opt_skip_if_unchanged;
static gboolean opt_tar_autocreate_parents;
static char *opt_tar_pathname_filter;
static gboolean opt_no_xattrs;
static char *opt_selinux_policy;
static gboolean opt_canonical_permissions;
static gboolean opt_consume;
static gboolean opt_devino_canonical;
static char **opt_trees;
static gint opt_owner_uid = -1;
static gint opt_owner_gid = -1;
static gboolean opt_table_output;
static char **opt_key_ids;
static char *opt_gpg_homedir;
static gboolean opt_generate_sizes;
static gboolean opt_disable_fsync;
static char *opt_timestamp;

static gboolean
parse_fsync_cb (const char  *option_name,
                const char  *value,
                gpointer     data,
                GError     **error)
{
  gboolean val;
  if (!ot_parse_boolean (value, &val, error))
    return FALSE;

  opt_disable_fsync = !val;
  return TRUE;
}

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-commit.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "parent", 0, 0, G_OPTION_ARG_STRING, &opt_parent, "Parent ref, or \"none\"", "REF" },
  { "subject", 's', 0, G_OPTION_ARG_STRING, &opt_subject, "One line subject", "SUBJECT" },
  { "body", 'm', 0, G_OPTION_ARG_STRING, &opt_body, "Full description", "BODY" },
  { "body-file", 'F', 0, G_OPTION_ARG_FILENAME, &opt_body_file, "Commit message from FILE path", "FILE" },
  { "editor", 'e', 0, G_OPTION_ARG_NONE, &opt_editor, "Use an editor to write the commit message", NULL },
  { "branch", 'b', 0, G_OPTION_ARG_STRING, &opt_branch, "Branch", "BRANCH" },
  { "orphan", 0, 0, G_OPTION_ARG_NONE, &opt_orphan, "Create a commit without writing a ref", NULL },
  { "bind-ref", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_bind_refs, "Add a ref to ref binding commit metadata", "BRANCH" },
  { "tree", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_trees, "Overlay the given argument as a tree", "dir=PATH or tar=TARFILE or ref=COMMIT" },
  { "add-metadata-string", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_metadata_strings, "Add a key/value pair to metadata", "KEY=VALUE" },
  { "add-detached-metadata-string", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_detached_metadata_strings, "Add a key/value pair to detached metadata", "KEY=VALUE" },
  { "owner-uid", 0, 0, G_OPTION_ARG_INT, &opt_owner_uid, "Set file ownership user id", "UID" },
  { "owner-gid", 0, 0, G_OPTION_ARG_INT, &opt_owner_gid, "Set file ownership group id", "GID" },
  { "canonical-permissions", 0, 0, G_OPTION_ARG_NONE, &opt_canonical_permissions, "Canonicalize permissions in the same way bare-user does for hardlinked files", NULL },
  { "no-xattrs", 0, 0, G_OPTION_ARG_NONE, &opt_no_xattrs, "Do not import extended attributes", NULL },
  { "selinux-policy", 0, 0, G_OPTION_ARG_FILENAME, &opt_selinux_policy, "Set SELinux labels based on policy in root filesystem PATH (may be /)", "PATH" },
  { "link-checkout-speedup", 0, 0, G_OPTION_ARG_NONE, &opt_link_checkout_speedup, "Optimize for commits of trees composed of hardlinks into the repository", NULL },
  { "devino-canonical", 'I', 0, G_OPTION_ARG_NONE, &opt_devino_canonical, "Assume hardlinked objects are unmodified.  Implies --link-checkout-speedup", NULL },
  { "tar-autocreate-parents", 0, 0, G_OPTION_ARG_NONE, &opt_tar_autocreate_parents, "When loading tar archives, automatically create parent directories as needed", NULL },
  { "tar-pathname-filter", 0, 0, G_OPTION_ARG_STRING, &opt_tar_pathname_filter, "When loading tar archives, use REGEX,REPLACEMENT against path names", "REGEX,REPLACEMENT" },
  { "skip-if-unchanged", 0, 0, G_OPTION_ARG_NONE, &opt_skip_if_unchanged, "If the contents are unchanged from previous commit, do nothing", NULL },
  { "statoverride", 0, 0, G_OPTION_ARG_FILENAME, &opt_statoverride_file, "File containing list of modifications to make to permissions", "PATH" },
  { "skip-list", 0, 0, G_OPTION_ARG_FILENAME, &opt_skiplist_file, "File containing list of files to skip", "PATH" },
  { "consume", 0, 0, G_OPTION_ARG_NONE, &opt_consume, "Consume (delete) content after commit (for local directories)", NULL },
  { "table-output", 0, 0, G_OPTION_ARG_NONE, &opt_table_output, "Output more information in a KEY: VALUE format", NULL },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_key_ids, "GPG Key ID to sign the commit with", "KEY-ID"},
  { "gpg-homedir", 0, 0, G_OPTION_ARG_FILENAME, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "HOMEDIR"},
  { "generate-sizes", 0, 0, G_OPTION_ARG_NONE, &opt_generate_sizes, "Generate size information along with commit metadata", NULL },
  { "disable-fsync", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_disable_fsync, "Do not invoke fsync()", NULL },
  { "fsync", 0, 0, G_OPTION_ARG_CALLBACK, parse_fsync_cb, "Specify how to invoke fsync()", "POLICY" },
  { "timestamp", 0, 0, G_OPTION_ARG_STRING, &opt_timestamp, "Override the timestamp of the commit", "TIMESTAMP" },
  { NULL }
};

static gboolean
parse_file_by_line (const char    *path,
                    gboolean     (*cb)(const char*, void*, GError**),
                    void          *cbdata,
                    GCancellable  *cancellable,
                    GError       **error)
{
  g_autofree char *contents =
    glnx_file_get_contents_utf8_at (AT_FDCWD, path, NULL, cancellable, error);
  if (!contents)
    return FALSE;

  g_auto(GStrv) lines = g_strsplit (contents, "\n", -1);
  for (char **iter = lines; iter && *iter; iter++)
    {
      /* skip empty lines at least */
      if (**iter == '\0')
        continue;

      if (!cb (*iter, cbdata, error))
        return FALSE;
    }

  return TRUE;
}

struct CommitFilterData {
  GHashTable *mode_adds;
  GHashTable *mode_overrides;
  GHashTable *skip_list;
};

static gboolean
handle_statoverride_line (const char  *line,
                          void        *data,
                          GError     **error)
{
  struct CommitFilterData *cf = data;
  const char *spc = strchr (line, ' ');
  if (spc == NULL)
    return glnx_throw (error, "Malformed statoverride file (no space found)");
  const char *fn = spc + 1;

  if (g_str_has_prefix (line, "="))
    {
      guint mode_override = (guint32)(gint32)g_ascii_strtod (line+1, NULL);
      g_hash_table_insert (cf->mode_overrides, g_strdup (fn),
                           GUINT_TO_POINTER((gint32)mode_override));
    }
  else
    {
      guint mode_add = (guint32)(gint32)g_ascii_strtod (line, NULL);
      g_hash_table_insert (cf->mode_adds, g_strdup (fn),
                           GUINT_TO_POINTER((gint32)mode_add));
    }
  return TRUE;
}

static gboolean
handle_skiplist_line (const char  *line,
                      void        *data,
                      GError     **error)
{
  GHashTable *files = data;
  g_hash_table_add (files, g_strdup (line));
  return TRUE;
}

static OstreeRepoCommitFilterResult
commit_filter (OstreeRepo         *self,
               const char         *path,
               GFileInfo          *file_info,
               gpointer            user_data)
{
  struct CommitFilterData *data = user_data;
  GHashTable *mode_adds = data->mode_adds;
  GHashTable *mode_overrides = data->mode_overrides;
  GHashTable *skip_list = data->skip_list;
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
  else if (mode_overrides && g_hash_table_lookup_extended (mode_overrides, path, NULL, &value))
    {
      guint current_fmt = g_file_info_get_attribute_uint32 (file_info, "unix::mode") & S_IFMT;
      guint mode_override = GPOINTER_TO_UINT (value);
      g_file_info_set_attribute_uint32 (file_info, "unix::mode",
                                        current_fmt | mode_override);
      g_hash_table_remove (mode_adds, path);
    }

  if (skip_list && g_hash_table_contains (skip_list, path))
    {
      g_hash_table_remove (skip_list, path);
      return OSTREE_REPO_COMMIT_FILTER_SKIP;
    }

  return OSTREE_REPO_COMMIT_FILTER_ALLOW;
}

#ifdef HAVE_LIBARCHIVE
typedef struct {
  GRegex *regex;
  const char *replacement;
} TranslatePathnameData;

/* Implement --tar-pathname-filter */
static char *
handle_translate_pathname (OstreeRepo *repo,
                           const struct stat *stbuf,
                           const char *path,
                           gpointer user_data)
{
  TranslatePathnameData *tpdata = user_data;
  g_autoptr(GError) tmp_error = NULL;
  char *ret =
    g_regex_replace (tpdata->regex, path, -1, 0,
                     tpdata->replacement, 0, &tmp_error);
  g_assert_no_error (tmp_error);
  g_assert (ret);
  return ret;
}
#endif

static gboolean
commit_editor (OstreeRepo     *repo,
               const char     *branch,
               char          **subject,
               char          **body,
               GCancellable   *cancellable,
               GError        **error)
{
  g_autofree char *input = g_strdup_printf ("\n"
      "# Please enter the commit message for your changes. The first line will\n"
      "# become the subject, and the remainder the body. Lines starting\n"
      "# with '#' will be ignored, and an empty message aborts the commit."
      "%s%s%s%s%s%s\n"
              , branch ? "\n#\n# Branch: " : "", branch ? branch : ""
              , *subject ? "\n" : "", *subject ? *subject : ""
              , *body ? "\n" : "", *body ? *body : ""
              );

  *subject = NULL;
  *body = NULL;

  g_autofree char *output = ot_editor_prompt (repo, input, cancellable, error);
  if (output == NULL)
    return FALSE;

  g_auto(GStrv) lines = g_strsplit (output, "\n", -1);
  g_autoptr(GString) bodybuf = NULL;
  for (guint i = 0; lines[i] != NULL; i++)
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
    return glnx_throw (error, "Aborting commit due to empty commit subject.");

  if (bodybuf)
    {
      *body = g_string_free (g_steal_pointer (&bodybuf), FALSE);
      g_strchomp (*body);
    }

  return TRUE;
}

static gboolean
parse_keyvalue_strings (char             **strings,
                        GVariant         **out_metadata,
                        GError           **error)
{
  g_autoptr(GVariantBuilder) builder =
    g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

  for (char ** iter = strings; *iter; iter++)
    {
      const char *s = *iter;
      const char *eq = strchr (s, '=');
      if (!eq)
        return glnx_throw (error, "Missing '=' in KEY=VALUE metadata '%s'", s);
      g_autofree char *key = g_strndup (s, eq - s);
      g_variant_builder_add (builder, "{sv}", key,
                             g_variant_new_string (eq + 1));
    }

  *out_metadata = g_variant_ref_sink (g_variant_builder_end (builder));
  return TRUE;
}

#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
static void
add_collection_binding (OstreeRepo       *repo,
                        GVariantBuilder  *metadata_builder)
{
  const char *collection_id = ostree_repo_get_collection_id (repo);

  if (collection_id == NULL)
    return;

  g_variant_builder_add (metadata_builder, "{s@v}", OSTREE_COMMIT_META_KEY_COLLECTION_BINDING,
                         g_variant_new_variant (g_variant_new_string (collection_id)));
}
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */

static int
compare_strings (gconstpointer a, gconstpointer b)
{
  const char **sa = (const char **)a;
  const char **sb = (const char **)b;

  return strcmp (*sa, *sb);
}

static void
add_ref_binding (GVariantBuilder *metadata_builder)
{
  if (opt_orphan)
    return;

  g_assert_nonnull (opt_branch);

  g_autoptr(GPtrArray) refs = g_ptr_array_new ();
  g_ptr_array_add (refs, opt_branch);
  for (char **iter = opt_bind_refs; iter != NULL && *iter != NULL; ++iter)
    g_ptr_array_add (refs, *iter);
  g_ptr_array_sort (refs, compare_strings);
  g_autoptr(GVariant) refs_v = g_variant_new_strv ((const char *const *)refs->pdata,
                                                   refs->len);
  g_variant_builder_add (metadata_builder, "{s@v}", OSTREE_COMMIT_META_KEY_REF_BINDING,
                         g_variant_new_variant (g_steal_pointer (&refs_v)));
}

/* Note if you're using the API, you currently need to do this yourself */
static void
fill_bindings (OstreeRepo    *repo,
               GVariant      *metadata,
               GVariant     **out_metadata)
{
  g_autoptr(GVariantBuilder) metadata_builder =
    ot_util_variant_builder_from_variant (metadata, G_VARIANT_TYPE_VARDICT);

  add_ref_binding (metadata_builder);

#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
  /* Allow the collection ID to be overridden using
   * --add-metadata-string=ostree.collection-binding=blah */
  if (metadata == NULL ||
      !g_variant_lookup (metadata, OSTREE_COMMIT_META_KEY_COLLECTION_BINDING, "*", NULL))
    add_collection_binding (repo, metadata_builder);
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */

  *out_metadata = g_variant_ref_sink (g_variant_builder_end (metadata_builder));
}

gboolean
ostree_builtin_commit (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  gboolean ret = FALSE;
  gboolean skip_commit = FALSE;
  g_autoptr(GFile) object_to_commit = NULL;
  g_autofree char *parent = NULL;
  g_autofree char *commit_checksum = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(GVariant) detached_metadata = NULL;
  g_autoptr(OstreeMutableTree) mtree = NULL;
  g_autofree char *tree_type = NULL;
  g_autoptr(GHashTable) mode_adds = NULL;
  g_autoptr(GHashTable) mode_overrides = NULL;
  g_autoptr(GHashTable) skip_list = NULL;
  OstreeRepoCommitModifierFlags flags = 0;
  g_autoptr(OstreeSePolicy) policy = NULL;
  OstreeRepoCommitModifier *modifier = NULL;
  OstreeRepoTransactionStats stats;
  struct CommitFilterData filter_data = { 0, };
  g_autofree char *commit_body = NULL;

  context = g_option_context_new ("[PATH]");

  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    goto out;

  if (!ostree_ensure_repo_writable (repo, error))
    goto out;

  if (opt_statoverride_file)
    {
      filter_data.mode_adds = mode_adds = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      filter_data.mode_overrides = mode_overrides = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      if (!parse_file_by_line (opt_statoverride_file, handle_statoverride_line,
                               &filter_data, cancellable, error))
        goto out;
    }

  if (opt_skiplist_file)
    {
      skip_list = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      if (!parse_file_by_line (opt_skiplist_file, handle_skiplist_line,
                               skip_list, cancellable, error))
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

  if (!(opt_branch || opt_orphan))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "A branch must be specified with --branch, or use --orphan");
      goto out;
    }

  if (opt_no_xattrs)
    flags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS;
  if (opt_consume)
    flags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CONSUME;
  if (opt_devino_canonical)
    {
      opt_link_checkout_speedup = TRUE; /* Imply this */
      flags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_DEVINO_CANONICAL;
    }
  if (opt_canonical_permissions)
    flags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS;
  if (opt_generate_sizes)
    flags |= OSTREE_REPO_COMMIT_MODIFIER_FLAGS_GENERATE_SIZES;
  if (opt_disable_fsync)
    ostree_repo_set_disable_fsync (repo, TRUE);

  if (flags != 0
      || opt_owner_uid >= 0
      || opt_owner_gid >= 0
      || opt_statoverride_file != NULL
      || opt_skiplist_file != NULL
      || opt_no_xattrs
      || opt_selinux_policy)
    {
      filter_data.mode_adds = mode_adds;
      filter_data.skip_list = skip_list;
      modifier = ostree_repo_commit_modifier_new (flags, commit_filter,
                                                  &filter_data, NULL);
      if (opt_selinux_policy)
        {
          glnx_autofd int rootfs_dfd = -1;
          if (!glnx_opendirat (AT_FDCWD, opt_selinux_policy, TRUE, &rootfs_dfd, error))
            {
              g_prefix_error (error, "selinux-policy: ");
              goto out;
            }
          policy = ostree_sepolicy_new_at (rootfs_dfd, cancellable, error);
          if (!policy)
            goto out;
          ostree_repo_commit_modifier_set_sepolicy (modifier, policy);
        }
    }

  if (opt_parent)
    {
      if (g_str_equal (opt_parent, "none"))
        parent = NULL;
      else
        {
          if (!ostree_validate_checksum_string (opt_parent, error))
            goto out;
          parent = g_strdup (opt_parent);
        }
    }
  else if (!opt_orphan)
    {
      if (!ostree_repo_resolve_rev (repo, opt_branch, TRUE, &parent, error))
        {
          if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY))
            {
              /* A folder exists with the specified ref name,
                 * which is handled by _ostree_repo_write_ref */
              g_clear_error (error);
            }
          else goto out;
        }
    }

  if (opt_editor)
    {
      if (!commit_editor (repo, opt_branch, &opt_subject, &commit_body, cancellable, error))
        goto out;
    }
  else if (opt_body_file)
    {
      commit_body = glnx_file_get_contents_utf8_at (AT_FDCWD, opt_body_file, NULL,
                                                    cancellable, error);
      if (!commit_body)
        goto out;
    }
  else if (opt_body)
    commit_body = g_strdup (opt_body);

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  if (opt_link_checkout_speedup && !ostree_repo_scan_hardlinks (repo, cancellable, error))
    goto out;

  mtree = ostree_mutable_tree_new ();

  if (argc <= 1 && (opt_trees == NULL || opt_trees[0] == NULL))
    {
      if (!ostree_repo_write_dfd_to_mtree (repo, AT_FDCWD, ".", mtree, modifier,
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

          g_clear_object (&object_to_commit);
          if (strcmp (tree_type, "dir") == 0)
            {
              if (!ostree_repo_write_dfd_to_mtree (repo, AT_FDCWD, tree, mtree, modifier,
                                                   cancellable, error))
                goto out;
            }
          else if (strcmp (tree_type, "tar") == 0)
            {
              if (!opt_tar_pathname_filter)
                {
                  object_to_commit = g_file_new_for_path (tree);
                  if (!ostree_repo_write_archive_to_mtree (repo, object_to_commit, mtree, modifier,
                                                           opt_tar_autocreate_parents,
                                                           cancellable, error))
                    goto out;
                }
              else
                {
#ifdef HAVE_LIBARCHIVE
                  const char *comma = strchr (opt_tar_pathname_filter, ',');
                  if (!comma)
                    {
                      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                           "Missing ',' in --tar-pathname-filter");
                      goto out;
                    }
                  const char *replacement = comma + 1;
                  g_autofree char *regexp_text = g_strndup (opt_tar_pathname_filter, comma - opt_tar_pathname_filter);
                  /* Use new API if we have a pathname filter */
                  OstreeRepoImportArchiveOptions opts = { 0, };
                  opts.autocreate_parents = opt_tar_autocreate_parents;
                  opts.translate_pathname = handle_translate_pathname;
                  g_autoptr(GRegex) regexp = g_regex_new (regexp_text, 0, 0, error);
                  TranslatePathnameData tpdata = { regexp, replacement };
                  if (!regexp)
                    {
                      g_prefix_error (error, "--tar-pathname-filter: ");
                      goto out;
                    }
                  opts.translate_pathname_user_data = &tpdata;
                  g_autoptr(OtAutoArchiveRead) archive = ot_open_archive_read (tree, error);
                  if (!archive)
                    goto out;
                  if (!ostree_repo_import_archive_to_mtree (repo, &opts, archive, mtree,
                                                            modifier, cancellable, error))
                    goto out;
#else
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                               "This version of ostree is not compiled with libarchive support");
                  goto out;
#endif
                }
            }
          else if (strcmp (tree_type, "ref") == 0)
            {
              if (!ostree_repo_read_commit (repo, tree, &object_to_commit, NULL, cancellable, error))
                goto out;

              if (!ostree_repo_write_directory_to_mtree (repo, object_to_commit, mtree, modifier,
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
      if (!ostree_repo_write_dfd_to_mtree (repo, AT_FDCWD, argv[1], mtree, modifier,
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

  if (skip_list && g_hash_table_size (skip_list) > 0)
    {
      GHashTableIter hash_iter;
      gpointer key;

      g_hash_table_iter_init (&hash_iter, skip_list);

      while (g_hash_table_iter_next (&hash_iter, &key, NULL))
        {
          g_printerr ("Unmatched skip-list path: %s\n", (char*)key);
        }
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unmatched skip-list paths");
      goto out;
    }

  if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
    goto out;

  if (opt_skip_if_unchanged && parent)
    {
      g_autoptr(GFile) parent_root;

      if (!ostree_repo_read_commit (repo, parent, &parent_root, NULL, cancellable, error))
        goto out;

      if (g_file_equal (root, parent_root))
        skip_commit = TRUE;
    }

  if (!skip_commit)
    {
      gboolean update_summary;
      guint64 timestamp;
      g_autoptr(GVariant) old_metadata = g_steal_pointer (&metadata);

      fill_bindings (repo, old_metadata, &metadata);

      if (!opt_timestamp)
        {
          GDateTime *now = g_date_time_new_now_utc ();
          timestamp = g_date_time_to_unix (now);
          g_date_time_unref (now);

          if (!ostree_repo_write_commit (repo, parent, opt_subject, commit_body, metadata,
                                         OSTREE_REPO_FILE (root),
                                         &commit_checksum, cancellable, error))
            goto out;
        }
      else
        {
          struct timespec ts;
          if (!parse_datetime (&ts, opt_timestamp, NULL))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Could not parse '%s'", opt_timestamp);
              goto out;
            }
          timestamp = ts.tv_sec;

          if (!ostree_repo_write_commit_with_time (repo, parent, opt_subject, commit_body, metadata,
                                                   OSTREE_REPO_FILE (root),
                                                   timestamp,
                                                   &commit_checksum, cancellable, error))
            goto out;
        }

      if (detached_metadata)
        {
          if (!ostree_repo_write_commit_detached_metadata (repo, commit_checksum,
                                                           detached_metadata,
                                                           cancellable, error))
            goto out;
        }

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

      if (opt_branch)
        ostree_repo_transaction_set_ref (repo, NULL, opt_branch, commit_checksum);
      else
        g_assert (opt_orphan);

      if (!ostree_repo_commit_transaction (repo, &stats, cancellable, error))
        goto out;

      /* The default for this option is FALSE, even for archive repos,
       * because ostree supports multiple processes committing to the same
       * repo (but different refs) concurrently, and in fact gnome-continuous
       * actually does this.  In that context it's best to update the summary
       * explicitly instead of automatically here. */
      if (!ot_keyfile_get_boolean_with_default (ostree_repo_get_config (repo), "core",
                                                "commit-update-summary", FALSE,
                                                &update_summary, error))
        goto out;

      if (update_summary && !ostree_repo_regenerate_summary (repo,
                                                             NULL,
                                                             cancellable,
                                                             error))
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
  if (repo)
    ostree_repo_abort_transaction (repo, cancellable, NULL);
  if (modifier)
    ostree_repo_commit_modifier_unref (modifier);
  return ret;
}
