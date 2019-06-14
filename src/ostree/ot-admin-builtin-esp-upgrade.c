/*
 * Copyright (C) 2019 Red Hat, Inc.
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
 * Author: Javier Martinez Canillas <javierm@redhat.com>
 */

#include "config.h"

#include "ostree-sysroot-private.h"
#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "otutil.h"

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ot_admin_builtin_esp_upgrade (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("");

  g_autoptr(OstreeSysroot) sysroot = NULL;
  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED,
                                          invocation, &sysroot, cancellable, error))
    return FALSE;

  g_autoptr(OstreeRepo) repo = NULL;
  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    return FALSE;

  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);

  if (deployments->len == 0)
    {
      g_print ("No deployments.\n");
      return TRUE;
    }

  OstreeDeployment *deployment = ostree_sysroot_get_booted_deployment (sysroot);

  if (!deployment)
    deployment = ot_admin_get_indexed_deployment (sysroot, 0, error);

  struct stat stbuf;

  if (!glnx_fstatat_allow_noent (sysroot->sysroot_fd, "sys/firmware/efi", &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;

  if (errno == ENOENT)
    {
      g_print ("Not an EFI system.\n");
      return TRUE;
    }

  if (!ot_is_rw_mount ("/boot/efi"))
    {
      if (ot_is_ro_mount ("/boot/efi"))
        g_print ("The ESP can't be updated because /boot/efi is a read-only mountpoint.\n");
      else
        g_print ("Only ESP mounted in /boot/efi is supported.\n");
      return TRUE;
    }

  g_autofree char *deployment_path = ostree_sysroot_get_deployment_dirpath (sysroot, deployment);

  g_autofree char *new_esp_path = g_strdup_printf ("%s/usr/lib/ostree-boot", deployment_path);

  GLNX_AUTO_PREFIX_ERROR ("During copy files to the ESP", error);
  glnx_autofd int old_esp_fd = -1;
  if (!glnx_opendirat (sysroot->sysroot_fd, "boot", TRUE, &old_esp_fd, error))
    return FALSE;

  glnx_autofd int new_esp_fd = -1;
  if (!glnx_opendirat (sysroot->sysroot_fd, new_esp_path, TRUE, &new_esp_fd, error))
    return FALSE;

  /* The ESP filesystem is vfat so don't attempt to copy ownership, mode, and xattrs */
  const OstreeSysrootDebugFlags flags = sysroot->debug_flags | OSTREE_SYSROOT_DEBUG_NO_XATTRS;

  if (!ot_copy_dir_recurse (new_esp_fd, old_esp_fd, "efi", flags , cancellable, error))
     return FALSE;

  return TRUE;
}
