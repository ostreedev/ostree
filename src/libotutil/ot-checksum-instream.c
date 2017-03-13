/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 * 
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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

#include "ot-checksum-instream.h"

G_DEFINE_TYPE (OtChecksumInstream, ot_checksum_instream, G_TYPE_FILTER_INPUT_STREAM)

struct _OtChecksumInstreamPrivate {
  GChecksum *checksum;
};

static gssize   ot_checksum_instream_read         (GInputStream         *stream,
                                                           void                 *buffer,
                                                           gsize                 count,
                                                           GCancellable         *cancellable,
                                                           GError              **error);

static void
ot_checksum_instream_finalize (GObject *object)
{
  OtChecksumInstream *self = (OtChecksumInstream*)object;

  g_checksum_free (self->priv->checksum);

  G_OBJECT_CLASS (ot_checksum_instream_parent_class)->finalize (object);
}

static void
ot_checksum_instream_class_init (OtChecksumInstreamClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);

  g_type_class_add_private (klass, sizeof (OtChecksumInstreamPrivate));

  object_class->finalize = ot_checksum_instream_finalize;
  stream_class->read_fn = ot_checksum_instream_read;
}

static void
ot_checksum_instream_init (OtChecksumInstream *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, OT_TYPE_CHECKSUM_INSTREAM, OtChecksumInstreamPrivate);

}

OtChecksumInstream *
ot_checksum_instream_new (GInputStream    *base,
                          GChecksumType    checksum_type)
{
  OtChecksumInstream *stream;

  g_return_val_if_fail (G_IS_INPUT_STREAM (base), NULL);

  stream = g_object_new (OT_TYPE_CHECKSUM_INSTREAM,
                         "base-stream", base,
                         NULL);
  stream->priv->checksum = g_checksum_new (checksum_type);

  return (OtChecksumInstream*) (stream);
}

static gssize
ot_checksum_instream_read (GInputStream  *stream,
                           void          *buffer,
                           gsize          count,
                           GCancellable  *cancellable,
                           GError       **error)
{
  OtChecksumInstream *self = (OtChecksumInstream*) stream;
  GFilterInputStream *fself = (GFilterInputStream*) self;
  gssize res = -1;

  res = g_input_stream_read (fself->base_stream,
                             buffer,
                             count,
                             cancellable,
                             error);
  if (res > 0)
    g_checksum_update (self->priv->checksum, buffer, res);

  return res;
}

void
ot_checksum_instream_get_digest (OtChecksumInstream *stream,
                                 guint8          *buffer,
                                 gsize           *digest_len)
{
  g_checksum_get_digest (stream->priv->checksum, buffer, digest_len);
}

guint8*
ot_checksum_instream_dup_digest (OtChecksumInstream *stream,
                                 gsize              *ret_len)
{
  gsize len = 32;
  guchar *ret = g_malloc (len);
  g_checksum_get_digest (stream->priv->checksum, ret, &len);
  g_assert (len == 32);
  if (ret_len)
    *ret_len = len;
  return ret;
}

char *
ot_checksum_instream_get_string (OtChecksumInstream *stream)
{
  return g_strdup (g_checksum_get_string (stream->priv->checksum));
}
