/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"
#include "otutil.h"

#include "../libostree/ostree-kernel-args.h"

#include <glib/gi18n.h>

static gboolean opt_retain;
static char *opt_osname;

static GOptionEntry options[] = {
  { "retain", 0, 0, G_OPTION_ARG_NONE, &opt_retain, "Do not delete previous deployment", NULL },
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Use a different operating system root than the current one", "OSNAME" },
  { NULL }
};

gboolean
ot_admin_builtin_dup (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  g_autoptr(GPtrArray) new_deployments = NULL;
  glnx_unref_object OstreeDeployment *new_deployment = NULL;
  glnx_unref_object OstreeDeployment *merge_deployment = NULL;
  g_autofree char *revision = NULL;
  __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *kargs = NULL;

  context = g_option_context_new ("Clone the current deployment as rollback target");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER,
                                          &sysroot, cancellable, error))
    goto out;

  if (argc > 1)
    {
      ot_util_usage_error (context, "This command takes no extra arguments", error);
      goto out;
    }

  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  /* Find the currently booted deployment, if any; we will ensure it
   * is present in the new deployment list.
   */
  if (!ot_admin_require_booted_deployment_or_osname (sysroot, opt_osname,
                                                     cancellable, error))
    {
      g_prefix_error (error, "Looking for booted deployment: ");
      goto out;
    }

  merge_deployment = ostree_sysroot_get_merge_deployment (sysroot, opt_osname);
  if (!merge_deployment)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No previous deployment to duplicate");
      goto out;
    }

  /* Ensure we have a clean slate */
  if (!ostree_sysroot_prepare_cleanup (sysroot, cancellable, error))
    {
      g_prefix_error (error, "Performing initial cleanup: ");
      goto out;
    }

  kargs = _ostree_kernel_args_new ();

  { OstreeBootconfigParser *bootconfig = ostree_deployment_get_bootconfig (merge_deployment);
    g_auto(GStrv) previous_args = g_strsplit (ostree_bootconfig_parser_get (bootconfig, "options"), " ", -1);
    
    _ostree_kernel_args_append_argv (kargs, previous_args);
  }

  {
    g_auto(GStrv) kargs_strv = _ostree_kernel_args_to_strv (kargs);

    if (!ostree_sysroot_deploy_tree (sysroot,
                                     opt_osname,
                                     ostree_deployment_get_csum (merge_deployment),
                                     ostree_deployment_get_origin (merge_deployment),
                                     merge_deployment,
                                     kargs_strv,
                                     &new_deployment,
                                     cancellable, error))
      goto out;
  }


  {
    OstreeSysrootSimpleWriteDeploymentFlags deploy_flags = OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NOT_DEFAULT;
    if (opt_retain)
      deploy_flags |= OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN;
    if (!ostree_sysroot_simple_write_deployment (sysroot, opt_osname,
                                                 new_deployment, merge_deployment,
                                                 deploy_flags,
                                                 cancellable, error))
      goto out;
  }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
