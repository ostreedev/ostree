/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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

#include "config.h"

#include "otutil.h"

void
ot_local_free (void *loc)
{
  void **location = loc;
  if (location)
    g_free (*location);
}

#define _ot_local_free(type, function) do {           \
    void **location = loc;                            \
    if (location)                                     \
      {                                               \
        type *value = *location;                      \
        if (value)                                    \
          function (value);                           \
      }                                               \
  } while (0)

void
ot_local_obj_unref (void *loc)
{
  GObject **location = (GObject**)loc;
  if (location && *location)
    g_object_unref (*location);
}

void
ot_local_variant_unref (GVariant **loc)
{
  if (loc && *loc)
    g_variant_unref (*loc);
}

void
ot_local_ptrarray_unref (GPtrArray **loc)
{
  if (loc && *loc)
    g_ptr_array_unref (*loc);
}

void
ot_local_hashtable_unref (GHashTable **loc)
{
  if (loc && *loc)
    g_hash_table_unref (*loc);
}
