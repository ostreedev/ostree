/*
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "otutil.h"

#include <glib/gi18n.h>

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-admin-init-fs.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ot_admin_builtin_init_fs (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("PATH");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER |
                                          OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED |
                                          OSTREE_ADMIN_BUILTIN_FLAG_NO_SYSROOT,
                                          invocation, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    {
      ot_util_usage_error (context, "PATH must be specified", error);
      return FALSE;
    }

  const char *sysroot_path = argv[1];

  glnx_autofd int root_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, sysroot_path, TRUE, &root_dfd, error))
    return FALSE;

  const char *normal_toplevels[] = {"boot", "dev", "home", "proc", "run", "sys"};
  for (guint i = 0; i < G_N_ELEMENTS (normal_toplevels); i++)
    {
      if (!glnx_shutil_mkdir_p_at (root_dfd, normal_toplevels[i], 0755,
                                   cancellable, error))
        return FALSE;
    }

  if (!glnx_shutil_mkdir_p_at (root_dfd, "root", 0700,
                               cancellable, error))
    return FALSE;

  if (!glnx_shutil_mkdir_p_at (root_dfd, "tmp", 01777,
                               cancellable, error))
    return FALSE;
  if (fchmodat (root_dfd, "tmp", 01777, 0) == -1)
    {
      glnx_set_prefix_error_from_errno (error, "chmod: %s", "tmp");
      return FALSE;
    }
  g_autoptr(GFile) dir = g_file_new_for_path (sysroot_path);
  g_autoptr(OstreeSysroot) sysroot = ostree_sysroot_new (dir);
  if (!ostree_sysroot_ensure_initialized (sysroot, cancellable, error))
    return FALSE;

  return TRUE;
}
