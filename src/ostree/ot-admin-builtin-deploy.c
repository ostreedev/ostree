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

#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"
#include "otutil.h"

#include <glib/gi18n.h>

static gboolean opt_no_bootloader;
static gboolean opt_retain;
static char **opt_kernel_argv;
static gboolean opt_kernel_proc_cmdline;
static char *opt_osname;
static char *opt_origin_path;

static GOptionEntry options[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Specify operating system root to use", NULL },
  { "origin-file", 0, 0, G_OPTION_ARG_FILENAME, &opt_origin_path, "Specify origin file", NULL },
  { "no-bootloader", 0, 0, G_OPTION_ARG_NONE, &opt_no_bootloader, "Don't update bootloader", NULL },
  { "retain", 0, 0, G_OPTION_ARG_NONE, &opt_retain, "Do not delete previous deployment", NULL },
  { "karg-proc-cmdline", 0, 0, G_OPTION_ARG_NONE, &opt_kernel_proc_cmdline, "Import current /proc/cmdline", NULL },
  { "karg", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kernel_argv, "Set kernel argument, like --karg=root=/dev/sda1", NULL },
  { NULL }
};

gboolean
ot_admin_builtin_deploy (int argc, char **argv, OstreeSysroot *sysroot, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  const char *refspec;
  GOptionContext *context;
  GKeyFile *origin = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_ptrarray GPtrArray *new_deployments = NULL;
  gs_unref_object OstreeDeployment *new_deployment = NULL;
  gs_unref_object OstreeDeployment *merge_deployment = NULL;
  gs_free char *revision = NULL;
  gs_unref_ptrarray GPtrArray *kargs = NULL;

  context = g_option_context_new ("REFSPEC - Checkout revision REFSPEC as the new default deployment");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
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
   * partial failures.  This avoids having to call gs_shutil_rm_rf()
   * at random points throughout the process.
   *
   * TODO: Add /ostree/transaction file, and only do this cleanup if
   * we find it.
   */
  if (!ostree_sysroot_cleanup (sysroot, cancellable, error))
    {
      g_prefix_error (error, "Performing initial cleanup: ");
      goto out;
    }

  kargs = g_ptr_array_new_with_free_func (g_free);

  if (opt_kernel_proc_cmdline)
    {
      gs_unref_object GFile *proc_cmdline_path = g_file_new_for_path ("/proc/cmdline");
      gs_free char *proc_cmdline = NULL;
      gsize proc_cmdline_len = 0;
      gs_strfreev char **proc_cmdline_args = NULL;
      char **strviter;

      if (!g_file_load_contents (proc_cmdline_path, cancellable,
                                 &proc_cmdline, &proc_cmdline_len,
                                 NULL, error))
        goto out;

      proc_cmdline_args = g_strsplit (proc_cmdline, " ", -1);
      for (strviter = proc_cmdline_args; strviter && *strviter; strviter++)
        {
          char *arg = *strviter;
          g_strchomp (arg);
          g_ptr_array_add (kargs, arg);
          *strviter = NULL; /* transfer ownership */
        }
    }

  if (opt_kernel_argv)
    {
      char **strviter;
      for (strviter = opt_kernel_argv; strviter && *strviter; strviter++)
        {
          const char *arg = *strviter;
          char *val = g_strdup (arg);
          g_strchomp (val);
          g_ptr_array_add (kargs, val);
        }
    }

  g_ptr_array_add (kargs, NULL);

  if (!ostree_sysroot_deploy_one_tree (sysroot,
                                       opt_osname, revision, origin,
                                       (char**)kargs->pdata, merge_deployment,
                                       &new_deployment,
                                       cancellable, error))
    goto out;

  if (!ot_admin_complete_deploy_one (sysroot, opt_osname,
                                     new_deployment, merge_deployment, opt_retain,
                                     cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (origin)
    g_key_file_unref (origin);
  if (context)
    g_option_context_free (context);
  return ret;
}
