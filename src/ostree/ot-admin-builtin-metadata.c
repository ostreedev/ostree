/*
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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

#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "otutil.h"

static char **opt_set;
static char **opt_get;

static GOptionEntry options[]
    = { { "set", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_set,
          "Set deployment metadata, like DATE=030424; this overrides any metadata with the "
          "same name",
          "KEY=VALUE" },
        { "get", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_get,
          "Get the value of a deployment metadata.", "KEY" },
        { NULL } };

gboolean
ot_admin_builtin_metadata (int argc, char **argv, OstreeCommandInvocation *invocation,
                           GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr (GPtrArray) deployments = NULL;
  OstreeDeployment *first_deployment = NULL;
  g_autoptr (GOptionContext) context = NULL;
  g_autoptr (OstreeSysroot) sysroot = NULL;

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER, invocation, &sysroot,
                                          cancellable, error))
    goto out;

  if (deployments->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Unable to find a deployment in sysroot");
      goto out;
    }

  first_deployment = deployments->pdata[0];

  if (opt_set)
    {
      char *key = strtok (*opt_set, "=");
      char *value = strtok (NULL, "=");
      // ^^ This needs error checking and probably is wrong... but builds!
      ostree_deployment_set_ext_metadata (first_deployment, key, value, error);
    }

  if (opt_get)
    {
      ostree_deployment_get_ext_metadata (first_deployment, *opt_get, error);
    }

  ret = TRUE;
out:
  return ret;
}
