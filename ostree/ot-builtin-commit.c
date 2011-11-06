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

#include <gio/gunixoutputstream.h>

#include <glib/gi18n.h>

static gboolean separator_null;
static int from_fd = -1;
static gboolean from_stdin;
static char *from_file;
static char *metadata_text_path;
static char *metadata_bin_path;
static char *subject;
static char *body;
static char *parent;
static char *branch;
static char **additions;
static char **removals;

static GOptionEntry options[] = {
  { "subject", 's', 0, G_OPTION_ARG_STRING, &subject, "One line subject", "subject" },
  { "body", 'm', 0, G_OPTION_ARG_STRING, &body, "Full description", "body" },
  { "metadata-variant-text", 0, 0, G_OPTION_ARG_FILENAME, &metadata_text_path, "File containing g_variant_print() output", "path" },
  { "metadata-variant", 0, 0, G_OPTION_ARG_FILENAME, &metadata_bin_path, "File containing serialized variant, in host endianness", "path" },
  { "branch", 'b', 0, G_OPTION_ARG_STRING, &branch, "Branch", "branch" },
  { "parent", 'p', 0, G_OPTION_ARG_STRING, &parent, "Parent commit", "commit" },
  { "from-fd", 0, 0, G_OPTION_ARG_INT, &from_fd, "Read new tree files from fd", "file descriptor" },
  { "from-stdin", 0, 0, G_OPTION_ARG_NONE, &from_stdin, "Read new tree files from stdin", "file descriptor" },
  { "from-file", 0, 0, G_OPTION_ARG_FILENAME, &from_file, "Read new tree files from another file", "path" },
  { "separator-null", 0, 0, G_OPTION_ARG_NONE, &separator_null, "", "Use '\\0' as filename separator, as with find -print0" },
  { "add", 'a', 0, G_OPTION_ARG_FILENAME_ARRAY, &additions, "Relative file path to add", "filename" },
  { "remove", 'r', 0, G_OPTION_ARG_FILENAME_ARRAY, &removals, "Relative file path to remove", "filename" },
  { NULL }
};

typedef struct {
  GFile *dir;
  char separator;
  GOutputStream *out;
  GCancellable *cancellable;
} FindThreadData;

static gboolean
find (const char *basepath,
      GFile *dir,
      char separator,
      GOutputStream *out,
      GCancellable *cancellable,
      GError  **error);

static gboolean
find_write_child (const char *basepath,
                  GFile      *dir,
                  char        separator,
                  GOutputStream *out,
                  GFileInfo  *finfo,
                  GCancellable *cancellable,
                  GError    **error)
{
  gboolean ret = FALSE;
  guint32 type;
  const char *name;
  char buf[1];
  char *child_path = NULL;
  GString *child_trimmed_path = NULL;
  GFile *child = NULL;
  gsize bytes_written;

  type = g_file_info_get_attribute_uint32 (finfo, "standard::type");
  name = g_file_info_get_attribute_byte_string (finfo, "standard::name");

  child = g_file_get_child (dir, name);

  if (type == G_FILE_TYPE_DIRECTORY)
    {
      if (!find (basepath, child, separator, out, cancellable, error))
        goto out;
    }

  child_path = g_file_get_path (child);
  child_trimmed_path = g_string_new (child_path + strlen (basepath));
  if (!*(child_trimmed_path->str))
    {
      /* do nothing - we implicitly add the root . */
    }
  else
    {
      g_assert (*(child_trimmed_path->str) == '/');
      g_string_insert_c (child_trimmed_path, 0, '.');

      if (!g_output_stream_write_all (out, child_trimmed_path->str, child_trimmed_path->len,
                                      &bytes_written, cancellable, error))
        goto out;
      buf[0] = separator;
      if (!g_output_stream_write_all (out, buf, 1, &bytes_written,
                                      cancellable, error))
        goto out;
    }
      
  ret = TRUE;
 out:
  g_string_free (child_trimmed_path, TRUE);
  child_trimmed_path = NULL;
  g_free (child_path);
  child_path = NULL;
  g_clear_object (&child);
  return ret;
}

static gboolean
find (const char *basepath,
      GFile *dir,
      char separator,
      GOutputStream *out,
      GCancellable *cancellable,
      GError  **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  GFileEnumerator *enumerator = NULL;
  GFileInfo *finfo = NULL;

  enumerator = g_file_enumerate_children (dir, "standard::type,standard::name", 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, error);
  if (!enumerator)
    goto out;

  while ((finfo = g_file_enumerator_next_file (enumerator, cancellable, error)) != NULL)
    {
      if (!find_write_child (basepath, dir, separator, out, finfo, cancellable, error))
        goto out;
      g_clear_object (&finfo);
    }
  if (temp_error)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  g_clear_object (&finfo);
  g_clear_object (&enumerator);
  return ret;
}

