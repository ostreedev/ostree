/*
 * Copyright (C) 2018 Red Hat, Inc.
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "config.h"

#include <stdlib.h>

#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"
#include "otutil.h"

#include "ostree-cmdprivate.h"
#include "ostree.h"

/* Called by ostree-finalize-staged.service, and in turn
 * invokes a cmdprivate function inside the shared library.
 */
gboolean
ot_admin_builtin_finalize_staged (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  /* Just a sanity check; we shouldn't be called outside of the service though.
   */
  struct stat stbuf;
  if (fstatat (AT_FDCWD, "/run/ostree-booted", &stbuf, 0) < 0)
    return TRUE;

  g_autoptr(GFile) sysroot_file = g_file_new_for_path ("/");
  g_autoptr(OstreeSysroot) sysroot = ostree_sysroot_new (sysroot_file);

  if (!ostree_sysroot_load (sysroot, cancellable, error))
    return FALSE;
  if (!ostree_cmd__private__()->ostree_finalize_staged (sysroot, cancellable, error))
    return FALSE;

  return TRUE;
}
