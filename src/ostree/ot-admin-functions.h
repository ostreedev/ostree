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

#pragma once

#include <gio/gio.h>
#include <ostree.h>

G_BEGIN_DECLS

gboolean
ot_admin_require_booted_deployment_or_osname (OstreeSysroot       *sysroot,
                                              const char          *osname,
                                              GCancellable        *cancellable,
                                              GError             **error);

char *
ot_admin_checksum_version (GVariant *checksum);

OstreeDeployment *
ot_admin_get_indexed_deployment (OstreeSysroot  *sysroot,
                                 int             index,
                                 GError        **error);

gboolean
ot_admin_sysroot_lock (OstreeSysroot  *sysroot,
                       GError        **error);

gboolean
ot_admin_execve_reboot (OstreeSysroot *sysroot,
                        GError **error);

G_END_DECLS
