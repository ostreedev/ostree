/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "ostree-types.h"

G_BEGIN_DECLS

gboolean _ostree_impl_system_generator (const char *normal_dir, const char *early_dir,
                                        const char *late_dir, GError **error);

typedef struct
{
  gboolean (*ostree_system_generator) (const char *normal_dir, const char *early_dir,
                                       const char *late_dir, GError **error);
  gboolean (*ostree_generate_grub2_config) (OstreeSysroot *sysroot, int bootversion, int target_fd,
                                            GCancellable *cancellable, GError **error);
  gboolean (*ostree_static_delta_dump) (OstreeRepo *repo, const char *delta_id,
                                        GCancellable *cancellable, GError **error);
  gboolean (*ostree_static_delta_query_exists) (OstreeRepo *repo, const char *delta_id,
                                                gboolean *out_exists, GCancellable *cancellable,
                                                GError **error);
  gboolean (*ostree_static_delta_delete) (OstreeRepo *repo, const char *delta_id,
                                          GCancellable *cancellable, GError **error);
  gboolean (*ostree_repo_verify_bindings) (const char *collection_id, const char *ref_name,
                                           GVariant *commit, GError **error);
  gboolean (*ostree_finalize_staged) (OstreeSysroot *sysroot, GCancellable *cancellable,
                                      GError **error);
  gboolean (*ostree_boot_complete) (OstreeSysroot *sysroot, GCancellable *cancellable,
                                    GError **error);
} OstreeCmdPrivateVTable;

/* Note this not really "public", we just export the symbol, but not the header */
_OSTREE_PUBLIC const OstreeCmdPrivateVTable *ostree_cmd__private__ (void);

G_END_DECLS
