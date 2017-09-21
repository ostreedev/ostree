/*
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

/**
 * OstreeSysrootUpgraderFlags:
 * @OSTREE_SYSROOT_UPGRADER_FLAGS_NONE: No options
 * @OSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED: Do not error if the origin has an unconfigured-state key
 *
 * Flags controlling operation of an #OstreeSysrootUpgrader.
 */
typedef enum {
  OSTREE_SYSROOT_UPGRADER_FLAGS_NONE = (1 << 0),
  OSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED = (1 << 1),
} OstreeSysrootUpgraderFlags;

_OSTREE_PUBLIC
GType ostree_sysroot_upgrader_get_type (void);

_OSTREE_PUBLIC
GType ostree_sysroot_upgrader_flags_get_type (void);

_OSTREE_PUBLIC
OstreeSysrootUpgrader *ostree_sysroot_upgrader_new (OstreeSysroot *sysroot,
                                                    GCancellable  *cancellable,
                                                    GError       **error);

_OSTREE_PUBLIC
OstreeSysrootUpgrader *ostree_sysroot_upgrader_new_for_os (OstreeSysroot *sysroot,
                                                           const char    *osname,
                                                           GCancellable  *cancellable,
                                                           GError       **error);

_OSTREE_PUBLIC
OstreeSysrootUpgrader *ostree_sysroot_upgrader_new_for_os_with_flags (OstreeSysroot              *sysroot,
                                                                      const char                 *osname,
                                                                      OstreeSysrootUpgraderFlags  flags,
                                                                      GCancellable               *cancellable,
                                                                      GError                    **error);

_OSTREE_PUBLIC
GKeyFile *ostree_sysroot_upgrader_get_origin (OstreeSysrootUpgrader *self);
_OSTREE_PUBLIC
GKeyFile *ostree_sysroot_upgrader_dup_origin (OstreeSysrootUpgrader *self);
_OSTREE_PUBLIC
gboolean ostree_sysroot_upgrader_set_origin (OstreeSysrootUpgrader *self, GKeyFile *origin,
                                             GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
char *ostree_sysroot_upgrader_get_origin_description (OstreeSysrootUpgrader *self);

_OSTREE_PUBLIC
gboolean ostree_sysroot_upgrader_check_timestamps (OstreeRepo     *repo,
                                                   const char     *from_rev,
                                                   const char     *to_rev,
                                                   GError        **error);

typedef enum {
  OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_NONE = 0,
  OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER = (1 << 0),
  OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_SYNTHETIC = (1 << 1) /* Don't actually do a pull, just check timestamps/changed */
} OstreeSysrootUpgraderPullFlags;

_OSTREE_PUBLIC
gboolean ostree_sysroot_upgrader_pull (OstreeSysrootUpgrader  *self,
                                       OstreeRepoPullFlags     flags,
                                       OstreeSysrootUpgraderPullFlags     upgrader_flags,
                                       OstreeAsyncProgress    *progress,
                                       gboolean               *out_changed,
                                       GCancellable           *cancellable,
                                       GError                **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_upgrader_pull_one_dir (OstreeSysrootUpgrader  *self,
                                      const char                   *dir_to_pull,
                                      OstreeRepoPullFlags     flags,
                                      OstreeSysrootUpgraderPullFlags     upgrader_flags,
                                      OstreeAsyncProgress    *progress,
                                      gboolean               *out_changed,
                                      GCancellable           *cancellable,
                                      GError                **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_upgrader_deploy (OstreeSysrootUpgrader  *self,
                                         GCancellable           *cancellable,
                                         GError                **error);

G_END_DECLS
