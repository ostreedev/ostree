/*
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
