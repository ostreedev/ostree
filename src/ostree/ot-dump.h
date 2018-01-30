/*
 * Copyright (C) 2013 Stef Walter <stefw@redhat.com>
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
 * Author: Stef Walter <stefw@redhat.com>
 */

#pragma once

#include <gio/gio.h>

#include "ostree-core.h"

typedef enum {
  OSTREE_DUMP_NONE = (1 << 0),
  OSTREE_DUMP_RAW = (1 << 1),
  OSTREE_DUMP_UNSWAPPED = (1 << 2),
} OstreeDumpFlags;

void   ot_dump_variant    (GVariant *variant);

void   ot_dump_object     (OstreeObjectType   objtype,
                           const char        *checksum,
                           GVariant          *variant,
                           OstreeDumpFlags    flags);

void   ot_dump_summary_bytes  (GBytes          *summary_bytes,
                               OstreeDumpFlags  flags);
