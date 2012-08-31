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
  ot_lfree GFile *ret_file = NULL;

  if (g_str_has_prefix (arg, "/")
      || g_str_has_prefix (arg, "./")
      )
    {
      ret_file = g_file_new_for_path (arg);
    }
  else
    {
      if (!ostree_repo_read_commit (repo, arg, &ret_file, cancellable, error))
        goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_file, &ret_file);
 out:
  return ret;
}

gboolean
ostree_builtin_diff (int argc, char **argv, GFile *repo_path, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  GCancellable *cancellable = NULL;
  int i;
  const char *src;
  const char *target;
  ot_lobj OstreeRepo *repo = NULL;
  ot_lfree char *src_prev = NULL;
  ot_lobj GFile *srcf = NULL;
  ot_lobj GFile *targetf = NULL;
  ot_lobj GFile *cwd = NULL;
  ot_lptrarray GPtrArray *modified = NULL;
  ot_lptrarray GPtrArray *removed = NULL;
  ot_lptrarray GPtrArray *added = NULL;

  context = g_option_context_new ("REV TARGETDIR - Compare directory TARGETDIR against revision REV");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (argc < 2)
    {
      gchar *help = g_option_context_get_help (context, TRUE, NULL);
      g_printerr ("%s\n", help);
      g_free (help);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "REV must be specified");
      goto out;
    }

  if (argc == 2)
    {
      src_prev = g_strconcat (argv[1], "^", NULL);
      src = src_prev;
      target = argv[1];
    }
  else
    {
      src = argv[1];
      target = argv[2];
    }

  cwd = g_file_new_for_path (".");

  if (!parse_file_or_commit (repo, src, &srcf, cancellable, error))
    goto out;
  if (!parse_file_or_commit (repo, target, &targetf, cancellable, error))
    goto out;

  modified = g_ptr_array_new_with_free_func ((GDestroyNotify)ostree_diff_item_unref);
  removed = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  added = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  
  if (!ostree_diff_dirs (srcf, targetf, modified, removed, added, cancellable, error))
    goto out;

  ostree_diff_print (srcf, modified, removed, added);

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
