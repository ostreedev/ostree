/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-builtins.h"
#include "ostree.h"

#include <glib/gi18n.h>

static char *repo_path;
static gboolean separator_null;
static int from_fd = -1;
static gboolean from_stdin;
static char *from_file;
static char *subject;
static char *body;
static char **additions;
static char **removals;

static GOptionEntry options[] = {
  { "repo", 0, 0, G_OPTION_ARG_FILENAME, &repo_path, "Repository path", "repo" },
  { "subject", 's', 0, G_OPTION_ARG_STRING, &subject, "One line subject", "subject" },
  { "body", 'b', 0, G_OPTION_ARG_STRING, &body, "Full description", "body" },
  { "from-fd", 0, 0, G_OPTION_ARG_INT, &from_fd, "Read new tree files from fd", "file descriptor" },
  { "from-stdin", 0, 0, G_OPTION_ARG_NONE, &from_stdin, "Read new tree files from stdin", "file descriptor" },
  { "from-file", 0, 0, G_OPTION_ARG_FILENAME, &from_file, "Read new tree files from another file", "path" },
  { "separator-null", 0, 0, G_OPTION_ARG_NONE, &separator_null, "", "Use '\\0' as filename separator, as with find -print0" },
  { "add", 'a', 0, G_OPTION_ARG_FILENAME_ARRAY, &additions, "Relative file path to add", "filename" },
  { "remove", 'r', 0, G_OPTION_ARG_FILENAME_ARRAY, &removals, "Relative file path to remove", "filename" },
  { NULL }
};

gboolean
ostree_builtin_commit (int argc, char **argv, const char *prefix, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  gboolean using_filename_cmdline;
  gboolean using_filedescriptors;
  GPtrArray *additions_array = NULL;
  GPtrArray *removals_array = NULL;
  GChecksum *commit_checksum = NULL;
  char **iter;

  context = g_option_context_new ("- Commit a new revision");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (repo_path == NULL)
    repo_path = ".";
  if (prefix == NULL)
    prefix = ".";

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  using_filename_cmdline = (removals || additions);
  using_filedescriptors = (from_file || from_fd >= 0 || from_stdin);

  if (!(using_filename_cmdline || using_filedescriptors))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "No additions or removals specified");
      goto out;
    }
  if (using_filename_cmdline && using_filedescriptors)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "File descriptors may not be combined with --add or --remove");
      goto out;
    }

  if (!subject)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "A subject must be specified with --subject");
      goto out;
    }

  if (using_filename_cmdline)
    {
      g_assert (removals || additions);
      additions_array = g_ptr_array_new ();
      removals_array = g_ptr_array_new ();

      if (additions)
        for (iter = additions; *iter; iter++)
          g_ptr_array_add (additions_array, *iter);
      if (removals)
        for (iter = removals; *iter; iter++)
          g_ptr_array_add (removals_array, *iter);
      
      if (!ostree_repo_commit (repo, subject, body, NULL,
                                 prefix, additions_array,
                                 removals_array,
                                 &commit_checksum,
                                 error))
        goto out;
    }
  else if (using_filedescriptors)
    {
      char separator = separator_null ? '\0' : '\n';
      gboolean temp_fd = -1;

      if (from_stdin)
        from_fd = 0;
      else if (from_file)
        {
          temp_fd = ot_util_open_file_read (from_file, error);
          if (temp_fd < 0)
            {
              g_prefix_error (error, "Failed to open '%s': ", from_file);
              goto out;
            }
          from_fd = temp_fd;
        }
      if (!ostree_repo_commit_from_filelist_fd (repo, subject, body, NULL,
                                                  prefix, from_fd, separator,
                                                  &commit_checksum, error))
        {
          if (temp_fd >= 0)
            close (temp_fd);
          goto out;
        }
      if (temp_fd >= 0)
        close (temp_fd);
    }
 
  ret = TRUE;
  g_print ("%s\n", g_checksum_get_string (commit_checksum));
 out:
  if (context)
    g_option_context_free (context);
  g_clear_object (&repo);
  if (removals_array)
    g_ptr_array_free (removals_array, TRUE);
  if (additions_array)
    g_ptr_array_free (additions_array, TRUE);
  if (commit_checksum)
    g_checksum_free (commit_checksum);
  return ret;
}
