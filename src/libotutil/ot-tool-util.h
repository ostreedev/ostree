/*
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean ot_parse_boolean (const char *value, gboolean *out_parsed, GError **error);
gboolean ot_parse_keyvalue (const char *keyvalue, char **out_key, char **out_value, GError **error);
gboolean ot_ptr_array_find_with_equal_func (GPtrArray *haystack, gconstpointer needle,
                                            GEqualFunc equal_func, guint *index_);

G_END_DECLS
