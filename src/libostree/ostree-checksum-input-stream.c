/* 
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "ostree-checksum-input-stream.h"

enum {
  PROP_0,
  PROP_CHECKSUM
};

G_DEFINE_TYPE (OstreeChecksumInputStream, ostree_checksum_input_stream, G_TYPE_FILTER_INPUT_STREAM)

struct _OstreeChecksumInputStreamPrivate {
  GChecksum *checksum;
};

static void     ostree_checksum_input_stream_set_property (GObject              *object,
                                                           guint                 prop_id,
                                                           const GValue         *value,
                                                           GParamSpec           *pspec);
static void     ostree_checksum_input_stream_get_property (GObject              *object,
                                                           guint                 prop_id,
                                                           GValue               *value,
                                                           GParamSpec           *pspec);
static gssize   ostree_checksum_input_stream_read         (GInputStream         *stream,
                                                           void                 *buffer,
                                                           gsize                 count,
                                                           GCancellable         *cancellable,
                                                           GError              **error);

static void
ostree_checksum_input_stream_class_init (OstreeChecksumInputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (OstreeChecksumInputStreamPrivate));

  gobject_class->get_property = ostree_checksum_input_stream_get_property;
  gobject_class->set_property = ostree_checksum_input_stream_set_property;

  stream_class->read_fn = ostree_checksum_input_stream_read;

  /*
   * OstreeChecksumInputStream:checksum:
   *
   * The checksum that the stream updates.
   */
  g_object_class_install_property (gobject_class,
				   PROP_CHECKSUM,
				   g_param_spec_pointer ("checksum",
							 "", "",
							 G_PARAM_READWRITE |
							 G_PARAM_CONSTRUCT_ONLY |
							 G_PARAM_STATIC_STRINGS));

}

static void
ostree_checksum_input_stream_set_property (GObject         *object,
					     guint            prop_id,
					     const GValue    *value,
					     GParamSpec      *pspec)
{
  OstreeChecksumInputStream *self;
  
  self = OSTREE_CHECKSUM_INPUT_STREAM (object);

  switch (prop_id)
    {
    case PROP_CHECKSUM:
      self->priv->checksum = g_value_get_pointer (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_checksum_input_stream_get_property (GObject    *object,
					     guint       prop_id,
					     GValue     *value,
					     GParamSpec *pspec)
{
  OstreeChecksumInputStream *self;

  self = OSTREE_CHECKSUM_INPUT_STREAM (object);

  switch (prop_id)
    {
    case PROP_CHECKSUM:
      g_value_set_pointer (value, self->priv->checksum);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ostree_checksum_input_stream_init (OstreeChecksumInputStream *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
					    OSTREE_TYPE_CHECKSUM_INPUT_STREAM,
					    OstreeChecksumInputStreamPrivate);

}

OstreeChecksumInputStream *
ostree_checksum_input_stream_new (GInputStream    *base,
                                  GChecksum       *checksum)
{
  OstreeChecksumInputStream *stream;

  g_return_val_if_fail (G_IS_INPUT_STREAM (base), NULL);

  stream = g_object_new (OSTREE_TYPE_CHECKSUM_INPUT_STREAM,
			 "base-stream", base,
                         "checksum", checksum,
			 NULL);

  return (OstreeChecksumInputStream*) (stream);
}

static gssize
ostree_checksum_input_stream_read (GInputStream  *stream,
                                   void          *buffer,
                                   gsize          count,
                                   GCancellable  *cancellable,
                                   GError       **error)
{
  OstreeChecksumInputStream *self = (OstreeChecksumInputStream*) stream;
  GFilterInputStream *fself = (GFilterInputStream*) self;
  gssize res = -1;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return -1;

  res = g_input_stream_read (fself->base_stream,
                             buffer,
                             count,
                             cancellable,
                             error);
  if (res > 0)
    g_checksum_update (self->priv->checksum, buffer, res);

  return res;
}
