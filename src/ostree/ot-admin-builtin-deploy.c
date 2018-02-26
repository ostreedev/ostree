/*
 * Copyright (C) 2012,2013 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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

#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"
#include "otutil.h"

#include "../libostree/ostree-kernel-args.h"

#include <glib/gi18n.h>

static gboolean opt_retain;
static gboolean opt_retain_pending;
static gboolean opt_retain_rollback;
static gboolean opt_not_as_default;
static gboolean opt_no_prune;
static char **opt_kernel_argv;
static char **opt_kernel_argv_append;
static gboolean opt_kernel_proc_cmdline;
static char *opt_osname;
static char *opt_origin_path;
static gboolean opt_kernel_arg_none;

static GOptionEntry options[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Use a different operating system root than the current one", "OSNAME" },
  { "origin-file", 0, 0, G_OPTION_ARG_FILENAME, &opt_origin_path, "Specify origin file", "FILENAME" },
  { "no-prune", 0, 0, G_OPTION_ARG_NONE, &opt_no_prune, "Don't prune the repo when done", NULL},
  { "retain", 0, 0, G_OPTION_ARG_NONE, &opt_retain, "Do not delete previous deployments", NULL },
  { "retain-pending", 0, 0, G_OPTION_ARG_NONE, &opt_retain_pending, "Do not delete pending deployments", NULL },
  { "retain-rollback", 0, 0, G_OPTION_ARG_NONE, &opt_retain_rollback, "Do not delete rollback deployments", NULL },
  { "not-as-default", 0, 0, G_OPTION_ARG_NONE, &opt_not_as_default, "Append rather than prepend new deployment", NULL },
  { "karg-proc-cmdline", 0, 0, G_OPTION_ARG_NONE, &opt_kernel_proc_cmdline, "Import current /proc/cmdline", NULL },
  { "karg", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_argv, "Set kernel argument, like root=/dev/sda1; this overrides any earlier argument with the same name", "NAME=VALUE" },
  { "karg-append", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_argv_append, "Append kernel argument; useful with e.g. console= that can be used multiple times", "NAME=VALUE" },
  { "karg-none", 0, 0, G_OPTION_ARG_NONE, &opt_kernel_arg_none, "Do not import kernel arguments", NULL },
  { NULL }
};

gboolean
ot_admin_builtin_deploy (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(OstreeKernelArgs) kargs = NULL;

  g_autoptr(GOptionContext) context =
    g_option_context_new ("REFSPEC");

  g_autoptr(OstreeSysroot) sysroot = NULL;
  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER,
                                          invocation, &sysroot, cancellable, error))
    return FALSE;

  if (argc < 2)
    {
      ot_util_usage_error (context, "REF/REV must be specified", error);
      return FALSE;
    }

  if (opt_kernel_proc_cmdline && opt_kernel_arg_none)
  {
    ot_util_usage_error (context, "Can't specify both --karg-proc-cmdline and --karg-none", error);
    return FALSE;
  }

  const char *refspec = argv[1];

  OstreeRepo *repo = ostree_sysroot_repo (sysroot);

  /* Find the currently booted deployment, if any; we will ensure it
   * is present in the new deployment list.
   */
  if (!ot_admin_require_booted_deployment_or_osname (sysroot, opt_osname,
                                                     cancellable, error))
    return glnx_prefix_error (error, "Looking for booted deployment");

  g_autoptr(GKeyFile) origin = NULL;
  if (opt_origin_path)
    {
      origin = g_key_file_new ();

      if (!g_key_file_load_from_file (origin, opt_origin_path, 0, error))
        return FALSE;
    }
  else
    {
      origin = ostree_sysroot_origin_new_from_refspec (sysroot, refspec);
    }

  g_autofree char *revision = NULL;
  if (!ostree_repo_resolve_rev (repo, refspec, FALSE, &revision, error))
    return FALSE;

  g_autoptr(OstreeDeployment) merge_deployment =
    ostree_sysroot_get_merge_deployment (sysroot, opt_osname);

  /* Here we perform cleanup of any leftover data from previous
   * partial failures.  This avoids having to call
   * glnx_shutil_rm_rf_at() at random points throughout the process.
   *
   * TODO: Add /ostree/transaction file, and only do this cleanup if
   * we find it.
   */
  if (!ostree_sysroot_prepare_cleanup (sysroot, cancellable, error))
    return glnx_prefix_error (error, "Performing initial cleanup");

  kargs = _ostree_kernel_args_new ();

  /* If they want the current kernel's args, they very likely don't
   * want the ones from the merge.
   */
  if (opt_kernel_proc_cmdline)
    {
      if (!_ostree_kernel_args_append_proc_cmdline (kargs, cancellable, error))
        return FALSE;
    }
  else if (merge_deployment && !opt_kernel_arg_none)
    {
      OstreeBootconfigParser *bootconfig = ostree_deployment_get_bootconfig (merge_deployment);
      g_auto(GStrv) previous_args = g_strsplit (ostree_bootconfig_parser_get (bootconfig, "options"), " ", -1);

      _ostree_kernel_args_append_argv (kargs, previous_args);
    }

  if (opt_kernel_argv)
    {
      _ostree_kernel_args_replace_argv (kargs, opt_kernel_argv);
    }

  if (opt_kernel_argv_append)
    {
      _ostree_kernel_args_append_argv (kargs, opt_kernel_argv_append);
    }

  g_autoptr(OstreeDeployment) new_deployment = NULL;
  g_auto(GStrv) kargs_strv = _ostree_kernel_args_to_strv (kargs);
  if (!ostree_sysroot_deploy_tree (sysroot, opt_osname, revision, origin, merge_deployment,
                                   kargs_strv, &new_deployment, cancellable, error))
    return FALSE;

  OstreeSysrootSimpleWriteDeploymentFlags flags = OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NO_CLEAN;
  if (opt_retain)
    flags |= OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN;
  else
    {
      if (opt_retain_pending)
        flags |= OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN_PENDING;
      if (opt_retain_rollback)
        flags |= OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN_ROLLBACK;
    }

  if (opt_not_as_default)
    flags |= OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NOT_DEFAULT;

  if (!ostree_sysroot_simple_write_deployment (sysroot, opt_osname, new_deployment,
                                               merge_deployment, flags, cancellable, error))
    return FALSE;

  /* And finally, cleanup of any leftover data.
   */
  if (opt_no_prune)
    {
      if (!ostree_sysroot_prepare_cleanup (sysroot, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!ostree_sysroot_cleanup (sysroot, cancellable, error))
        return FALSE;
    }

  return TRUE;
}
