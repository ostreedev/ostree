/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <string.h>
#include <glib-unix.h>

#include "ot-main.h"
#include "ot-admin-instutil-builtins.h"

#include "otutil.h"

#include "../libostree/ostree-kernel-args.h"

static gboolean opt_proc_cmdline;
static gboolean opt_merge;
static char **opt_replace;
static char **opt_append;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-admin-instutil.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "import-proc-cmdline", 0, 0, G_OPTION_ARG_NONE, &opt_proc_cmdline, "Import current /proc/cmdline", NULL },
  { "merge", 0, 0, G_OPTION_ARG_NONE, &opt_merge, "Merge with previous command line", NULL },
  { "replace", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_replace, "Set kernel argument, like root=/dev/sda1; this overrides any earlier argument with the same name", "NAME=VALUE" },
  { "append", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_append, "Append kernel argument; useful with e.g. console= that can be used multiple times", "NAME=VALUE" },
  { NULL }
};

gboolean
ot_admin_instutil_builtin_set_kargs (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  guint i;
  g_autoptr(GPtrArray) deployments = NULL;
  OstreeDeployment *first_deployment = NULL;
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeSysroot) sysroot = NULL;
  __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *kargs = NULL;

  context = g_option_context_new ("ARGS - set new kernel command line arguments");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER | OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED,
                                          &sysroot, cancellable, error))
    goto out;

  deployments = ostree_sysroot_get_deployments (sysroot);
  if (deployments->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to find a deployment in sysroot");
      goto out;
    }
  first_deployment = deployments->pdata[0];

  kargs = _ostree_kernel_args_new ();

  /* If they want the current kernel's args, they very likely don't
   * want the ones from the merge.
   */
  if (opt_proc_cmdline)
    {
      if (!_ostree_kernel_args_append_proc_cmdline (kargs, cancellable, error))
        goto out;
    }
  else if (opt_merge)
    {
      OstreeBootconfigParser *bootconfig = ostree_deployment_get_bootconfig (first_deployment);
      g_auto(GStrv) previous_args = g_strsplit (ostree_bootconfig_parser_get (bootconfig, "options"), " ", -1);

      _ostree_kernel_args_append_argv (kargs, previous_args);
    }

  if (opt_replace)
    {
      _ostree_kernel_args_replace_argv (kargs, opt_replace);
    }

  if (opt_append)
    {
      _ostree_kernel_args_append_argv (kargs, opt_append);
    }

  for (i = 1; i < argc; i++)
    _ostree_kernel_args_append (kargs, argv[i]);

  {
    g_auto(GStrv) kargs_strv = _ostree_kernel_args_to_strv (kargs);

    if (!ostree_sysroot_deployment_set_kargs (sysroot, first_deployment,
                                              kargs_strv,
                                              cancellable, error))
      goto out;
  }

  ret = TRUE;
 out:
  return ret;
}
