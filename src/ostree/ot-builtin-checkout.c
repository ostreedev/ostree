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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <gio/gunixinputstream.h>
#include <string.h>

#include "ostree.h"
#include "ot-builtins.h"
#include "ot-main.h"
#include "otutil.h"

static gboolean opt_user_mode;
static gboolean opt_allow_noent;
static gboolean opt_disable_cache;
static char *opt_subpath;
static gboolean opt_union;
static gboolean opt_union_add;
static gboolean opt_union_identical;
static gboolean opt_whiteouts;
static gboolean opt_process_passthrough_whiteouts;
static gboolean opt_from_stdin;
static char *opt_from_file;
static gboolean opt_disable_fsync;
static gboolean opt_require_hardlinks;
static gboolean opt_force_copy;
static gboolean opt_force_copy_zerosized;
static gboolean opt_bareuseronly_dirs;
static char *opt_skiplist_file;
static char *opt_selinux_policy;
static char *opt_selinux_prefix;

static gboolean
parse_fsync_cb (const char *option_name, const char *value, gpointer data, GError **error)
{
  gboolean val;

  if (!ot_parse_boolean (value, &val, error))
    return FALSE;

  opt_disable_fsync = !val;

  return TRUE;
}

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-checkout.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "user-mode", 'U', 0, G_OPTION_ARG_NONE, &opt_user_mode,
    "Do not change file ownership or initialize extended attributes", NULL },
  { "disable-cache", 0, 0, G_OPTION_ARG_NONE, &opt_disable_cache,
    "Do not update or use the internal repository uncompressed object cache", NULL },
  { "subpath", 0, 0, G_OPTION_ARG_FILENAME, &opt_subpath, "Checkout sub-directory PATH", "PATH" },
  { "union", 0, 0, G_OPTION_ARG_NONE, &opt_union,
    "Keep existing directories, overwrite existing files", NULL },
  { "union-add", 0, 0, G_OPTION_ARG_NONE, &opt_union_add,
    "Keep existing files/directories, only add new", NULL },
  { "union-identical", 0, 0, G_OPTION_ARG_NONE, &opt_union_identical,
    "When layering checkouts, error out if a file would be replaced with a different version, but "
    "add new files and directories",
    NULL },
  { "whiteouts", 0, 0, G_OPTION_ARG_NONE, &opt_whiteouts,
    "Process 'whiteout' (Docker style) entries", NULL },
  { "process-passthrough-whiteouts", 0, 0, G_OPTION_ARG_NONE, &opt_process_passthrough_whiteouts,
    "Enable overlayfs whiteout extraction into char 0:0 devices", NULL },
  { "allow-noent", 0, 0, G_OPTION_ARG_NONE, &opt_allow_noent,
    "Do nothing if specified path does not exist", NULL },
  { "from-stdin", 0, 0, G_OPTION_ARG_NONE, &opt_from_stdin,
    "Process many checkouts from standard input", NULL },
  { "from-file", 0, 0, G_OPTION_ARG_STRING, &opt_from_file,
    "Process many checkouts from input file", "FILE" },
  { "fsync", 0, 0, G_OPTION_ARG_CALLBACK, parse_fsync_cb, "Specify how to invoke fsync()",
    "POLICY" },
  { "require-hardlinks", 'H', 0, G_OPTION_ARG_NONE, &opt_require_hardlinks,
    "Do not fall back to full copies if hardlinking fails", NULL },
  { "force-copy-zerosized", 'z', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_force_copy_zerosized,
    "Do not hardlink zero-sized files", NULL },
  { "force-copy", 'C', 0, G_OPTION_ARG_NONE, &opt_force_copy,
    "Never hardlink (but may reflink if available)", NULL },
  { "bareuseronly-dirs", 'M', 0, G_OPTION_ARG_NONE, &opt_bareuseronly_dirs,
    "Suppress mode bits outside of 0775 for directories (suid, world writable, etc.)", NULL },
  { "skip-list", 0, 0, G_OPTION_ARG_FILENAME, &opt_skiplist_file,
    "File containing list of files to skip", "FILE" },
  { "selinux-policy", 0, 0, G_OPTION_ARG_FILENAME, &opt_selinux_policy,
    "Set SELinux labels based on policy in root filesystem PATH (may be /); implies --force-copy",
    "PATH" },
  { "selinux-prefix", 0, 0, G_OPTION_ARG_STRING, &opt_selinux_prefix,
    "When setting SELinux labels, prefix all paths by PREFIX", "PREFIX" },
  { NULL }
};

