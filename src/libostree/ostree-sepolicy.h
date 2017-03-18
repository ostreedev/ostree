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

#include "ostree-types.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_SEPOLICY ostree_sepolicy_get_type()
#define OSTREE_SEPOLICY(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSTREE_TYPE_SEPOLICY, OstreeSePolicy))
#define OSTREE_IS_SEPOLICY(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSTREE_TYPE_SEPOLICY))

_OSTREE_PUBLIC
GType ostree_sepolicy_get_type (void);

_OSTREE_PUBLIC
OstreeSePolicy* ostree_sepolicy_new (GFile         *path,
                                     GCancellable  *cancellable,
                                     GError       **error);

_OSTREE_PUBLIC
OstreeSePolicy* ostree_sepolicy_new_at (int            rootfs_dfd,
                                        GCancellable  *cancellable,
                                        GError       **error);


_OSTREE_PUBLIC
GFile * ostree_sepolicy_get_path (OstreeSePolicy  *self);

_OSTREE_PUBLIC
const char *ostree_sepolicy_get_name (OstreeSePolicy *self);

_OSTREE_PUBLIC
const char *ostree_sepolicy_get_csum (OstreeSePolicy *self);

_OSTREE_PUBLIC
gboolean ostree_sepolicy_get_label (OstreeSePolicy    *self,
                                    const char       *relpath,
                                    guint32           unix_mode,
                                    char            **out_label,
                                    GCancellable     *cancellable,
                                    GError          **error);

typedef enum {
  OSTREE_SEPOLICY_RESTORECON_FLAGS_NONE,
  OSTREE_SEPOLICY_RESTORECON_FLAGS_ALLOW_NOLABEL = (1 << 0),
  OSTREE_SEPOLICY_RESTORECON_FLAGS_KEEP_EXISTING = (1 << 1)
} OstreeSePolicyRestoreconFlags;

_OSTREE_PUBLIC
gboolean ostree_sepolicy_restorecon (OstreeSePolicy   *self,
                                     const char       *path,
                                     GFileInfo        *info,
                                     GFile            *target,
                                     OstreeSePolicyRestoreconFlags  flags,
                                     char            **out_new_label,
                                     GCancellable     *cancellable,
                                     GError          **error);

_OSTREE_PUBLIC
gboolean ostree_sepolicy_setfscreatecon (OstreeSePolicy   *self,
                                         const char       *path,
                                         guint32           mode,
                                         GError          **error);

_OSTREE_PUBLIC
void ostree_sepolicy_fscreatecon_cleanup (void **unused);

#define ostree_cleanup_sepolicy_fscreatecon __attribute__ ((cleanup(ostree_sepolicy_fscreatecon_cleanup)))

G_END_DECLS
