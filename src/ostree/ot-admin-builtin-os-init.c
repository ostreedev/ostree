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
#include "ot-admin-functions.h"
#include "otutil.h"

#include <glib/gi18n.h>

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ot_admin_builtin_os_init (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  const char *osname = NULL;
  GFile *ostree_dir = admin_opts->ostree_dir;
  ot_lobj GFile *deploy_dir = NULL;
  ot_lobj GFile *dir = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  context = g_option_context_new ("OSNAME - Initialize empty state for given operating system");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (!ot_admin_ensure_initialized (ostree_dir, cancellable, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "OSNAME must be specified", error);
      goto out;
    }

  osname = argv[1];

  deploy_dir = ot_gfile_get_child_build_path (ostree_dir, "deploy", osname, NULL);

  /* Ensure core subdirectories of /var exist, since we need them for
   * dracut generation, and the host will want them too.
   */
  g_clear_object (&dir);
  dir = ot_gfile_get_child_build_path (deploy_dir, "var", "log", NULL);
  if (!gs_file_ensure_directory (dir, TRUE, cancellable, error))
    goto out;

  g_clear_object (&dir);
  dir = ot_gfile_get_child_build_path (deploy_dir, "var", "tmp", NULL);
  if (!gs_file_ensure_directory (dir, TRUE, cancellable, error))
    goto out;
  if (chmod (gs_file_get_path_cached (dir), 01777) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  g_clear_object (&dir);
  dir = ot_gfile_get_child_build_path (deploy_dir, "var", "lib", NULL);
  if (!gs_file_ensure_directory (dir, TRUE, cancellable, error))
    goto out;

  g_clear_object (&dir);
  dir = ot_gfile_get_child_build_path (deploy_dir, "var", "run", NULL);
  if (!g_file_test (gs_file_get_path_cached (dir), G_FILE_TEST_IS_SYMLINK))
    {
      if (symlink ("../run", gs_file_get_path_cached (dir)) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  g_print ("%s initialized as OSTree root\n", gs_file_get_path_cached (deploy_dir));

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
