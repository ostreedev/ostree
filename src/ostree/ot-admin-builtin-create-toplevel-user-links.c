/*
 * Copyright (C) 2022 Red Hat, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <string.h>
#include <glib-unix.h>

#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ostree-cmd-private.h"

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ot_admin_builtin_create_toplevel_user_links (int argc, char **argv,
                                             OstreeCommandInvocation *invocation,
                                             GCancellable *cancellable,
                                             GError **error)
{
  g_autoptr(OstreeSysroot) sysroot = NULL;
  g_autoptr(GOptionContext) context = g_option_context_new ("");
  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER,
                                          invocation, &sysroot, cancellable, error))
    return glnx_prefix_error (error, "parsing options");

  OstreeDeployment *deployment = ostree_sysroot_get_booted_deployment (sysroot);
  g_autofree char *deployment_path = ostree_sysroot_get_deployment_dirpath (sysroot, deployment);

  glnx_autofd int deployment_dfd = -1;
  if (!glnx_opendirat (ostree_sysroot_get_fd (sysroot), deployment_path, TRUE, &deployment_dfd, error))
    return glnx_prefix_error (error, "open(%s)", deployment_path);

  if (!ostree_sysroot_deployment_set_mutable (sysroot, deployment, TRUE, cancellable, error))
    return glnx_prefix_error (error, "setting deployment mutable");

  if (!ostree_cmd__private__()->ostree_create_toplevel_user_links (sysroot, deployment_dfd, cancellable, error))
    return glnx_prefix_error (error, "creating toplevel user links");

  if (!ostree_sysroot_deployment_set_mutable (sysroot, deployment, FALSE, cancellable, error))
    return glnx_prefix_error (error, "setting deployment immutable");

  return TRUE;
}
