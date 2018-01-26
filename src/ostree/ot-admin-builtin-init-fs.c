/*
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
