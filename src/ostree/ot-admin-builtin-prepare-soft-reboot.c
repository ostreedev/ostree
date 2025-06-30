/*
 * Copyright (C) 2025 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include <stdlib.h>

#include "ostree.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "otutil.h"

static gboolean opt_reboot;
static gboolean opt_reset;

static GOptionEntry options[]
    = { { "reboot", 0, 0, G_OPTION_ARG_NONE, &opt_reboot, "Initiate a soft reboot on success",
          NULL },
        { "reset", 0, 0, G_OPTION_ARG_NONE, &opt_reset, "Undo queued soft reboot state", NULL },
        { NULL } };

gboolean
ot_admin_builtin_prepare_soft_reboot (int argc, char **argv, OstreeCommandInvocation *invocation,
                                      GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("INDEX");

  g_autoptr (OstreeSysroot) sysroot = NULL;
  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER, invocation, &sysroot,
                                          cancellable, error))
    return FALSE;

  if (opt_reset)
    return ostree_sysroot_clear_soft_reboot (sysroot, cancellable, error);

  if (argc < 2)
    {
      ot_util_usage_error (context, "INDEX must be specified", error);
      return FALSE;
    }

  const char *deploy_index_str = argv[1];
  guint deploy_index;
  {
    char *endptr = NULL;
    errno = 0;
    deploy_index = (guint)g_ascii_strtoull (deploy_index_str, &endptr, 10);
    if (*endptr != '\0')
      return glnx_throw (error, "Invalid index: %s", deploy_index_str);
  }

  g_autoptr (OstreeDeployment) target_deployment
      = ot_admin_get_indexed_deployment (sysroot, deploy_index, error);
  if (!target_deployment)
    return FALSE;

  if (target_deployment == ostree_sysroot_get_booted_deployment (sysroot))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Cannot prepare for soft-reboot currently booted deployment %i", deploy_index);
      return FALSE;
    }

  if (!ostree_sysroot_deployment_set_soft_reboot (sysroot, target_deployment, FALSE, cancellable,
                                                  error))
    return FALSE;

  if (opt_reboot)
    {
      execlp ("systemctl", "systemctl", "soft-reboot", NULL);
      return glnx_throw_errno_prefix (error, "exec(systemctl soft-reboot)");
    }

  return TRUE;
}