static gboolean
handle_skiplist_line (const char *line, void *data, GError **error)
{
  GHashTable *files = data;
  g_hash_table_add (files, g_strdup (line));
  return TRUE;
}

static OstreeRepoCheckoutFilterResult
checkout_filter (OstreeRepo *self, const char *path, struct stat *st_buf, gpointer user_data)
{
  GHashTable *skiplist = user_data;
  if (g_hash_table_contains (skiplist, path))
    return OSTREE_REPO_CHECKOUT_FILTER_SKIP;
  return OSTREE_REPO_CHECKOUT_FILTER_ALLOW;
}

static gboolean
process_one_checkout (OstreeRepo *repo, const char *resolved_commit, const char *subpath,
                      const char *destination, GCancellable *cancellable, GError **error)
{
  /* This strange code structure is to preserve testing
   * coverage of both `ostree_repo_checkout_tree` and
   * `ostree_repo_checkout_at` until such time as we have a more
   * convenient infrastructure for testing C APIs with data.
   */
  if (opt_disable_cache || opt_whiteouts || opt_require_hardlinks || opt_union_add || opt_force_copy
      || opt_force_copy_zerosized || opt_bareuseronly_dirs || opt_union_identical
      || opt_skiplist_file || opt_selinux_policy || opt_selinux_prefix
      || opt_process_passthrough_whiteouts)
    {
      OstreeRepoCheckoutAtOptions checkout_options = {
        0,
      };

      /* do this early so option checking also catches force copy conflicts */
      if (opt_selinux_policy)
        opt_force_copy = TRUE;

      if (opt_user_mode)
        checkout_options.mode = OSTREE_REPO_CHECKOUT_MODE_USER;
      /* Can't union these */
      if (opt_union && opt_union_add)
        return glnx_throw (error, "Cannot specify both --union and --union-add");
      if (opt_union && opt_union_identical)
        return glnx_throw (error, "Cannot specify both --union and --union-identical");
      if (opt_union_add && opt_union_identical)
        return glnx_throw (error, "Cannot specify both --union-add and --union-identical");
      if (opt_require_hardlinks && opt_force_copy)
        return glnx_throw (error, "Cannot specify both --require-hardlinks and --force-copy");
      if (opt_selinux_prefix && !opt_selinux_policy)
        return glnx_throw (error, "Cannot specify --selinux-prefix without --selinux-policy");
      else if (opt_union)
        checkout_options.overwrite_mode = OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES;
      else if (opt_union_add)
        checkout_options.overwrite_mode = OSTREE_REPO_CHECKOUT_OVERWRITE_ADD_FILES;
      else if (opt_union_identical)
        {
          if (!opt_require_hardlinks)
            return glnx_throw (error, "--union-identical requires --require-hardlinks");
          checkout_options.overwrite_mode = OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL;
        }
      if (opt_whiteouts)
        checkout_options.process_whiteouts = TRUE;
      if (opt_process_passthrough_whiteouts)
        checkout_options.process_passthrough_whiteouts = TRUE;
      if (subpath)
        checkout_options.subpath = subpath;

      g_autoptr (OstreeSePolicy) policy = NULL;
      if (opt_selinux_policy)
        {
          glnx_autofd int rootfs_dfd = -1;
          if (!glnx_opendirat (AT_FDCWD, opt_selinux_policy, TRUE, &rootfs_dfd, error))
            return glnx_prefix_error (error, "selinux-policy: ");
          policy = ostree_sepolicy_new_at (rootfs_dfd, cancellable, error);
          if (!policy)
            return FALSE;
          checkout_options.sepolicy = policy;
          checkout_options.sepolicy_prefix = opt_selinux_prefix;
        }

      g_autoptr (GHashTable) skip_list
          = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      if (opt_skiplist_file)
        {
          if (!ot_parse_file_by_line (opt_skiplist_file, handle_skiplist_line, skip_list,
                                      cancellable, error))
            return FALSE;
          checkout_options.filter = checkout_filter;
          checkout_options.filter_user_data = skip_list;
        }

      checkout_options.no_copy_fallback = opt_require_hardlinks;
      checkout_options.force_copy = opt_force_copy;
      checkout_options.force_copy_zerosized = opt_force_copy_zerosized;
      checkout_options.bareuseronly_dirs = opt_bareuseronly_dirs;

      if (!ostree_repo_checkout_at (repo, &checkout_options, AT_FDCWD, destination, resolved_commit,
                                    cancellable, error))
        return FALSE;
    }
  else
    {
      GError *tmp_error = NULL;
      g_autoptr (GFile) root = NULL;
      g_autoptr (GFile) destination_file = g_file_new_for_path (destination);

      if (!ostree_repo_read_commit (repo, resolved_commit, &root, NULL, cancellable, error))
        return FALSE;

      g_autoptr (GFile) subtree = NULL;
      if (subpath)
        subtree = g_file_resolve_relative_path (root, subpath);
      else
        subtree = g_object_ref (root);

      g_autoptr (GFileInfo) file_info
          = g_file_query_info (subtree, OSTREE_GIO_FAST_QUERYINFO,
                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &tmp_error);
      if (!file_info)
        {
          if (opt_allow_noent && g_error_matches (tmp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&tmp_error);
              /* Note early return */
              return TRUE;
            }
          else
            {
              g_propagate_error (error, tmp_error);
              return FALSE;
            }
        }

      if (!ostree_repo_checkout_tree (repo, opt_user_mode ? OSTREE_REPO_CHECKOUT_MODE_USER : 0,
                                      opt_union ? OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES : 0,
                                      destination_file, OSTREE_REPO_FILE (subtree), file_info,
                                      cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
process_many_checkouts (OstreeRepo *repo, const char *target, GCancellable *cancellable,
                        GError **error)
{
  GError *temp_error = NULL;

  g_autoptr (GInputStream) instream = NULL;
  if (opt_from_stdin)
    {
      instream = (GInputStream *)g_unix_input_stream_new (0, FALSE);
    }
  else
    {
      g_autoptr (GFile) f = g_file_new_for_path (opt_from_file);

      instream = (GInputStream *)g_file_read (f, cancellable, error);
      if (!instream)
        return FALSE;
    }

  g_autoptr (GDataInputStream) datastream = g_data_input_stream_new (instream);

  g_autofree char *resolved_commit = NULL;
  g_autofree char *revision = NULL;
  gsize len;
  while (
      (revision = g_data_input_stream_read_upto (datastream, "", 1, &len, cancellable, &temp_error))
      != NULL)
    {
      if (revision[0] == '\0')
        break;

      /* Read the null byte */
      (void)g_data_input_stream_read_byte (datastream, cancellable, NULL);
      g_autofree char *subpath
          = g_data_input_stream_read_upto (datastream, "", 1, &len, cancellable, &temp_error);
      if (temp_error)
        {
          g_propagate_error (error, temp_error);
          return FALSE;
        }

      /* Read the null byte */
      (void)g_data_input_stream_read_byte (datastream, cancellable, NULL);

      if (!ostree_repo_resolve_rev (repo, revision, FALSE, &resolved_commit, error))
        return FALSE;

      if (!process_one_checkout (repo, resolved_commit, subpath, target, cancellable, error))
        {
          g_prefix_error (error, "Processing tree %s: ", resolved_commit);
          return FALSE;
        }
    }
  if (temp_error)
    {
      g_propagate_error (error, temp_error);
      return FALSE;
    }

  return TRUE;
}

gboolean
ostree_builtin_checkout (int argc, char **argv, OstreeCommandInvocation *invocation,
                         GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("COMMIT [DESTINATION]");
  g_autoptr (OstreeRepo) repo = NULL;
  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable,
                                    error))
    return FALSE;

  if (opt_disable_fsync)
    ostree_repo_set_disable_fsync (repo, TRUE);

  if (argc < 2)
    {
      g_autofree char *help = g_option_context_get_help (context, TRUE, NULL);
      g_printerr ("%s\n", help);
      return glnx_throw (error, "COMMIT must be specified");
    }

  if (opt_from_stdin || opt_from_file)
    {
      const char *destination = argv[1];

      if (!process_many_checkouts (repo, destination, cancellable, error))
        return FALSE;
    }
  else
    {
      const char *commit = argv[1];
      const char *destination;
      if (argc < 3)
        destination = commit;
      else
        destination = argv[2];

      g_autofree char *resolved_commit = NULL;
      if (!ostree_repo_resolve_rev (repo, commit, FALSE, &resolved_commit, error))
        return FALSE;

      if (!process_one_checkout (repo, resolved_commit, opt_subpath, destination, cancellable,
                                 error))
        return FALSE;
    }

  return TRUE;
}
