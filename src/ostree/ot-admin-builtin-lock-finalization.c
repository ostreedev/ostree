/*
 * Copyright (C) 2023 Red Hat, Inc.
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

#include "ostree-sysroot-private.h"

#include "ostree.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "otutil.h"

#include <glib/gi18n.h>

static gboolean opt_unlock;

static GOptionEntry options[]
    = { { "unlock", 'u', 0, G_OPTION_ARG_NONE, &opt_unlock, "Unlock finalization", NULL },
        { NULL } };

gboolean
ot_admin_builtin_lock_finalization (int argc, char **argv, OstreeCommandInvocation *invocation,
                                    GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("");

  g_autoptr (OstreeSysroot) sysroot = NULL;
  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER, invocation, &sysroot,
                                          cancellable, error))
    return FALSE;

  OstreeDeployment *staged = ostree_sysroot_get_staged_deployment (sysroot);
  if (!staged)
    return glnx_throw (error, "No staged deployment");

  const gboolean is_locked = ostree_deployment_is_finalization_locked (staged);
  if (opt_unlock && !is_locked)
    {
      g_print ("Staged deployment is already prepared for finalization\n");
      return TRUE;
    }
  else if (!opt_unlock && is_locked)
    {
      g_print ("Staged deployment is already finalization locked\n");
      return TRUE;
    }

  if (!ostree_sysroot_change_finalization (sysroot, staged, error))
    return FALSE;

  if (opt_unlock)
    {
      g_print ("Staged deployment is now queued to apply on shutdown\n");
    }
  else
    {
      g_print ("Staged deployment is now finalization locked\n");
    }
  return TRUE;
}
