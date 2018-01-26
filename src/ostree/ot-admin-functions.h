/*
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
