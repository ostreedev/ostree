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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ostree-sysroot-private.h"

#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"
#include "otutil.h"

#include <glib/gi18n.h>

static gboolean opt_retain;
static gboolean opt_stage;
static gboolean opt_lock_finalization;
static gboolean opt_retain_pending;
static gboolean opt_retain_rollback;
static gboolean opt_not_as_default;
static gboolean opt_no_prune;
static gboolean opt_no_merge;
static char **opt_kernel_argv;
static char **opt_kernel_argv_append;
static char *opt_kernel_argv_delete;
static gboolean opt_kernel_proc_cmdline;
static char *opt_osname;
static char *opt_origin_path;
static gboolean opt_kernel_arg_none;
static char **opt_overlay_initrds;

static GOptionEntry options[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Use a different operating system root than the current one", "OSNAME" },
  { "origin-file", 0, 0, G_OPTION_ARG_FILENAME, &opt_origin_path, "Specify origin file", "FILENAME" },
  { "no-prune", 0, 0, G_OPTION_ARG_NONE, &opt_no_prune, "Don't prune the repo when done", NULL},
  { "no-merge", 0, 0, G_OPTION_ARG_NONE, &opt_no_merge, "Do not apply configuration (/etc and kernel arguments) from booted deployment", NULL},
  { "retain", 0, 0, G_OPTION_ARG_NONE, &opt_retain, "Do not delete previous deployments", NULL },
  { "stage", 0, 0, G_OPTION_ARG_NONE, &opt_stage, "Complete deployment at OS shutdown", NULL },
  { "lock-finalization", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_lock_finalization, "Prevent automatic deployment finalization on shutdown", NULL },
  { "retain-pending", 0, 0, G_OPTION_ARG_NONE, &opt_retain_pending, "Do not delete pending deployments", NULL },
  { "retain-rollback", 0, 0, G_OPTION_ARG_NONE, &opt_retain_rollback, "Do not delete rollback deployments", NULL },
  { "not-as-default", 0, 0, G_OPTION_ARG_NONE, &opt_not_as_default, "Append rather than prepend new deployment", NULL },
  { "karg-proc-cmdline", 0, 0, G_OPTION_ARG_NONE, &opt_kernel_proc_cmdline, "Import current /proc/cmdline", NULL },
  { "karg", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_argv, "Set kernel argument, like root=/dev/sda1; this overrides any earlier argument with the same name", "NAME=VALUE" },
  { "karg-append", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_argv_append, "Append kernel argument; useful with e.g. console= that can be used multiple times", "NAME=VALUE" },
  { "karg-none", 0, 0, G_OPTION_ARG_NONE, &opt_kernel_arg_none, "Do not import kernel arguments", NULL },
  { "karg-delete", 0, 0, G_OPTION_ARG_STRING, &opt_kernel_argv_delete, "Delete kernel argument if exists", "NAME=VALUE" },
  { "overlay-initrd", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_overlay_initrds, "Overlay iniramfs file", "FILE" },
  { NULL }
};

