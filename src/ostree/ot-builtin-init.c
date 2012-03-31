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

static gboolean archive;

static GOptionEntry options[] = {
  { "archive", 0, 0, G_OPTION_ARG_NONE, &archive, "Initialize repository as archive", NULL },
  { NULL }
};

#define DEFAULT_CONFIG_CONTENTS ("[core]\n" \
                                 "repo_version=0\n")


gboolean
ostree_builtin_init (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context = NULL;
  gboolean ret = FALSE;
  GFile *child = NULL;
  GFile *grandchild = NULL;
  GCancellable *cancellable = NULL;
  GString *config_data = NULL;
  OstreeRepo *repo = NULL;

  context = g_option_context_new ("- Initialize a new empty repository");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  child = g_file_get_child (repo_path, "config");

  config_data = g_string_new (DEFAULT_CONFIG_CONTENTS);
  g_string_append_printf (config_data, "mode=%s\n", archive ? "archive" : "bare");
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

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (!ostree_repo_regenerate_pack_index (repo, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  if (config_data)
    g_string_free (config_data, TRUE);
  g_clear_object (&child);
  g_clear_object (&grandchild);
  g_clear_object (&repo);
  return ret;
}
