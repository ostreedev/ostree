/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
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

#define OSTREE_TYPE_CHECKSUM_INPUT_STREAM         (ostree_checksum_input_stream_get_type ())
#define OSTREE_CHECKSUM_INPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), OSTREE_TYPE_CHECKSUM_INPUT_STREAM, OstreeChecksumInputStream))
#define OSTREE_CHECKSUM_INPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), OSTREE_TYPE_CHECKSUM_INPUT_STREAM, OstreeChecksumInputStreamClass))
#define OSTREE_IS_CHECKSUM_INPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), OSTREE_TYPE_CHECKSUM_INPUT_STREAM))
#define OSTREE_IS_CHECKSUM_INPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), OSTREE_TYPE_CHECKSUM_INPUT_STREAM))
#define OSTREE_CHECKSUM_INPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), OSTREE_TYPE_CHECKSUM_INPUT_STREAM, OstreeChecksumInputStreamClass))

typedef struct _OstreeChecksumInputStream         OstreeChecksumInputStream;
typedef struct _OstreeChecksumInputStreamClass    OstreeChecksumInputStreamClass;
typedef struct _OstreeChecksumInputStreamPrivate  OstreeChecksumInputStreamPrivate;

struct _OstreeChecksumInputStream
{
  GFilterInputStream parent_instance;

  /*< private >*/
  OstreeChecksumInputStreamPrivate *priv;
};

struct _OstreeChecksumInputStreamClass
{
  GFilterInputStreamClass parent_class;

  /*< private >*/
  /* Padding for future expansion */
  void (*_g_reserved1) (void);
  void (*_g_reserved2) (void);
  void (*_g_reserved3) (void);
  void (*_g_reserved4) (void);
  void (*_g_reserved5) (void);
};

_OSTREE_PUBLIC
GType          ostree_checksum_input_stream_get_type     (void) G_GNUC_CONST;

_OSTREE_PUBLIC
OstreeChecksumInputStream * ostree_checksum_input_stream_new          (GInputStream   *stream,
                                                                       GChecksum      *checksum);

G_END_DECLS
