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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define OSTREE_TYPE_LZMA_COMPRESSOR         (_ostree_lzma_compressor_get_type ())
#define OSTREE_LZMA_COMPRESSOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), OSTREE_TYPE_LZMA_COMPRESSOR, OstreeLzmaCompressor))
#define OSTREE_LZMA_COMPRESSOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), OSTREE_TYPE_LZMA_COMPRESSOR, OstreeLzmaCompressorClass))
#define OSTREE_IS_LZMA_COMPRESSOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), OSTREE_TYPE_LZMA_COMPRESSOR))
#define OSTREE_IS_LZMA_COMPRESSOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), OSTREE_TYPE_LZMA_COMPRESSOR))
#define OSTREE_LZMA_COMPRESSOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), OSTREE_TYPE_LZMA_COMPRESSOR, OstreeLzmaCompressorClass))

typedef struct _OstreeLzmaCompressorClass   OstreeLzmaCompressorClass;
typedef struct _OstreeLzmaCompressor        OstreeLzmaCompressor;

struct _OstreeLzmaCompressorClass
{
  GObjectClass parent_class;
};

GType            _ostree_lzma_compressor_get_type (void) G_GNUC_CONST;

OstreeLzmaCompressor *_ostree_lzma_compressor_new (GVariant *params);

G_END_DECLS
