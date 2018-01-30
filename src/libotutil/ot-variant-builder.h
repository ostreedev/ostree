/*
 * Copyright (C) 2017 Alexander Larsson <alexl@redhat.com>.
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
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
