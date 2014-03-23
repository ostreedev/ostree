/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#pragma once

#include "ostree-sysroot.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_SYSROOT_UPGRADER ostree_sysroot_upgrader_get_type()
#define OSTREE_SYSROOT_UPGRADER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSTREE_TYPE_SYSROOT_UPGRADER, OstreeSysrootUpgrader))
#define OSTREE_IS_SYSROOT_UPGRADER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSTREE_TYPE_SYSROOT_UPGRADER))

GType ostree_sysroot_upgrader_get_type (void);

OstreeSysrootUpgrader *ostree_sysroot_upgrader_new (OstreeSysroot *sysroot,
                                                    GCancellable  *cancellable,
                                                    GError       **error);

OstreeSysrootUpgrader *ostree_sysroot_upgrader_new_for_os (OstreeSysroot *sysroot,
                                                           const char    *osname,
                                                           GCancellable  *cancellable,
                                                           GError       **error);

GKeyFile *ostree_sysroot_upgrader_get_origin (OstreeSysrootUpgrader *self);
gboolean ostree_sysroot_upgrader_set_origin (OstreeSysrootUpgrader *self, GKeyFile *origin,
                                             GCancellable *cancellable, GError **error);

gboolean ostree_sysroot_upgrader_check_timestamps (OstreeRepo     *repo,
                                                   const char     *from_rev,
                                                   const char     *to_rev,
                                                   GError        **error);

typedef enum {
  OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_NONE = 0,
  OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER = (1 << 0)
} OstreeSysrootUpgraderPullFlags;

gboolean ostree_sysroot_upgrader_pull (OstreeSysrootUpgrader  *self,
                                       OstreeRepoPullFlags     flags,
                                       OstreeSysrootUpgraderPullFlags     upgrader_flags,
                                       OstreeAsyncProgress    *progress,
                                       gboolean               *out_changed,
                                       GCancellable           *cancellable,
                                       GError                **error);

gboolean ostree_sysroot_upgrader_deploy (OstreeSysrootUpgrader  *self,
                                         GCancellable           *cancellable,
                                         GError                **error);

G_END_DECLS