gboolean
ot_admin_builtin_deploy (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
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

  if (opt_kernel_arg_none && opt_kernel_argv_delete)
    {
      ot_util_usage_error (context, "Can't specify both --karg-none and --karg-delete", error);
      return FALSE;
    }

  if (opt_no_merge && opt_kernel_argv_delete)
    {
      ot_util_usage_error (context, "Can't specify both --no-merge and --karg-delete", error);
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
    opt_no_merge ? NULL : ostree_sysroot_get_merge_deployment (sysroot, opt_osname);

  /* Here we perform cleanup of any leftover data from previous
   * partial failures.  This avoids having to call
   * glnx_shutil_rm_rf_at() at random points throughout the process.
   *
   * TODO: Add /ostree/transaction file, and only do this cleanup if
   * we find it.
   */
  if (!ostree_sysroot_prepare_cleanup (sysroot, cancellable, error))
    return glnx_prefix_error (error, "Performing initial cleanup");

  /* Initial set of kernel arguments; the default is to use the merge
   * deployment, unless --karg-none or --karg-proc-cmdline are specified.
   */
  g_autoptr(OstreeKernelArgs) kargs = NULL;
  if (opt_kernel_arg_none)
    {
      kargs = ostree_kernel_args_new ();
    }
  else if (opt_kernel_proc_cmdline)
    {
      kargs = ostree_kernel_args_new ();
      if (!ostree_kernel_args_append_proc_cmdline (kargs, cancellable, error))
        return FALSE;
    }
  else if (merge_deployment && (opt_kernel_argv || opt_kernel_argv_append || opt_kernel_argv_delete))
    {
      OstreeBootconfigParser *bootconfig = ostree_deployment_get_bootconfig (merge_deployment);
      g_auto(GStrv) previous_args = g_strsplit (ostree_bootconfig_parser_get (bootconfig, "options"), " ", -1);
      kargs = ostree_kernel_args_new ();
      ostree_kernel_args_append_argv (kargs, previous_args);
    }

  /* Now replace/extend the above set.  Note that if no options are specified,
   * we should end up passing NULL as override_kernel_argv for
   * ostree_sysroot_deploy_tree() so we get the defaults.
   */
  if (opt_kernel_argv)
    {
      if (!kargs)
        kargs = ostree_kernel_args_new ();
      ostree_kernel_args_replace_argv (kargs, opt_kernel_argv);
    }

  if (opt_kernel_argv_append)
    {
      if (!kargs)
        kargs = ostree_kernel_args_new ();
      ostree_kernel_args_append_argv (kargs, opt_kernel_argv_append);
    }

  if (opt_kernel_argv_delete)
    {
      if (!ostree_kernel_args_delete (kargs, opt_kernel_argv_delete, error))
        return FALSE;
    }

  g_autoptr(GPtrArray) overlay_initrd_chksums = NULL;
  for (char **it = opt_overlay_initrds; it && *it; it++)
    {
      const char *path = *it;

      glnx_autofd int fd = -1;
      if (!glnx_openat_rdonly (AT_FDCWD, path, TRUE, &fd, error))
        return FALSE;

      g_autofree char *chksum = NULL;
      if (!ostree_sysroot_stage_overlay_initrd (sysroot, fd, &chksum, cancellable, error))
        return FALSE;

      if (!overlay_initrd_chksums)
        overlay_initrd_chksums = g_ptr_array_new_full (g_strv_length (opt_overlay_initrds), g_free);
      g_ptr_array_add (overlay_initrd_chksums, g_steal_pointer (&chksum));
    }

  if (overlay_initrd_chksums)
    g_ptr_array_add (overlay_initrd_chksums, NULL);

  g_auto(GStrv) kargs_strv = kargs ? ostree_kernel_args_to_strv (kargs) : NULL;

  OstreeSysrootDeployTreeOpts opts = {
    .override_kernel_argv = kargs_strv,
    .overlay_initrds = overlay_initrd_chksums ? (char**)overlay_initrd_chksums->pdata : NULL,
  };

  g_autoptr(OstreeDeployment) new_deployment = NULL;
  if (opt_stage)
    {
      if (opt_retain_pending || opt_retain_rollback)
        return glnx_throw (error, "--stage cannot currently be combined with --retain arguments");
      if (opt_not_as_default)
        return glnx_throw (error, "--stage cannot currently be combined with --not-as-default");
      /* touch file *before* we stage to avoid races */
      if (opt_lock_finalization)
        {
          if (!glnx_shutil_mkdir_p_at (AT_FDCWD,
                                       dirname (strdupa (_OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED)),
                                       0755, cancellable, error))
            return FALSE;

          glnx_autofd int fd = open (_OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED,
                                     O_CREAT | O_WRONLY | O_NOCTTY | O_CLOEXEC, 0640);
          if (fd == -1)
            return glnx_throw_errno_prefix (error, "touch(%s)",
                                            _OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED);
        }
      /* use old API if we can to exercise it in CI */
      if (!overlay_initrd_chksums)
        {
          if (!ostree_sysroot_stage_tree (sysroot, opt_osname, revision, origin,
                                          merge_deployment, kargs_strv, &new_deployment,
                                          cancellable, error))
            return FALSE;
        }
      else
        {
          if (!ostree_sysroot_stage_tree_with_options (sysroot, opt_osname, revision,
                                                       origin, merge_deployment, &opts,
                                                       &new_deployment, cancellable, error))
            return FALSE;
        }
      g_assert (new_deployment);
    }
  else
    {
      /* use old API if we can to exercise it in CI */
      if (!overlay_initrd_chksums)
        {
          if (!ostree_sysroot_deploy_tree (sysroot, opt_osname, revision, origin,
                                           merge_deployment, kargs_strv, &new_deployment,
                                           cancellable, error))
            return FALSE;
        }
      else
        {
          if (!ostree_sysroot_deploy_tree_with_options (sysroot, opt_osname, revision,
                                                        origin, merge_deployment, &opts,
                                                        &new_deployment, cancellable,
                                                        error))
            return FALSE;
        }
      g_assert (new_deployment);

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
    }

  /* And finally, cleanup of any leftover data.  In stage mode, we
   * don't do a full cleanup as we didn't touch the bootloader.
   */
  if (opt_no_prune || opt_stage)
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
