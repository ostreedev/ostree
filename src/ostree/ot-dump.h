/*
 * Copyright (C) 2013 Stef Walter <stefw@redhat.com>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
