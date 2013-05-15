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

#include <unistd.h>
#include <stdlib.h>
#include <glib/gi18n.h>

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ot_admin_builtin_upgrade (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  GFile *ostree_dir = admin_opts->ostree_dir;
  gs_free char *booted_osname = NULL;
  const char *osname = NULL;
  gs_free char *ostree_dir_arg = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  context = g_option_context_new ("[OSNAME] - pull, deploy, and prune");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc > 1)
    {
      osname = argv[1];
    }
  else
    {
      if (!ot_admin_get_active_deployment (NULL, &booted_osname, NULL, cancellable, error))
        goto out;
      if (booted_osname == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Not in an active OSTree system; must specify OSNAME");
          goto out;
        }
      osname = booted_osname;
    }

  ostree_dir_arg = g_strconcat ("--ostree-dir=",
                                gs_file_get_path_cached (ostree_dir),
                                NULL);

  if (!gs_subprocess_simple_run_sync (gs_file_get_path_cached (ostree_dir),
                                      GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                      cancellable, error,
                                      "ostree", "admin", ostree_dir_arg, "pull-deploy", osname, NULL))
    goto out;

  if (!gs_subprocess_simple_run_sync (gs_file_get_path_cached (ostree_dir),
                                      GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                      cancellable, error,
                                      "ostree", "admin", ostree_dir_arg, "prune", osname, NULL))
    goto out;

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
