/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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

#pragma once

#include <gio/gio.h>
#include <ostree.h>

G_BEGIN_DECLS

gboolean
ot_admin_require_booted_deployment_or_osname (OstreeSysroot       *sysroot,
                                              const char          *osname,
                                              GCancellable        *cancellable,
                                              GError             **error);

gboolean
ot_admin_deploy_prepare (OstreeSysroot      *sysroot,
                         const char         *osname,
                         OstreeDeployment  **merge_deployment,
                         char              **origin_remote,
                         char              **origin_ref,
                         GKeyFile          **out_origin,
                         GCancellable        *cancellable,
                         GError             **error);

gboolean
ot_admin_complete_deploy_one (OstreeSysroot      *sysroot,
                              const char         *osname,
                              OstreeDeployment   *new_deployment,
                              OstreeDeployment   *merge_deployment,
                              gboolean            opt_retain,
                              GCancellable        *cancellable,
                              GError             **error);

G_END_DECLS

