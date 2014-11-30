/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

#include "ostree-core.h"

G_BEGIN_DECLS

/* Arbitrarily chosen */
#define OSTREE_STATIC_DELTA_PART_MAX_SIZE_BYTES (16*1024*1024)
/* 1 byte for object type, 32 bytes for checksum */
#define OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN 33

/**
 * OSTREE_STATIC_DELTA_PART_PAYLOAD_FORMAT:
 *
 *   ay data source
 *   ay operations
 */
#define OSTREE_STATIC_DELTA_PART_PAYLOAD_FORMAT "(ayay)"

/**
 * OSTREE_STATIC_DELTA_META_ENTRY_FORMAT:
 *
 *   ay checksum
 *   guint64 size:   Total size of delta (sum of parts)
 *   guint64 usize:   Uncompressed size of resulting objects on disk
 *   ARRAY[(guint8 objtype, csum object)]
 *
 * The checksum is of the delta payload, and each entry in the array
 * represents an OSTree object which will be created by the deltapart.
 */

#define OSTREE_STATIC_DELTA_META_ENTRY_FORMAT "(ayttay)"

/**
 * OSTREE_STATIC_DELTA_META_FORMAT:
 *
 * A .delta object is a custom binary format.  It has the following high
 * level form:
 *
 * delta-descriptor:
 *   metadata: a{sv}
 *   timestamp: guint64
 *   ARRAY[(csum from, csum to)]: ay
 *   ARRAY[delta-meta-entry]
 *
 * The metadata would include things like a version number, as well as
 * extended verification data like a GPG signature.
 *
 * The second array is an array of delta objects that should be
 * fetched and applied before this one.  This is a fairly generic
 * recursion mechanism that would potentially allow saving significant
 * storage space on the server.
 */
#define OSTREE_STATIC_DELTA_META_FORMAT "(a{sv}taya" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT ")"

gboolean _ostree_static_delta_part_execute (OstreeRepo      *repo,
                                            GVariant        *header,
                                            GVariant        *part,
                                            GCancellable    *cancellable,
                                            GError         **error);

typedef enum {
  OSTREE_STATIC_DELTA_OP_FETCH = 1,
  OSTREE_STATIC_DELTA_OP_WRITE = 2,
  OSTREE_STATIC_DELTA_OP_GUNZIP = 3,
  OSTREE_STATIC_DELTA_OP_CLOSE = 4,
  OSTREE_STATIC_DELTA_OP_READOBJECT = 5,
  OSTREE_STATIC_DELTA_OP_READPAYLOAD = 6
} OstreeStaticDeltaOpCode;

gboolean
_ostree_static_delta_parse_checksum_array (GVariant      *array,
                                           guint8       **out_checksums_array,
                                           guint         *out_n_checksums,
                                           GError       **error);
G_END_DECLS

