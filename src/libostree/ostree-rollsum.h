/*
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
