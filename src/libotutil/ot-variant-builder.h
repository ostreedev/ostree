/*
 * Copyright (C) 2017 Alexander Larsson <alexl@redhat.com>.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Author: Alexander Larsson <alexl@redhat.com>.
 */

#pragma once

#include <gio/gio.h>

#include "libglnx.h"

G_BEGIN_DECLS

typedef struct _OtVariantBuilder OtVariantBuilder;

OtVariantBuilder *ot_variant_builder_new         (const GVariantType  *type,
                                                  int                  fd);
void              ot_variant_builder_unref       (OtVariantBuilder    *builder);
OtVariantBuilder *ot_variant_builder_ref         (OtVariantBuilder    *builder);
gboolean          ot_variant_builder_end         (OtVariantBuilder    *builder,
                                                  GError             **error);
gboolean          ot_variant_builder_open        (OtVariantBuilder    *builder,
                                                  const GVariantType  *type,
                                                  GError             **error);
gboolean          ot_variant_builder_close       (OtVariantBuilder    *builder,
                                                  GError             **error);
gboolean          ot_variant_builder_add_from_fd (OtVariantBuilder    *builder,
                                                  const GVariantType  *type,
                                                  int                  fd,
                                                  guint64              size,
                                                  GError             **error);
gboolean          ot_variant_builder_add_value   (OtVariantBuilder    *builder,
                                                  GVariant            *value,
                                                  GError             **error);
gboolean          ot_variant_builder_add         (OtVariantBuilder    *builder,
                                                  GError             **error,
                                                  const gchar         *format_string,
                                                  ...);
void              ot_variant_builder_add_parsed  (OtVariantBuilder    *builder,
                                                  const gchar         *format,
                                                  ...);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OtVariantBuilder, ot_variant_builder_unref)

G_END_DECLS
