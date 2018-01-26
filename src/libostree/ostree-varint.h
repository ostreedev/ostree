/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean _ostree_read_varuint64 (const guint8   *buf,
                                 gsize           buflen,
                                 guint64        *out_value,
                                 gsize          *bytes_read);

void _ostree_write_varuint64 (GString *buf, guint64 n);

G_END_DECLS
