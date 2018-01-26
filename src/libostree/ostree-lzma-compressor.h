/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
