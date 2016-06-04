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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ostree-lzma-decompressor.h"
#include "ostree-lzma-common.h"

#include <errno.h>
#include <lzma.h>
#include <string.h>

enum {
  PROP_0,
};

/**
 * SECTION:ostree-lzma-decompressor
 * @title: LZMA decompressor
 *
 * An implementation of #GConverter that decompresses data using
 * LZMA.
 */

static void _ostree_lzma_decompressor_iface_init          (GConverterIface *iface);

struct _OstreeLzmaDecompressor
{
  GObject parent_instance;

  lzma_stream lstream;
  gboolean initialized;
};

G_DEFINE_TYPE_WITH_CODE (OstreeLzmaDecompressor, _ostree_lzma_decompressor,
			 G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_CONVERTER,
						_ostree_lzma_decompressor_iface_init))

static void
_ostree_lzma_decompressor_finalize (GObject *object)
{
  OstreeLzmaDecompressor *self;

  self = OSTREE_LZMA_DECOMPRESSOR (object);
  lzma_end (&self->lstream);

  G_OBJECT_CLASS (_ostree_lzma_decompressor_parent_class)->finalize (object);
}

static void
_ostree_lzma_decompressor_init (OstreeLzmaDecompressor *self)
{
  lzma_stream tmp = LZMA_STREAM_INIT;
  self->lstream = tmp;
}

static void
_ostree_lzma_decompressor_class_init (OstreeLzmaDecompressorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = _ostree_lzma_decompressor_finalize;
}

OstreeLzmaDecompressor *
_ostree_lzma_decompressor_new (void)
{
  return g_object_new (OSTREE_TYPE_LZMA_DECOMPRESSOR, NULL);
}

static void
_ostree_lzma_decompressor_reset (GConverter *converter)
{
  OstreeLzmaDecompressor *self = OSTREE_LZMA_DECOMPRESSOR (converter);

  if (self->initialized)
    {
      lzma_stream tmp = LZMA_STREAM_INIT;
      lzma_end (&self->lstream);
      self->lstream = tmp;
      self->initialized = FALSE;
    }
}

static GConverterResult
_ostree_lzma_decompressor_convert (GConverter *converter,
                                   const void *inbuf,
                                   gsize       inbuf_size,
                                   void       *outbuf,
                                   gsize       outbuf_size,
                                   GConverterFlags flags,
                                   gsize      *bytes_read,
                                   gsize      *bytes_written,
                                   GError    **error)
{
  OstreeLzmaDecompressor *self = OSTREE_LZMA_DECOMPRESSOR (converter);
  int res;

  if (inbuf_size != 0 && outbuf_size == 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
         "Output buffer too small");
      return G_CONVERTER_ERROR;
    }

  if (!self->initialized)
    {
      res = lzma_stream_decoder (&self->lstream, G_MAXUINT64, 0);
      if (res != LZMA_OK)
        goto out;
      self->initialized = TRUE;
    }

  self->lstream.next_in = (void *)inbuf;
  self->lstream.avail_in = inbuf_size;

  self->lstream.next_out = outbuf;
  self->lstream.avail_out = outbuf_size;

  res = lzma_code (&self->lstream, LZMA_RUN);
  if (res != LZMA_OK && res != LZMA_STREAM_END)
    goto out;

  *bytes_read = inbuf_size - self->lstream.avail_in;
  *bytes_written = outbuf_size - self->lstream.avail_out;

 out:
  return _ostree_lzma_return (res, error);
}

static void
_ostree_lzma_decompressor_iface_init (GConverterIface *iface)
{
  iface->convert = _ostree_lzma_decompressor_convert;
  iface->reset = _ostree_lzma_decompressor_reset;
}
