/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 *
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define OT_TYPE_CHECKSUM_INSTREAM         (ot_checksum_instream_get_type ())
#define OT_CHECKSUM_INSTREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), OT_TYPE_CHECKSUM_INPUT_STREAM, OtChecksumInstream))
#define OT_CHECKSUM_INSTREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), OT_TYPE_CHECKSUM_INPUT_STREAM, OtChecksumInstreamClass))
#define OT_IS_CHECKSUM_INSTREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), OT_TYPE_CHECKSUM_INPUT_STREAM))
#define OT_IS_CHECKSUM_INSTREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), OT_TYPE_CHECKSUM_INPUT_STREAM))
#define OT_CHECKSUM_INSTREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), OT_TYPE_CHECKSUM_INPUT_STREAM, OtChecksumInstreamClass))

typedef struct _OtChecksumInstream         OtChecksumInstream;
typedef struct _OtChecksumInstreamClass    OtChecksumInstreamClass;
typedef struct _OtChecksumInstreamPrivate  OtChecksumInstreamPrivate;

struct _OtChecksumInstream
{
  GFilterInputStream parent_instance;

  /*< private >*/
  OtChecksumInstreamPrivate *priv;
};

struct _OtChecksumInstreamClass
{
  GFilterInputStreamClass parent_class;
};

GType          ot_checksum_instream_get_type     (void) G_GNUC_CONST;

OtChecksumInstream * ot_checksum_instream_new          (GInputStream   *stream, GChecksumType   checksum);

void   ot_checksum_instream_get_digest (OtChecksumInstream *stream,
                                        guint8          *buffer,
                                        gsize           *digest_len);

guint8* ot_checksum_instream_dup_digest (OtChecksumInstream *stream,
                                         gsize              *ret_len);
char * ot_checksum_instream_get_string (OtChecksumInstream *stream);

G_END_DECLS
