/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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

#include "ot-admin-builtins.h"
#include "otutil.h"

#include <glib/gi18n.h>

static char *opt_ostree_dir = "/ostree";

static GOptionEntry options[] = {
  { "ostree-dir", 0, 0, G_OPTION_ARG_STRING, &opt_ostree_dir, "Path to OSTree root directory (default: /ostree)", NULL },
  { NULL }
};


gboolean
ot_admin_builtin_init (int argc, char **argv, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  ot_lobj GFile *dir = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  context = g_option_context_new ("- Initialize /ostree directory");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  g_clear_object (&dir);
  dir = ot_gfile_from_build_path (opt_ostree_dir, "repo", NULL);
  if (!ot_gfile_ensure_directory (dir, TRUE, error))
    goto out;

  /* We presently copy over host kernel modules */
  g_clear_object (&dir);
  dir = ot_gfile_from_build_path (opt_ostree_dir, "modules", NULL);
  if (!ot_gfile_ensure_directory (dir, TRUE, error))
    goto out;

  g_clear_object (&dir);
  dir = ot_gfile_from_build_path (opt_ostree_dir, "repo", "objects", NULL);
  if (!g_file_query_exists (dir, NULL))
    {
      ot_lfree char *opt_repo_path = g_strdup_printf ("--repo=%s/repo", opt_ostree_dir);
      const char *child_argv[] = { "ostree", opt_repo_path, "init", NULL };

      if (!ot_spawn_sync_checked (NULL, (char**)child_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                                  NULL, NULL, error))
        {
          g_prefix_error (error, "Failed to initialize repository: ");
          goto out;
        }
    }

  /* Ensure a few subdirectories of /var exist, since we need them for
     dracut generation */
  g_clear_object (&dir);
  dir = ot_gfile_from_build_path (opt_ostree_dir, "var", "log", NULL);
  if (!ot_gfile_ensure_directory (dir, TRUE, error))
    goto out;
  g_clear_object (&dir);
  dir = ot_gfile_from_build_path (opt_ostree_dir, "var", "tmp", NULL);
  if (!ot_gfile_ensure_directory (dir, TRUE, error))
    goto out;
  if (chmod (ot_gfile_get_path_cached (dir), 01777) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  g_print ("%s initialized as OSTree root\n", opt_ostree_dir);

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
