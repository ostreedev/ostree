/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
