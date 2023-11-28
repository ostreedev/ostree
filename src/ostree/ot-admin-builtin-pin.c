/*
 * Copyright (C) 2018 Colin Walters <walters@verbum.org>
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

static gboolean opt_unpin;

static GOptionEntry options[]
    = { { "unpin", 'u', 0, G_OPTION_ARG_NONE, &opt_unpin, "Unset pin", NULL }, { NULL } };

gboolean
ot_admin_builtin_pin (int argc, char **argv, OstreeCommandInvocation *invocation,
                      GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("INDEX");
  g_autoptr (OstreeSysroot) sysroot = NULL;
  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER, invocation, &sysroot,
                                          cancellable, error))
    return FALSE;

  if (argc < 2)
    {
      ot_util_usage_error (context, "INDEX must be specified", error);
      return FALSE;
    }

  for (unsigned int i = 1; i < argc; i++)
    {
      const char *deploy_index_str = argv[i];
      char *endptr = NULL;

      errno = 0;
      const guint64 deploy_index = g_ascii_strtoull (deploy_index_str, &endptr, 10);
      if (*endptr != '\0')
        return glnx_throw (error, "Invalid index: %s", deploy_index_str);
      if (errno == ERANGE)
        return glnx_throw (error, "Index too large: %s", deploy_index_str);

      g_autoptr (OstreeDeployment) target_deployment
          = ot_admin_get_indexed_deployment (sysroot, deploy_index, error);
      if (!target_deployment)
        return FALSE;

      gboolean current_pin = ostree_deployment_is_pinned (target_deployment);
      const gboolean desired_pin = !opt_unpin;
      if (current_pin == desired_pin)
        {
          g_print ("Deployment %s is already %s\n", deploy_index_str,
                   current_pin ? "pinned" : "unpinned");
        }
      else
        {
          if (!ostree_sysroot_deployment_set_pinned (sysroot, target_deployment, desired_pin,
                                                     error))
            return FALSE;
          g_print ("Deployment %s is now %s\n", deploy_index_str,
                   desired_pin ? "pinned" : "unpinned");
        }
    }

  return TRUE;
}
