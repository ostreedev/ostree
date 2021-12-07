/*
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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

#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"
#include "otutil.h"

#include <glib/gi18n.h>
#include <err.h>

static gboolean opt_hotfix;
static gboolean opt_transient;

static GOptionEntry options[] = {
  { "hotfix", 0, 0, G_OPTION_ARG_NONE, &opt_hotfix, "Retain changes across reboots", NULL },
  { "transient", 0, 0, G_OPTION_ARG_NONE, &opt_transient, "Mount overlayfs read-only by default", NULL },
  { NULL }
};

gboolean
ot_admin_builtin_unlock (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeSysroot) sysroot = NULL;
  OstreeDeployment *booted_deployment = NULL;
  OstreeDeploymentUnlockedState target_state;

  context = g_option_context_new ("");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER,
                                          invocation, &sysroot, cancellable, error))
    goto out;
  
  if (argc > 1)
    {
      ot_util_usage_error (context, "This command takes no extra arguments", error);
      goto out;
    }

  booted_deployment = ostree_sysroot_require_booted_deployment (sysroot, error);
  if (!booted_deployment)
    goto out;

  if (opt_hotfix && opt_transient)
    {
      glnx_throw (error, "Cannot specify both --hotfix and --transient");
      goto out;
    }
  else if (opt_hotfix)
    target_state = OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX;
  else if (opt_transient)
    target_state = OSTREE_DEPLOYMENT_UNLOCKED_TRANSIENT;
  else
    target_state = OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT;

  if (!ostree_sysroot_deployment_unlock (sysroot, booted_deployment,
                                         target_state, cancellable, error))
    goto out;
  
  switch (target_state)
    {
    case OSTREE_DEPLOYMENT_UNLOCKED_NONE:
      g_assert_not_reached ();
      break;
    case OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX:
      g_print ("Hotfix mode enabled.  A writable overlayfs is now mounted on /usr\n"
               "for this booted deployment.  A non-hotfixed clone has been created\n"
               "as the non-default rollback target.\n");
      break;
    case OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT:
      g_print ("Development mode enabled.  A writable overlayfs is now mounted on /usr.\n"
               "All changes there will be discarded on reboot.\n");
      break;
    case OSTREE_DEPLOYMENT_UNLOCKED_TRANSIENT:
      g_print ("A writable overlayfs is prepared for /usr, but is mounted read-only by default.\n"
               "All changes there will be discarded on reboot.\n");
      break;
    }

  ret = TRUE;
 out:
  return ret;
}
