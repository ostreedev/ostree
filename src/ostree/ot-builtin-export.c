/*
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include "otutil.h"
#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree-libarchive-private.h"
#include "ostree.h"
#include "ostree-repo-file.h"

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

static GOptionEntry options[] = {
  { "no-xattrs", 0, 0, G_OPTION_ARG_NONE, &opt_no_xattrs, "Skip output of extended attributes", NULL },
  { "subpath", 0, 0, G_OPTION_ARG_FILENAME, &opt_subpath, "Checkout sub-directory PATH", "PATH" },
  { "prefix", 0, 0, G_OPTION_ARG_FILENAME, &opt_prefix, "Add PATH as prefix to archive pathnames", "PATH" },
  { "output", 'o', 0, G_OPTION_ARG_FILENAME, &opt_output_path, "Output to PATH ", "PATH" },
  { NULL }
};

#ifdef HAVE_LIBARCHIVE

static void
propagate_libarchive_error (GError      **error,
                            struct archive *a)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s", archive_error_string (a));
}

#endif

gboolean
ostree_builtin_export (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  gboolean ret = FALSE;
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) subtree = NULL;
  g_autofree char *commit = NULL;
  g_autoptr(GVariant) commit_data = NULL;
#ifdef HAVE_LIBARCHIVE
  const char *rev;
  g_autoptr(OtAutoArchiveWrite) a = NULL;
  OstreeRepoExportArchiveOptions opts = { 0, };
#endif

  context = g_option_context_new ("COMMIT");

  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    goto out;

#ifdef HAVE_LIBARCHIVE  

  if (argc <= 1)
    {
      ot_util_usage_error (context, "A COMMIT argument is required", error);
      goto out;
    }
  rev = argv[1];

  a = archive_write_new ();
  /* Yes, this is hardcoded for now.  There is
   * archive_write_set_format_filter_by_ext() but it's fairly magic.
   * Many programs have support now for GNU tar, so should be a good
   * default.  I also don't want to lock us into everything libarchive
   * supports.
   */
  if (archive_write_set_format_gnutar (a) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }
  if (archive_write_add_filter_none (a) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }
  if (opt_output_path)
    {
      if (archive_write_open_filename (a, opt_output_path) != ARCHIVE_OK)
        {
          propagate_libarchive_error (error, a);
          goto out;
        }
    }
  else
    {
      if (archive_write_open_FILE (a, stdout) != ARCHIVE_OK)
        {
          propagate_libarchive_error (error, a);
          goto out;
        }
    }

  if (opt_no_xattrs)
    opts.disable_xattrs = TRUE;

  if (!ostree_repo_read_commit (repo, rev, &root, &commit, cancellable, error))
    goto out;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit, &commit_data, error))
    goto out;

  opts.timestamp_secs = ostree_commit_get_timestamp (commit_data);

  if (opt_subpath)
    subtree = g_file_resolve_relative_path (root, opt_subpath);
  else
    subtree = g_object_ref (root);

  opts.path_prefix = opt_prefix;

  if (!ostree_repo_export_tree_to_archive (repo, &opts, (OstreeRepoFile*)subtree, a,
                                           cancellable, error))
    goto out;

  if (archive_write_close (a) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }

#else
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This version of ostree is not compiled with libarchive support");
  goto out;
#endif  
  
  ret = TRUE;
 out:
  return ret;
}
