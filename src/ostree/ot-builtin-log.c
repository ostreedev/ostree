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
#include "ostree.h"

#include <glib/gi18n.h>

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ostree_builtin_log (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  const char *rev;
  ot_lobj OstreeRepo *repo = NULL;
  ot_lobj GOutputStream *pager = NULL;
  ot_lvariant GVariant *commit = NULL;
  ot_lfree char *resolved_rev = NULL;

  context = g_option_context_new ("- Show revision log");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "A revision must be specified");
      goto out;
    }
                   
  rev = argv[1];

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (!ot_util_spawn_pager (&pager, error))
    goto out;

  if (!ostree_repo_resolve_rev (repo, rev, FALSE, &resolved_rev, error))
    goto out;

  while (TRUE)
    {
      char *formatted = NULL;
      GVariant *parent_csum_v = NULL;
      const char *subject;
      const char *body;
      guint64 timestamp;
      GVariant *content_csum_v = NULL;
      GVariant *metadata_csum_v = NULL;
      GDateTime *time_obj = NULL;
      char *formatted_date = NULL;
      const char *body_newline;
      gsize bytes_written;
      GVariant *commit_metadata = NULL;
      char *formatted_metadata = NULL;
      
      ot_clear_gvariant (&commit);
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, resolved_rev, &commit, error))
        goto out;

      /* Ignore commit metadata for now */
      ot_clear_gvariant (&commit_metadata);
      ot_clear_gvariant (&parent_csum_v);
      ot_clear_gvariant (&content_csum_v);
      ot_clear_gvariant (&metadata_csum_v);
      g_variant_get (commit, "(@a{sv}@ay@a(say)&s&st@ay@ay)",
                     &commit_metadata, &parent_csum_v, NULL, &subject, &body,
                     &timestamp, &content_csum_v, &metadata_csum_v);
      timestamp = GUINT64_FROM_BE (timestamp);
      time_obj = g_date_time_new_from_unix_utc (timestamp);
      formatted_date = g_date_time_format (time_obj, "%a %b %d %H:%M:%S %Y %z");
      g_date_time_unref (time_obj);
      time_obj = NULL;

      ot_clear_gvariant (&commit_metadata);
      formatted = g_strdup_printf ("commit %s\nSubject: %s\nDate: %s\nMetadata: %s\n\n",
                                   resolved_rev, subject, formatted_date, formatted_metadata);
      g_free (formatted_metadata);
      g_free (formatted_date);
      formatted_date = NULL;
      
      if (!g_output_stream_write_all (pager, formatted, strlen (formatted), &bytes_written, NULL, error))
        {
          g_free (formatted);
          goto out;
        }
      g_free (formatted);
      
      body_newline = strchr (body, '\n');
      do {
        gsize len;
        if (!g_output_stream_write_all (pager, "    ", 4, &bytes_written, NULL, error))
          goto out;
        len = body_newline ? body_newline - body : strlen (body);
        if (!g_output_stream_write_all (pager, body, len, &bytes_written, NULL, error))
          goto out;
        if (!g_output_stream_write_all (pager, "\n\n", 2, &bytes_written, NULL, error))
          goto out;
        body_newline = strchr (body, '\n');
        if (!body_newline)
          break;
        else
          body_newline += 1;
      } while (*body_newline);

      if (g_variant_n_children (parent_csum_v) == 0)
        break;
      g_free (resolved_rev);
      resolved_rev = ostree_checksum_from_bytes_v (parent_csum_v);
    }

  if (!g_output_stream_close (pager, NULL, error))
    goto out;
 
  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
