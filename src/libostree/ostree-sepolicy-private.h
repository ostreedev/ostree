/*
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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
 */

#pragma once

#include "ostree-types.h"

G_BEGIN_DECLS

typedef struct {
  gboolean initialized;
} OstreeSepolicyFsCreatecon;

void _ostree_sepolicy_fscreatecon_clear (OstreeSepolicyFsCreatecon *con);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(OstreeSepolicyFsCreatecon, _ostree_sepolicy_fscreatecon_clear)

gboolean _ostree_sepolicy_preparefscreatecon (OstreeSepolicyFsCreatecon *con,
                                              OstreeSePolicy   *self,
                                              const char       *path,
                                              guint32           mode,
                                              GError          **error);

GVariant *_ostree_filter_selinux_xattr (GVariant *xattrs);

G_END_DECLS
