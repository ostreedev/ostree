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
#include "ostree.h"

#include <glib/gi18n.h>

typedef struct {
  OstreeRepo  *repo;
} OtAdminDeploy;

static gboolean opt_no_kernel;
static char *opt_ostree_dir;

static GOptionEntry options[] = {
  { "ostree-dir", 0, 0, G_OPTION_ARG_STRING, &opt_ostree_dir, "Path to OSTree root directory", NULL },
  { "no-kernel", 0, 0, G_OPTION_ARG_NONE, &opt_no_kernel, "Don't update kernel related config (initramfs, bootloader)", NULL },
  { NULL }
};

static gboolean
update_current (const char         *deploy_target,
                GCancellable       *cancellable,
                GError            **error)
{
  gboolean ret = FALSE;
  ot_lfree char *tmp_symlink = NULL;
  ot_lfree char *current_name = NULL;

  tmp_symlink = g_build_filename (opt_ostree_dir, "tmp-current", NULL);
  (void) unlink (tmp_symlink);

  if (symlink (deploy_target, tmp_symlink) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  current_name = g_build_filename (opt_ostree_dir, "current", NULL);
  if (rename (tmp_symlink, current_name) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  g_print ("%s set to %s\n", current_name, deploy_target);

  ret = TRUE;
 out:
  return ret;
}

static gboolean
do_checkout (OtAdminDeploy     *self,
             const char        *deploy_target,
             const char        *revision,
             GCancellable      *cancellable,
             GError           **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *deploy_path = NULL;
  ot_lobj GFile *deploy_parent = NULL;
  ot_lfree char *tree_ref = NULL;
  ot_lfree char *repo_path = NULL;
  ot_lfree char *repo_arg = NULL;
  ot_lptrarray GPtrArray *checkout_args = NULL;

  repo_path = g_build_filename (opt_ostree_dir, "repo", NULL);
  repo_arg = g_strconcat ("--repo=", repo_path, NULL);

  deploy_path = ot_gfile_from_build_path (opt_ostree_dir, deploy_target, NULL);
  deploy_parent = g_file_get_parent (deploy_path);
  if (!ot_gfile_ensure_directory (deploy_parent, TRUE, error))
    goto out;

  checkout_args = g_ptr_array_new ();
  ot_ptrarray_add_many (checkout_args, "ostree", repo_arg,
                        "checkout", "--atomic-retarget", revision ? revision : deploy_target,
                        ot_gfile_get_path_cached (deploy_path), NULL);
  g_ptr_array_add (checkout_args, NULL);

  if (!ot_spawn_sync_checked (opt_ostree_dir, (char**)checkout_args->pdata, NULL, G_SPAWN_SEARCH_PATH,
                              NULL, NULL, NULL, NULL, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
do_update_kernel (OtAdminDeploy     *self,
                  const char        *deploy_target,
                  GCancellable      *cancellable,
                  GError           **error)
{
  gboolean ret = FALSE;
  ot_lptrarray GPtrArray *args = NULL;

  args = g_ptr_array_new ();
  ot_ptrarray_add_many (args, "ostadmin", "update-kernel",
                        "--ostree-dir", opt_ostree_dir,
                        deploy_target, NULL);
  g_ptr_array_add (args, NULL);

  if (!ot_spawn_sync_checked (opt_ostree_dir, (char**)args->pdata, NULL, G_SPAWN_SEARCH_PATH,
                              NULL, NULL, NULL, NULL, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}


gboolean
ot_admin_builtin_deploy (int argc, char **argv, GError **error)
{
  GOptionContext *context;
  OtAdminDeploy self_data;
  OtAdminDeploy *self = &self_data;
  gboolean ret = FALSE;
  const char *deploy_target = NULL;
  const char *revision = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  if (!opt_ostree_dir)
    opt_ostree_dir = "/ostree";

  memset (self, 0, sizeof (*self));

  context = g_option_context_new ("NAME [REVISION] - Check out revision NAME (or REVISION as NAME)");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "NAME must be specified", error);
      goto out;
    }
    
  deploy_target = argv[1];
  if (argc > 2)
    revision = argv[2];

  if (!do_checkout (self, deploy_target, revision, cancellable, error))
    goto out;

  if (!opt_no_kernel)
    {
      if (!do_update_kernel (self, deploy_target, cancellable, error))
        goto out;
    }
  
  if (!update_current (deploy_target, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