static gpointer
find_thread (gpointer data)
{
  FindThreadData *tdata = data;
  GError *error = NULL;
  char *path;
  
  path = g_file_get_path (tdata->dir);
  if (!find (path, tdata->dir, tdata->separator, tdata->out,
             tdata->cancellable, &error))
    {
      g_printerr ("%s", error->message);
      g_clear_error (&error);
    }
  g_free (path);
  g_clear_object (&(tdata->dir));
  g_clear_object (&(tdata->out));
  return NULL;
}

gboolean
ostree_builtin_commit (int argc, char **argv, const char *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  char *dir = NULL;
  gboolean using_filename_cmdline;
  gboolean using_filedescriptors;
  GPtrArray *additions_array = NULL;
  GPtrArray *removals_array = NULL;
  GChecksum *commit_checksum = NULL;
  char **iter;
  char separator;
  GVariant *metadata = NULL;
  GMappedFile *metadata_mappedf = NULL;
  GFile *metadata_f = NULL;

  context = g_option_context_new ("[DIR] - Commit a new revision");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc > 1)
    dir = g_strdup (argv[1]);
  else
    dir = g_get_current_dir ();

  if (g_str_has_suffix (dir, "/"))
    dir[strlen (dir) - 1] = '\0';

  separator = separator_null ? '\0' : '\n';

  if (!*dir)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid empty directory");
      goto out;
    }

  if (metadata_text_path || metadata_bin_path)
    {
      metadata_mappedf = g_mapped_file_new (metadata_text_path ? metadata_text_path : metadata_bin_path, FALSE, error);
      if (!metadata_mappedf)
        goto out;
      if (metadata_text_path)
        {
          metadata = g_variant_parse (G_VARIANT_TYPE ("a{sv}"),
                                      g_mapped_file_get_contents (metadata_mappedf),
                                      g_mapped_file_get_contents (metadata_mappedf) + g_mapped_file_get_length (metadata_mappedf),
                                      NULL, error);
          if (!metadata)
            goto out;
        }
      else if (metadata_bin_path)
        {
          metadata_f = ot_util_new_file_for_path (metadata_bin_path);
          if (!ot_util_variant_map (metadata_f, G_VARIANT_TYPE ("a{sv}"), &metadata, error))
            goto out;
        }
      else
        g_assert_not_reached ();
    }

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  using_filename_cmdline = (removals || additions);
  using_filedescriptors = (from_file || from_fd >= 0 || from_stdin);

  if (using_filename_cmdline && using_filedescriptors)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "File descriptors may not be combined with --add or --remove");
      goto out;
    }

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
      
      if (!ostree_repo_commit (repo, branch, parent, subject, body, metadata,
                               dir, additions_array,
                               removals_array,
                               &commit_checksum,
                               error))
        goto out;
    }
  else if (using_filedescriptors)
    {
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
      if (!ostree_repo_commit_from_filelist_fd (repo, branch, parent, subject, body, metadata,
                                                dir, from_fd, separator,
                                                &commit_checksum, error))
        {
          if (temp_fd >= 0)
            close (temp_fd);
          goto out;
        }
      if (temp_fd >= 0)
        close (temp_fd);
    }
  else
    {
      int pipefd[2];
      GOutputStream *out;
      FindThreadData fdata;

      if (pipe (pipefd) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }

      out = (GOutputStream*)g_unix_output_stream_new (pipefd[1], TRUE);

      fdata.dir = ot_util_new_file_for_path (dir);
      fdata.separator = separator;
      fdata.out = out;
      fdata.cancellable = NULL;

      if (g_thread_create_full (find_thread, &fdata, 0, FALSE, FALSE, G_THREAD_PRIORITY_NORMAL, error) == NULL)
        goto out;

      if (!ostree_repo_commit_from_filelist_fd (repo, branch, parent, subject, body, metadata,
                                                dir, pipefd[0], separator,
                                                &commit_checksum, error))
        goto out;

      (void)close (pipefd[0]);
    }
 
  ret = TRUE;
  g_print ("%s\n", g_checksum_get_string (commit_checksum));
 out:
  g_free (dir);
  if (metadata_mappedf)
    g_mapped_file_unref (metadata_mappedf);
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
