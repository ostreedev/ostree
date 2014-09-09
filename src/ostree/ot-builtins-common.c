/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

#include "ot-builtins-common.h"
#include "otutil.h"

void
ot_common_pull_progress (OstreeAsyncProgress       *progress,
                         gpointer                   user_data)
{
  GSConsole *console = user_data;
  GString *buf;
  gs_free char *status = NULL;
  guint outstanding_fetches;
  guint outstanding_writes;
  guint n_scanned_metadata;

  if (!console)
    return;

  buf = g_string_new ("");

  status = ostree_async_progress_get_status (progress);
  outstanding_fetches = ostree_async_progress_get_uint (progress, "outstanding-fetches");
  outstanding_writes = ostree_async_progress_get_uint (progress, "outstanding-writes");
  n_scanned_metadata = ostree_async_progress_get_uint (progress, "scanned-metadata");
  if (status)
    {
      g_string_append (buf, status);
    }
  else if (outstanding_fetches)
    {
      guint64 bytes_transferred = ostree_async_progress_get_uint64 (progress, "bytes-transferred");
      guint fetched = ostree_async_progress_get_uint (progress, "fetched");
      guint requested = ostree_async_progress_get_uint (progress, "requested");
      guint64 bytes_sec = (g_get_monotonic_time () - ostree_async_progress_get_uint64 (progress, "start-time")) / G_USEC_PER_SEC;
      gs_free char *formatted_bytes_transferred =
        g_format_size_full (bytes_transferred, 0);
      gs_free char *formatted_bytes_sec = NULL;

      if (!bytes_sec) // Ignore first second
        formatted_bytes_sec = g_strdup ("-");
      else
        {
          bytes_sec = bytes_transferred / bytes_sec;
          formatted_bytes_sec = g_format_size (bytes_sec);
        }

      g_string_append_printf (buf, "Receiving objects: %u%% (%u/%u) %s/s %s",
                              (guint)((((double)fetched) / requested) * 100),
                              fetched, requested, formatted_bytes_sec, formatted_bytes_transferred);
    }
  else if (outstanding_writes)
    {
      g_string_append_printf (buf, "Writing objects: %u", outstanding_writes);
    }
  else
    {
      g_string_append_printf (buf, "Scanning metadata: %u", n_scanned_metadata);
    }

  gs_console_begin_status_line (console, buf->str, NULL, NULL);
  
  g_string_free (buf, TRUE);
  
}

gboolean
ot_common_parse_statoverride_file (const char    *statoverride_file,
                                   GHashTable   **out_mode_add,
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

  path = g_file_new_for_path (statoverride_file);

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

gboolean
ot_common_commit_editor (OstreeRepo     *repo,
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

