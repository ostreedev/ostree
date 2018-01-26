/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

GVariant *ot_gvariant_new_bytearray (const guchar   *data,
                                     gsize           len);

GVariant *ot_gvariant_new_ay_bytes (GBytes *bytes);

GVariant *ot_gvariant_new_empty_string_dict (void);

gboolean ot_variant_read_fd (int                  fd,
                             goffset              offset,
                             const GVariantType  *type,
                             gboolean             trusted,
                             GVariant           **out_variant,
                             GError             **error);

GVariantBuilder *ot_util_variant_builder_from_variant (GVariant            *variant,
                                                       const GVariantType  *type);

gboolean
ot_variant_bsearch_str (GVariant   *array,
                        const char *str,
                        int        *out_pos);

G_END_DECLS
