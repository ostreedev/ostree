/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012,2013 Colin Walters <walters@verbum.org>
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
static char **opt_kernel_argv;
static char **opt_kernel_argv_append;
static gboolean opt_kernel_proc_cmdline;
static char *opt_osname;
static char *opt_origin_path;

static GOptionEntry options[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Use a different operating system root than the current one", "OSNAME" },
  { "origin-file", 0, 0, G_OPTION_ARG_FILENAME, &opt_origin_path, "Specify origin file", "FILENAME" },
  { "retain", 0, 0, G_OPTION_ARG_NONE, &opt_retain, "Do not delete previous deployment", NULL },
  { "karg-proc-cmdline", 0, 0, G_OPTION_ARG_NONE, &opt_kernel_proc_cmdline, "Import current /proc/cmdline", NULL },
  { "karg", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_argv, "Set kernel argument, like root=/dev/sda1; this overrides any earlier argument with the same name", "NAME=VALUE" },
  { "karg-append", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_argv_append, "Append kernel argument; useful with e.g. console= that can be used multiple times", "NAME=VALUE" },
  { NULL }
};

gboolean
ot_admin_builtin_deploy (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  const char *refspec;
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  GKeyFile *origin = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  glnx_unref_object OstreeDeployment *new_deployment = NULL;
  glnx_unref_object OstreeDeployment *merge_deployment = NULL;
  g_autofree char *revision = NULL;
  __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *kargs = NULL;

  context = g_option_context_new ("REFSPEC - Checkout revision REFSPEC as the new default deployment");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER,
                                          &sysroot, cancellable, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "REF/REV must be specified", error);
      goto out;
    }

  refspec = argv[1];

  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
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

  if (opt_origin_path)
    {
      origin = g_key_file_new ();
      
      if (!g_key_file_load_from_file (origin, opt_origin_path, 0, error))
        goto out;
    }
  else
    {
      origin = ostree_sysroot_origin_new_from_refspec (sysroot, refspec);
    }

  if (!ostree_repo_resolve_rev (repo, refspec, FALSE, &revision, error))
    goto out;

  merge_deployment = ostree_sysroot_get_merge_deployment (sysroot, opt_osname);

  /* Here we perform cleanup of any leftover data from previous
   * partial failures.  This avoids having to call
   * glnx_shutil_rm_rf_at() at random points throughout the process.
   *
   * TODO: Add /ostree/transaction file, and only do this cleanup if
   * we find it.
   */
  if (!ostree_sysroot_prepare_cleanup (sysroot, cancellable, error))
    {
      g_prefix_error (error, "Performing initial cleanup: ");
      goto out;
    }

  kargs = _ostree_kernel_args_new ();

  /* If they want the current kernel's args, they very likely don't
   * want the ones from the merge.
   */
  if (opt_kernel_proc_cmdline)
    {
      if (!_ostree_kernel_args_append_proc_cmdline (kargs, cancellable, error))
        goto out;
    }
  else if (merge_deployment)
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

  {
    g_auto(GStrv) kargs_strv = _ostree_kernel_args_to_strv (kargs);

    if (!ostree_sysroot_deploy_tree (sysroot,
                                     opt_osname, revision, origin,
                                     merge_deployment, kargs_strv,
                                     &new_deployment,
                                     cancellable, error))
      goto out;
  }

  if (!ostree_sysroot_simple_write_deployment (sysroot, opt_osname,
                                               new_deployment, merge_deployment,
                                               opt_retain ? OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN : 0,
                                               cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (origin)
    g_key_file_unref (origin);
  return ret;
}
