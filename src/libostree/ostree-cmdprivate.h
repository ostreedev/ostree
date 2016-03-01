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

typedef struct {
  gboolean (* ostree_generate_grub2_config) (OstreeSysroot *sysroot, int bootversion, int target_fd, GCancellable *cancellable, GError **error);
  gboolean (* ostree_static_delta_dump) (OstreeRepo *repo, const char *delta_id, GCancellable *cancellable, GError **error);
} OstreeCmdPrivateVTable;

/* Note this not really "public", we just export the symbol, but not the header */
_OSTREE_PUBLIC const OstreeCmdPrivateVTable *
ostree_cmd__private__ (void);

G_END_DECLS
