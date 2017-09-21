/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>.
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
 * Author: Colin Walters <walters@verbum.org>
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

GVariant *ot_gvariant_new_bytearray (const guchar   *data,
                                     gsize           len);

GVariant *ot_gvariant_new_ay_bytes (GBytes *bytes);

GVariant *ot_gvariant_new_empty_string_dict (void);

GHashTable *ot_util_variant_asv_to_hash_table (GVariant *variant);

GVariant * ot_util_variant_take_ref (GVariant *variant);

typedef enum {
  OT_VARIANT_MAP_TRUSTED = (1 << 0),
  OT_VARIANT_MAP_ALLOW_NOENT = (1 << 1)
} OtVariantMapFlags;

gboolean ot_util_variant_map_at (int dfd,
                                 const char *path,
                                 const GVariantType *type,
                                 OtVariantMapFlags flags,
                                 GVariant **out_variant,
                                 GError  **error);

gboolean ot_util_variant_map_fd (int                  fd,
                                 goffset              offset,
                                 const GVariantType  *type,
                                 gboolean             trusted,
                                 GVariant           **out_variant,
                                 GError             **error);

GInputStream *ot_variant_read (GVariant             *variant);

GVariantBuilder *ot_util_variant_builder_from_variant (GVariant            *variant,
                                                       const GVariantType  *type);

gboolean
ot_variant_bsearch_str (GVariant   *array,
                        const char *str,
                        int        *out_pos);

G_END_DECLS
