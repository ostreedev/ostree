/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 * Copyright (C) 2022 Huijing Hei <hhei@redhat.com>
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
 * Public License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ot-admin-kargs-builtins.h"
#include "otutil.h"

static char **opt_kargs_edit_in_place_append;

static GOptionEntry options[]
    = { { "append-if-missing", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_kargs_edit_in_place_append,
          "Append kernel arguments if they do not exist", "NAME=VALUE" },
        { NULL } };

gboolean
ot_admin_kargs_builtin_edit_in_place (int argc, char **argv, OstreeCommandInvocation *invocation,
                                      GCancellable *cancellable, GError **error)
{
  g_autoptr (OstreeSysroot) sysroot = NULL;

  g_autoptr (GOptionContext) context = g_option_context_new ("ARGS");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER, invocation, &sysroot,
                                          cancellable, error))
    return FALSE;

  g_autoptr (GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  if (deployments->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Unable to find a deployment in sysroot");
      return FALSE;
    }

  // set kargs for each deployment
  for (guint i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      OstreeBootconfigParser *bootconfig = ostree_deployment_get_bootconfig (deployment);
      g_autoptr (OstreeKernelArgs) kargs
          = ostree_kernel_args_from_string (ostree_bootconfig_parser_get (bootconfig, "options"));

      if (opt_kargs_edit_in_place_append)
        {
          for (char **strviter = opt_kargs_edit_in_place_append; strviter && *strviter; strviter++)
            {
              const char *arg = *strviter;
              ostree_kernel_args_append_if_missing (kargs, arg);
            }
        }

      g_autofree char *new_options = ostree_kernel_args_to_string (kargs);

      if (!ostree_sysroot_deployment_set_kargs_in_place (sysroot, deployment, new_options,
                                                         cancellable, error))
        return FALSE;
    }

  return TRUE;
}
