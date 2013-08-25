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
#include "libgsystem.h"

static char *opt_mode = NULL;

static GOptionEntry options[] = {
  { "mode", 0, 0, G_OPTION_ARG_STRING, &opt_mode, "Initialize repository in given mode (bare, archive-z2)", NULL },
  { NULL }
};

#define DEFAULT_CONFIG_CONTENTS ("[core]\n" \
                                 "repo_version=1\n")


gboolean
ostree_builtin_init (int argc, char **argv, OstreeRepo *repo, GCancellable *cancellable, GError **error)
{
  GOptionContext *context = NULL;
  gboolean ret = FALSE;
  const char *mode_str = "bare";
  GFile *repo_path = NULL;
  gs_unref_object GFile *child = NULL;
  gs_unref_object GFile *grandchild = NULL;
  GString *config_data = NULL;

  context = g_option_context_new ("- Initialize a new empty repository");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo_path = ostree_repo_get_path (repo);

  child = g_file_get_child (repo_path, "config");

  config_data = g_string_new (DEFAULT_CONFIG_CONTENTS);
  if (opt_mode)
    {
      OstreeRepoMode mode;
      if (!ostree_repo_mode_from_string (opt_mode, &mode, error))
        goto out;
      mode_str = opt_mode;
    }
  g_string_append_printf (config_data, "mode=%s\n", mode_str);
  if (!g_file_replace_contents (child,
                                config_data->str,
                                config_data->len,
                                NULL, FALSE, 0, NULL,
                                NULL, error))
    goto out;

  g_clear_object (&child);
  child = g_file_get_child (repo_path, "objects");
  if (!g_file_make_directory (child, NULL, error))
    goto out;

  g_clear_object (&grandchild);
  grandchild = g_file_get_child (child, "pack");
  if (!g_file_make_directory (grandchild, NULL, error))
    goto out;

  g_clear_object (&child);
  child = g_file_get_child (repo_path, "tmp");
  if (!g_file_make_directory (child, NULL, error))
    goto out;

  g_clear_object (&child);
  child = g_file_get_child (repo_path, "refs");
  if (!g_file_make_directory (child, NULL, error))
    goto out;

  g_clear_object (&grandchild);
  grandchild = g_file_get_child (child, "heads");
  if (!g_file_make_directory (grandchild, NULL, error))
    goto out;

  g_clear_object (&grandchild);
  grandchild = g_file_get_child (child, "remotes");
  if (!g_file_make_directory (grandchild, NULL, error))
    goto out;

  g_clear_object (&child);
  child = g_file_get_child (repo_path, "tags");
  if (!g_file_make_directory (child, NULL, error))
    goto out;

  g_clear_object (&child);
  child = g_file_get_child (repo_path, "remote-cache");
  if (!g_file_make_directory (child, NULL, error))
    goto out;

  if (!ostree_repo_check (repo, error))
    goto out;

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  if (config_data)
    g_string_free (config_data, TRUE);
  return ret;
}
