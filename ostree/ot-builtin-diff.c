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

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ostree_builtin_diff (int argc, char **argv, const char *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  const char *target;
  const char *rev;
  GFile *targetf = NULL;
  GPtrArray *modified = NULL;
  GPtrArray *removed = NULL;
  GPtrArray *added = NULL;

  context = g_option_context_new ("REV TARGETDIR - Compare directory TARGETDIR against revision REV");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (argc < 3)
    {
      gchar *help = g_option_context_get_help (context, TRUE, NULL);
      g_printerr ("%s\n", help);
      g_free (help);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "REV and TARGETDIR must be specified");
      goto out;
    }

  rev = argv[1];
  target = argv[2];
  targetf = ot_util_new_file_for_path (target);
  
  if (!ostree_repo_diff (repo, rev, targetf, &modified, &removed, &added, NULL, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_object (&repo);
  g_clear_object (&targetf);
  if (modified)
    g_ptr_array_free (modified, TRUE);
  if (removed)
    g_ptr_array_free (removed, TRUE);
  if (added)
    g_ptr_array_free (added, TRUE);
  return ret;
}
