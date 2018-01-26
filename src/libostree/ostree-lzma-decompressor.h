/*
 * Copyright (C) 2014 Colin Walters <walters@redhat.com>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
