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

static gboolean
parse_file_or_commit (OstreeRepo  *repo,
                      const char  *arg,
                      GFile      **out_file,
                      GCancellable *cancellable,
                      GError     **error)
{
  gboolean ret = FALSE;
  GFile *ret_file = NULL;

  if (g_str_has_prefix (arg, "/")
      || g_str_has_prefix (arg, "./"))
    {
      ret_file = ot_util_new_file_for_path (arg);
    }
  else
    {
      if (!ostree_repo_read_commit (repo, arg, &ret_file, cancellable, NULL))
        goto out;
    }

  ret = TRUE;
  *out_file = ret_file;
  ret_file = NULL;
 out:
  g_clear_object (&ret_file);
  return ret;
}

gboolean
ostree_builtin_diff (int argc, char **argv, const char *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  const char *src;
  const char *target;
  GFile *srcf = NULL;
  GFile *targetf = NULL;
  GFile *cwd = NULL;
  GPtrArray *modified = NULL;
  GPtrArray *removed = NULL;
  GPtrArray *added = NULL;
  int i;

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

  src = argv[1];
  target = argv[2];

  cwd = ot_util_new_file_for_path (".");

  if (!parse_file_or_commit (repo, src, &srcf, NULL, error))
    goto out;
  if (!parse_file_or_commit (repo, target, &targetf, NULL, error))
    goto out;
  
  if (!ostree_repo_diff (repo, srcf, targetf, &modified, &removed, &added, NULL, error))
    goto out;

  for (i = 0; i < modified->len; i++)
    {
      OstreeRepoDiffItem *diff = modified->pdata[i];
      g_print ("M    %s\n", ot_gfile_get_path_cached (diff->src));
    }
  for (i = 0; i < removed->len; i++)
    {
      g_print ("D    %s\n", ot_gfile_get_path_cached (removed->pdata[i]));
    }
  for (i = 0; i < added->len; i++)
    {
      GFile *added_f = added->pdata[i];
      if (g_file_is_native (added_f))
        {
          char *relpath = g_file_get_relative_path (cwd, added_f);
          g_assert (relpath != NULL);
          g_print ("A    %s\n", relpath);
          g_free (relpath);
        }
      else
        g_print ("A    %s\n", ot_gfile_get_path_cached (added_f));
    }

  ret = TRUE;
 out:
  g_clear_object (&repo);
  g_clear_object (&cwd);
  g_clear_object (&srcf);
  g_clear_object (&targetf);
  if (modified)
    g_ptr_array_free (modified, TRUE);
  if (removed)
    g_ptr_array_free (removed, TRUE);
  if (added)
    g_ptr_array_free (added, TRUE);
  return ret;
}
