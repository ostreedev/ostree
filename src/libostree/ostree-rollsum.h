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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include <gio/gio.h>
#include "libglnx.h"

G_BEGIN_DECLS

typedef struct {
  GHashTable *from_rollsums;
  GHashTable *to_rollsums;
  guint crcmatches;
  guint bufmatches;
  guint total;
  guint64 match_size;
  GPtrArray *matches;
} OstreeRollsumMatches;

OstreeRollsumMatches *
_ostree_compute_rollsum_matches (GBytes                           *from,
                                 GBytes                           *to);

void _ostree_rollsum_matches_free (OstreeRollsumMatches *rollsum);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OstreeRollsumMatches, _ostree_rollsum_matches_free)

G_END_DECLS
