/*
 * Copyright (C) 2018 Red Hat, Inc.
 * Copyright (C) 2022 Endless OS Foundation LLC
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

#include <glib-unix.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>

#include "ostree.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "otutil.h"

#include "ostree-cmd-private.h"
#include "ostree.h"

static gboolean opt_hold;

static GOptionEntry options[]
    = { { "hold", 0, 0, G_OPTION_ARG_NONE, &opt_hold, "Hold /boot open during finalization", NULL },
        { NULL } };

/* Called by ostree-finalize-staged.service, and in turn
 * invokes a cmdprivate function inside the shared library.
 */
gboolean
ot_admin_builtin_finalize_staged (int argc, char **argv, OstreeCommandInvocation *invocation,
                                  GCancellable *cancellable, GError **error)
{
  /* Just a sanity check; we shouldn't be called outside of the service though.
   */
  struct stat stbuf;
  if (fstatat (AT_FDCWD, OSTREE_PATH_BOOTED, &stbuf, 0) < 0)
    return TRUE;

  g_autoptr (GOptionContext) context = g_option_context_new ("");
  g_autoptr (OstreeSysroot) sysroot = NULL;

  /* First parse the args without loading the sysroot to see what options are
   * set. */
  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_NO_LOAD, invocation, &sysroot,
                                          cancellable, error))
    return FALSE;

  if (opt_hold)
    {
      /* Load the sysroot unlocked so that a separate namespace isn't
       * created. */
      if (!ostree_admin_sysroot_load (
              sysroot, OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER | OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED,
              cancellable, error))
        return FALSE;

      /* In case it's an automount, open /boot so that the automount doesn't
       * expire until before this process exits. If it did expire and got
       * unmounted, the service would be stopped and the deployment would be
       * finalized earlier than expected.
       */
      int sysroot_fd = ostree_sysroot_get_fd (sysroot);
      glnx_autofd int boot_fd = -1;
      g_debug ("Opening /boot directory");
      if (!glnx_opendirat (sysroot_fd, "boot", TRUE, &boot_fd, error))
        return FALSE;

      /* We want to keep /boot open until the deployment is finalized during
       * system shutdown, so block until we get SIGTERM which systemd will send
       * when the unit is stopped.
       */
      pause ();
    }
  else
    {
      /* Load the sysroot with the normal flags and actually finalize the
       * deployment. */
      if (!ostree_admin_sysroot_load (sysroot, OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER, cancellable,
                                      error))
        return FALSE;

      if (!ostree_cmd__private__ ()->ostree_finalize_staged (sysroot, cancellable, error))
        return FALSE;
    }

  return TRUE;
}
