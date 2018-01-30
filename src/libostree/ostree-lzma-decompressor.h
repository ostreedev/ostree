/*
 * Copyright (C) 2014 Colin Walters <walters@redhat.com>
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define OSTREE_TYPE_LZMA_DECOMPRESSOR         (_ostree_lzma_decompressor_get_type ())
#define OSTREE_LZMA_DECOMPRESSOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), OSTREE_TYPE_LZMA_DECOMPRESSOR, OstreeLzmaDecompressor))
#define OSTREE_LZMA_DECOMPRESSOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), OSTREE_TYPE_LZMA_DECOMPRESSOR, OstreeLzmaDecompressorClass))
#define OSTREE_IS_LZMA_DECOMPRESSOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), OSTREE_TYPE_LZMA_DECOMPRESSOR))
#define OSTREE_IS_LZMA_DECOMPRESSOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), OSTREE_TYPE_LZMA_DECOMPRESSOR))
#define OSTREE_LZMA_DECOMPRESSOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), OSTREE_TYPE_LZMA_DECOMPRESSOR, OstreeLzmaDecompressorClass))

typedef struct _OstreeLzmaDecompressorClass   OstreeLzmaDecompressorClass;
typedef struct _OstreeLzmaDecompressor   OstreeLzmaDecompressor;

struct _OstreeLzmaDecompressorClass
{
  GObjectClass parent_class;
};

GLIB_AVAILABLE_IN_ALL
GType              _ostree_lzma_decompressor_get_type (void) G_GNUC_CONST;

GLIB_AVAILABLE_IN_ALL
OstreeLzmaDecompressor *_ostree_lzma_decompressor_new (void);

G_END_DECLS
