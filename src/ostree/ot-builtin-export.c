/*
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include "ostree-libarchive-private.h"
#include "ostree-repo-file.h"
#include "ostree.h"
#include "ot-builtins.h"
#include "ot-main.h"
#include "otutil.h"

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

static char *opt_output_path;
static char *opt_subpath;
static char *opt_prefix;
static gboolean opt_no_xattrs;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-export.xml) when changing the option list.
 */

static GOptionEntry options[]
    = { { "no-xattrs", 0, 0, G_OPTION_ARG_NONE, &opt_no_xattrs,
          "Skip output of extended attributes", NULL },
        { "subpath", 0, 0, G_OPTION_ARG_FILENAME, &opt_subpath, "Checkout sub-directory PATH",
          "PATH" },
        { "prefix", 0, 0, G_OPTION_ARG_FILENAME, &opt_prefix,
          "Add PATH as prefix to archive pathnames", "PATH" },
        { "output", 'o', 0, G_OPTION_ARG_FILENAME, &opt_output_path, "Output to PATH ", "PATH" },
        { NULL } };

gboolean
ostree_builtin_export (int argc, char **argv, OstreeCommandInvocation *invocation,
                       GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("COMMIT");

  g_autoptr (OstreeRepo) repo = NULL;
  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable,
                                    error))
    return FALSE;

#ifdef HAVE_LIBARCHIVE

  if (argc <= 1)
    {
      ot_util_usage_error (context, "A COMMIT argument is required", error);
      return FALSE;
    }
  const char *rev = argv[1];

  g_autoptr (OtAutoArchiveWrite) a = archive_write_new ();
  /* Yes, this is hardcoded for now.  There is
   * archive_write_set_format_filter_by_ext() but it's fairly magic.
   * Many programs have support now for GNU tar, so should be a good
   * default.  I also don't want to lock us into everything libarchive
   * supports.
   */
  if (archive_write_set_format_gnutar (a) != ARCHIVE_OK)
    return glnx_throw (error, "%s", archive_error_string (a));
  if (archive_write_add_filter_none (a) != ARCHIVE_OK)
    return glnx_throw (error, "%s", archive_error_string (a));
  if (opt_output_path)
    {
      if (archive_write_open_filename (a, opt_output_path) != ARCHIVE_OK)
        return glnx_throw (error, "%s", archive_error_string (a));
    }
  else
    {
      if (archive_write_open_FILE (a, stdout) != ARCHIVE_OK)
        return glnx_throw (error, "%s", archive_error_string (a));
    }

  OstreeRepoExportArchiveOptions opts = {
    0,
  };
  if (opt_no_xattrs)
    opts.disable_xattrs = TRUE;

  g_autofree char *commit = NULL;
  g_autoptr (GFile) root = NULL;
  if (!ostree_repo_read_commit (repo, rev, &root, &commit, cancellable, error))
    return FALSE;

  g_autoptr (GVariant) commit_data = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit, &commit_data, error))
    return FALSE;

  opts.timestamp_secs = ostree_commit_get_timestamp (commit_data);

  g_autoptr (GFile) subtree = NULL;
  if (opt_subpath)
    subtree = g_file_resolve_relative_path (root, opt_subpath);
  else
    subtree = g_object_ref (root);

  opts.path_prefix = opt_prefix;

  if (!ostree_repo_export_tree_to_archive (repo, &opts, (OstreeRepoFile *)subtree, a, cancellable,
                                           error))
    return FALSE;

  if (archive_write_close (a) != ARCHIVE_OK)
    return glnx_throw (error, "%s", archive_error_string (a));

  return TRUE;
#else
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This version of ostree is not compiled with libarchive support");
  return FALSE;
#endif
}
