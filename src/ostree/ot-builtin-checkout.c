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

#include <string.h>
#include <gio/gunixinputstream.h>

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"
#include "ot-tool-util.h"

static gboolean opt_user_mode;
static gboolean opt_allow_noent;
static gboolean opt_disable_cache;
static char *opt_subpath;
static gboolean opt_union;
static gboolean opt_whiteouts;
static gboolean opt_from_stdin;
static char *opt_from_file;
static gboolean opt_disable_fsync;
static gboolean opt_require_hardlinks;

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

static GOptionEntry options[] = {
  { "user-mode", 'U', 0, G_OPTION_ARG_NONE, &opt_user_mode, "Do not change file ownership or initialize extended attributes", NULL },
  { "disable-cache", 0, 0, G_OPTION_ARG_NONE, &opt_disable_cache, "Do not update or use the internal repository uncompressed object cache", NULL },
  { "subpath", 0, 0, G_OPTION_ARG_STRING, &opt_subpath, "Checkout sub-directory PATH", "PATH" },
  { "union", 0, 0, G_OPTION_ARG_NONE, &opt_union, "Keep existing directories, overwrite existing files", NULL },
  { "whiteouts", 0, 0, G_OPTION_ARG_NONE, &opt_whiteouts, "Process 'whiteout' (Docker style) entries", NULL },
  { "allow-noent", 0, 0, G_OPTION_ARG_NONE, &opt_allow_noent, "Do nothing if specified path does not exist", NULL },
  { "from-stdin", 0, 0, G_OPTION_ARG_NONE, &opt_from_stdin, "Process many checkouts from standard input", NULL },
  { "from-file", 0, 0, G_OPTION_ARG_STRING, &opt_from_file, "Process many checkouts from input file", "FILE" },
  { "fsync", 0, 0, G_OPTION_ARG_CALLBACK, parse_fsync_cb, "Specify how to invoke fsync()", "POLICY" },
  { "require-hardlinks", 'H', 0, G_OPTION_ARG_NONE, &opt_require_hardlinks, "Do not fall back to full copies if hardlinking fails", NULL },
  { NULL }
};

static gboolean
process_one_checkout (OstreeRepo           *repo,
                      const char           *resolved_commit,
                      const char           *subpath,
                      const char           *destination,
                      GCancellable         *cancellable,
                      GError              **error)
{
  gboolean ret = FALSE;

  /* This strange code structure is to preserve testing
   * coverage of both `ostree_repo_checkout_tree` and
   * `ostree_repo_checkout_at` until such time as we have a more
   * convenient infrastructure for testing C APIs with data.
   */
  if (opt_disable_cache || opt_whiteouts || opt_require_hardlinks)
    {
      OstreeRepoCheckoutAtOptions options = { 0, };
      
      if (opt_user_mode)
        options.mode = OSTREE_REPO_CHECKOUT_MODE_USER;
      if (opt_union)
        options.overwrite_mode = OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES;
      if (opt_whiteouts)
        options.process_whiteouts = TRUE;
      if (subpath)
        options.subpath = subpath;
      options.no_copy_fallback = opt_require_hardlinks;

      if (!ostree_repo_checkout_at (repo, &options,
                                    AT_FDCWD, destination,
                                    resolved_commit,
                                    cancellable, error))
        goto out;
    }
  else
    {
      GError *tmp_error = NULL;
      g_autoptr(GFile) root = NULL;
      g_autoptr(GFile) subtree = NULL;
      g_autoptr(GFileInfo) file_info = NULL;
      g_autoptr(GFile) destination_file = g_file_new_for_path (destination);

      if (!ostree_repo_read_commit (repo, resolved_commit, &root, NULL, cancellable, error))
        goto out;

      if (subpath)
        subtree = g_file_resolve_relative_path (root, subpath);
      else
        subtree = g_object_ref (root);

      file_info = g_file_query_info (subtree, OSTREE_GIO_FAST_QUERYINFO,
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                     cancellable, &tmp_error);
      if (!file_info)
        {
          if (opt_allow_noent
              && g_error_matches (tmp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&tmp_error);
              ret = TRUE;
            }
          else
            {
              g_propagate_error (error, tmp_error);
            }
          goto out;
        }

      if (!ostree_repo_checkout_tree (repo, opt_user_mode ? OSTREE_REPO_CHECKOUT_MODE_USER : 0,
                                      opt_union ? OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES : 0,
                                      destination_file,
                                      OSTREE_REPO_FILE (subtree), file_info,
                                      cancellable, error))
        goto out;
    }
                      
  ret = TRUE;
 out:
  return ret;
}

static gboolean
process_many_checkouts (OstreeRepo         *repo,
                        const char         *target,
                        GCancellable       *cancellable,
                        GError            **error)
{
  gboolean ret = FALSE;
  gsize len;
  GError *temp_error = NULL;
  g_autoptr(GInputStream) instream = NULL;
  g_autoptr(GDataInputStream) datastream = NULL;
  g_autofree char *revision = NULL;
  g_autofree char *subpath = NULL;
  g_autofree char *resolved_commit = NULL;

  if (opt_from_stdin)
    {
      instream = (GInputStream*)g_unix_input_stream_new (0, FALSE);
    }
  else
    {
      g_autoptr(GFile) f = g_file_new_for_path (opt_from_file);

      instream = (GInputStream*)g_file_read (f, cancellable, error);
      if (!instream)
        goto out;
    }
    
  datastream = g_data_input_stream_new (instream);

  while ((revision = g_data_input_stream_read_upto (datastream, "", 1, &len,
                                                    cancellable, &temp_error)) != NULL)
    {
      if (revision[0] == '\0')
        break;

      /* Read the null byte */
      (void) g_data_input_stream_read_byte (datastream, cancellable, NULL);
      g_free (subpath);
      subpath = g_data_input_stream_read_upto (datastream, "", 1, &len,
                                               cancellable, &temp_error);
      if (temp_error)
        {
          g_propagate_error (error, temp_error);
          goto out;
        }

      /* Read the null byte */
      (void) g_data_input_stream_read_byte (datastream, cancellable, NULL);

      if (!ostree_repo_resolve_rev (repo, revision, FALSE, &resolved_commit, error))
        goto out;

      if (!process_one_checkout (repo, resolved_commit, subpath, target,
                                 cancellable, error))
        {
          g_prefix_error (error, "Processing tree %s: ", resolved_commit);
          goto out;
        }

      g_free (revision);
    }
  if (temp_error)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_builtin_checkout (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  glnx_unref_object OstreeRepo *repo = NULL;
  gboolean ret = FALSE;
  const char *commit;
  const char *destination;
  g_autofree char *resolved_commit = NULL;

  context = g_option_context_new ("COMMIT [DESTINATION] - Check out a commit into a filesystem tree");

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (opt_disable_fsync)
    ostree_repo_set_disable_fsync (repo, TRUE);

  if (argc < 2)
    {
      gchar *help = g_option_context_get_help (context, TRUE, NULL);
      g_printerr ("%s\n", help);
      g_free (help);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "COMMIT must be specified");
      goto out;
    }

  if (opt_from_stdin || opt_from_file)
    {
      destination = argv[1];

      if (!process_many_checkouts (repo, destination, cancellable, error))
        goto out;
    }
  else
    {
      commit = argv[1];
      if (argc < 3)
        destination = commit;
      else
        destination = argv[2];

      if (!ostree_repo_resolve_rev (repo, commit, FALSE, &resolved_commit, error))
        goto out;

      if (!process_one_checkout (repo, resolved_commit, opt_subpath,
                                 destination,
                                 cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
