/*
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

#include "ostree-lzma-compressor.h"
#include "ostree-lzma-common.h"

#include <errno.h>
#include <lzma.h>
#include <string.h>

enum {
  PROP_0,
  PROP_PARAMS
};

/**
 * SECTION:ostree-lzma-compressor
 * @title: LZMA compressor
 *
 * An implementation of #GConverter that compresses data using
 * LZMA.
 */

static void _ostree_lzma_compressor_iface_init          (GConverterIface *iface);

/**
 * OstreeLzmaCompressor:
 *
 * Zlib decompression
 */
struct _OstreeLzmaCompressor
{
  GObject parent_instance;

  GVariant *params;
  lzma_stream lstream;
  gboolean initialized;
};

G_DEFINE_TYPE_WITH_CODE (OstreeLzmaCompressor, _ostree_lzma_compressor,
			 G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_CONVERTER,
						_ostree_lzma_compressor_iface_init))

static void
_ostree_lzma_compressor_finalize (GObject *object)
{
  OstreeLzmaCompressor *self = OSTREE_LZMA_COMPRESSOR (object);

  lzma_end (&self->lstream);
  g_clear_pointer (&self->params, (GDestroyNotify)g_variant_unref);

  G_OBJECT_CLASS (_ostree_lzma_compressor_parent_class)->finalize (object);
}

static void
_ostree_lzma_compressor_set_property (GObject      *object,
				      guint         prop_id,
				      const GValue *value,
				      GParamSpec   *pspec)
{
  OstreeLzmaCompressor *self = OSTREE_LZMA_COMPRESSOR (object);

  switch (prop_id)
    {
    case PROP_PARAMS:
      self->params = g_value_get_variant (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }

}

static void
_ostree_lzma_compressor_get_property (GObject    *object,
				      guint       prop_id,
				      GValue     *value,
				      GParamSpec *pspec)
{
  OstreeLzmaCompressor *self = OSTREE_LZMA_COMPRESSOR (object);

  switch (prop_id)
    {
    case PROP_PARAMS:
      g_value_set_variant (value, self->params);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
_ostree_lzma_compressor_init (OstreeLzmaCompressor *self)
{
  lzma_stream tmp = LZMA_STREAM_INIT;
  self->lstream = tmp;
}

static void
_ostree_lzma_compressor_class_init (OstreeLzmaCompressorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = _ostree_lzma_compressor_finalize;
  gobject_class->get_property = _ostree_lzma_compressor_get_property;
  gobject_class->set_property = _ostree_lzma_compressor_set_property;

  g_object_class_install_property (gobject_class,
				   PROP_PARAMS,
				   g_param_spec_variant ("params", "", "",
							 G_VARIANT_TYPE ("a{sv}"),
							 NULL,
							 G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							 G_PARAM_STATIC_STRINGS));
}

OstreeLzmaCompressor *
_ostree_lzma_compressor_new (GVariant *params)
{
  return g_object_new (OSTREE_TYPE_LZMA_COMPRESSOR,
		       "params", params,
		       NULL);
}

static void
_ostree_lzma_compressor_reset (GConverter *converter)
{
  OstreeLzmaCompressor *self = OSTREE_LZMA_COMPRESSOR (converter);

  if (self->initialized)
    {
      lzma_stream tmp = LZMA_STREAM_INIT;
      lzma_end (&self->lstream);
      self->lstream = tmp;
      self->initialized = FALSE;
    }
}

static GConverterResult
_ostree_lzma_compressor_convert (GConverter *converter,
				 const void *inbuf,
				 gsize       inbuf_size,
				 void       *outbuf,
				 gsize       outbuf_size,
				 GConverterFlags flags,
				 gsize      *bytes_read,
				 gsize      *bytes_written,
				 GError    **error)
{
  OstreeLzmaCompressor *self = OSTREE_LZMA_COMPRESSOR (converter);
  int res;
  lzma_action action; 

  if (inbuf_size != 0 && outbuf_size == 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
         "Output buffer too small");
      return G_CONVERTER_ERROR;
    }

  if (!self->initialized)
    {
      res = lzma_easy_encoder (&self->lstream, 8, LZMA_CHECK_CRC64);
      if (res != LZMA_OK)
        goto out;
      self->initialized = TRUE;
    }

  self->lstream.next_in = (void *)inbuf;
  self->lstream.avail_in = inbuf_size;

  self->lstream.next_out = outbuf;
  self->lstream.avail_out = outbuf_size;

  action = LZMA_RUN;
  if (flags & G_CONVERTER_INPUT_AT_END)
    action = LZMA_FINISH;
  else if (flags & G_CONVERTER_FLUSH)
    action = LZMA_SYNC_FLUSH;

  res = lzma_code (&self->lstream, action);
  if (res != LZMA_OK && res != LZMA_STREAM_END)
    goto out;

  *bytes_read = inbuf_size - self->lstream.avail_in;
  *bytes_written = outbuf_size - self->lstream.avail_out;

 out:
  return _ostree_lzma_return (res, error);
}

static void
_ostree_lzma_compressor_iface_init (GConverterIface *iface)
{
  iface->convert = _ostree_lzma_compressor_convert;
  iface->reset = _ostree_lzma_compressor_reset;
}
