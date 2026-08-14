/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

#include "ostree-core.h"

G_BEGIN_DECLS

/* Maximum uncompressed size for a single static delta part (512 MiB).
 * Enforced on both generation and consumption sides to prevent
 * decompression bombs from exhausting memory/disk (CVE / RHEL-189208).
 *
 * The delta compiler splits parts at max-chunk-size (default 32 MB), so
 * legitimate parts are typically well under 32 MB uncompressed.  512 MiB
 * provides ~16x headroom over the default, which is generous enough to
 * accommodate large custom --max-chunk-size values while still rejecting
 * decompression bombs that would expand to gigabytes.
 */
#define OSTREE_STATIC_DELTA_PART_MAX_USIZE_BYTES (512ULL * 1024ULL * 1024ULL)

/* The declared "usize" in a delta part header only accounts for the final
 * on-disk size of the objects the part will produce; the part payload
 * that actually gets decompressed is larger.  The constants below bound
 * that difference so a decompression limit can be derived from usize
 * without either false-positiving on legitimate parts or degenerating
 * into a check that never rejects anything (see
 * _ostree_static_delta_compute_part_margin() in
 * ostree-repo-static-delta-core.c for how they're combined).  Each term
 * is sized from the actual on-disk formats in
 * ostree-repo-static-delta-compilation.c and ostree-varint.c rather than
 * from a single round guess, so the resulting margin stays proportionate
 * as the number of objects in a part grows.
 */

/* Flat per-part overhead: GVariant framing for the part's outer tuple and
 * the operations/payload byte arrays.  A few KiB is generous here; this
 * doesn't scale with content.
 */
#define OSTREE_STATIC_DELTA_PART_FIXED_OVERHEAD_BYTES (4ULL * 1024ULL)

/* Per-object operations overhead: each object contributes a mode table
 * entry ("(uuu)", 12 bytes, deduplicated but bounded per-object in the
 * worst case) plus opcode bytecode.  _ostree_write_varuint64() emits at
 * most 10 bytes, and the largest per-object opcode sequence is the
 * bsdiff path (SET_READ_SOURCE, OPEN, BSPATCH, CLOSE, UNSET_READ_SOURCE:
 * 5 opcodes + 6 varints = 65 bytes) plus its embedded 32-byte source
 * checksum, which isn't counted in usize at all.  109 bytes worst case;
 * round up generously.
 */
#define OSTREE_STATIC_DELTA_PART_OP_OVERHEAD_PER_OBJECT_BYTES 256ULL

/* Per-object xattr allowance: xattrs are written into the payload in full
 * and aren't reflected in usize either.  There's no way to derive a true
 * worst-case bound for this from filesystem limits: ext4 caps total
 * attribute bytes per inode at ~4 KiB (one external block plus a little
 * in-inode space), but that bound doesn't hold in general -- XFS and
 * Btrfs impose no total per-inode limit, and even ext4 with the
 * (non-default) ea_inode feature can push individual values up to
 * XATTR_SIZE_MAX (64 KiB) each across its ~100-250 max entries.  A file
 * could in principle carry many such values on some filesystem; fully
 * covering that here would require a per-object allowance in the tens of
 * MiB, which for parts with more than a handful of objects would swamp
 * this margin and degenerate the check back into "always equal to the
 * hard cap" -- the exact failure mode we moved away from a flat
 * 1 MiB/object margin to avoid.  So use a single XATTR_SIZE_MAX (64 KiB)
 * as a generous but pragmatic allowance: real xattr sets (SELinux label,
 * capabilities, ACLs, IMA/EVM signatures) total well under 2 KiB in
 * practice, so this covers legitimate content with 30x+ headroom to
 * spare.  Pathological xattr counts beyond that are left to
 * OSTREE_STATIC_DELTA_PART_MAX_USIZE_BYTES, the same hard-cap backstop
 * that bounds this whole margin.
 */
#define OSTREE_STATIC_DELTA_PART_XATTR_ALLOWANCE_PER_OBJECT_BYTES (64ULL * 1024ULL)

/* Rollsum overhead divisor: rollsum (bsdiff-like binary delta against a
 * similar file) emits a WRITE op per matched/unmatched chunk, and chunk
 * boundaries come from bupsplit's content-defined chunking, which
 * averages BUP_BLOBSIZE (8 KiB) per chunk.  At up to ~64 bytes of opcode
 * overhead per chunk, that's an expected overhead of roughly usize/128;
 * dividing by 32 instead bakes in a further 4x safety margin for content
 * that chunks more finely than average.
 */
#define OSTREE_STATIC_DELTA_PART_ROLLSUM_OVERHEAD_DIVISOR 32ULL

/* Multiplier-based safety margin applied on top of the other margin terms.
 * This accounts for the gap between the declared usize and the actual
 * decompressed payload size for deltas generated before the overhead
 * accounting fix (PR #3618).  Older delta generators didn't include
 * mode/xattr tables or operations bytecode in the usize declaration, so
 * the difference can exceed the formulaic per-object estimates in
 * OSTREE_STATIC_DELTA_PART_OP_OVERHEAD_PER_OBJECT_BYTES and
 * OSTREE_STATIC_DELTA_PART_XATTR_ALLOWANCE_PER_OBJECT_BYTES.
 *
 * The effective decompression limit is at least
 * expected_usize * OSTREE_STATIC_DELTA_PART_USIZE_MULTIPLIER plus
 * the other margin terms, still bounded by
 * OSTREE_STATIC_DELTA_PART_MAX_USIZE_BYTES (512 MiB). A multiplier of 1
 * doubles the effective limit (expected_usize + expected_usize = 2x), which
 * is sufficient for all known legitimate deltas while remaining well
 * below the hard cap.
 */
#define OSTREE_STATIC_DELTA_PART_USIZE_MULTIPLIER 1ULL

/* 1 byte for object type, 32 bytes for checksum */
#define OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN 33

#define OSTREE_SUMMARY_STATIC_DELTAS "ostree.static-deltas"

/**
 * OSTREE_STATIC_DELTA_PART_PAYLOAD_FORMAT_V0:
 *
 *   y  compression type (0: none, 'x': lzma)
 *   ---
 *   a(uuu) modes
 *   aa(ayay) xattrs
 *   ay raw data source
 *   ay operations
 */
#define OSTREE_STATIC_DELTA_PART_PAYLOAD_FORMAT_V0 "(a(uuu)aa(ayay)ayay)"

/**
 * OSTREE_STATIC_DELTA_META_ENTRY_FORMAT:
 *
 *   u: version     (non-canonical endian)
 *   ay checksum
 *   guint64 size:   Total size of delta (sum of parts) (non-canonical endian)
 *   guint64 usize:   Uncompressed size of resulting objects on disk (non-canonical endian)
 *   ARRAY[(guint8 objtype, csum object)]
 *
 * The checksum is of the delta payload, and each entry in the array
 * represents an OSTree object which will be created by the deltapart.
 */

#define OSTREE_STATIC_DELTA_META_ENTRY_FORMAT "(uayttay)"

/**
 * OSTREE_STATIC_DELTA_FALLBACK_FORMAT:
 *
 * y: objtype
 * ay: checksum
 * t: compressed size (non-canonical endian)
 * t: uncompressed size (non-canonical endian)
 *
 * Object to fetch invididually; includes compressed/uncompressed size.
 */
#define OSTREE_STATIC_DELTA_FALLBACK_FORMAT "(yaytt)"

/**
 * OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT:
 *
 * A .delta object is a custom binary format.  It has the following high
 * level form:
 *
 * delta-descriptor:
 *   metadata: a{sv}
 *   t: timestamp (big endian)
 *   from: ay checksum
 *   to: ay checksum
 *   commit: new commit object
 *   ARRAY[(csum from, csum to)]: ay
 *   ARRAY[delta-meta-entry]
 *   array[fallback]
 *
 * The metadata would include things like a version number, as well as
 * extended verification data like a GPG signature.
 *
 * The second array is an array of delta objects that should be
 * fetched and applied before this one.  This is a fairly generic
 * recursion mechanism that would potentially allow saving significant
 * storage space on the server.
 *
 * The heart of the static delta: the array of delta parts.
 *
 * Finally, we have the fallback array, which is the set of objects to
 * fetch individually - the compiler determined it wasn't worth
 * duplicating the space.
 */
#define OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT \
  "(a{sv}tayay" OSTREE_COMMIT_GVARIANT_STRING "aya" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT \
  "a" OSTREE_STATIC_DELTA_FALLBACK_FORMAT ")"

/**
 * OSTREE_STATIC_DELTA_SIGNED_FORMAT
 *
 *   magic: t magic number, 8 bytes for alignment
 *   superblock: ay delta supeblock variant
 *   signatures: a{sv}
 *
 * The signed static delta starts with the 'OSTSGNDT' magic number followed by
 * the array of bytes containing the superblock used for the signature.
 *
 * Then, the signatures array contains the signatures of the superblock. A
 * signature has the following form:
 *  type: signature key
 *  signature: variant depending on type used
 */
#define OSTREE_STATIC_DELTA_SIGNED_FORMAT "(taya{sv})"

#define OSTREE_STATIC_DELTA_SIGNED_MAGIC 0x4F535453474E4454 /* OSTSGNDT */

typedef enum
{
  OSTREE_STATIC_DELTA_OPEN_FLAGS_NONE = 0,
  OSTREE_STATIC_DELTA_OPEN_FLAGS_SKIP_CHECKSUM = (1 << 0),
  OSTREE_STATIC_DELTA_OPEN_FLAGS_VARIANT_TRUSTED = (1 << 1)
} OstreeStaticDeltaOpenFlags;

typedef enum
{
  OSTREE_STATIC_DELTA_OP_OPEN_SPLICE_AND_CLOSE = 'S',
  OSTREE_STATIC_DELTA_OP_OPEN = 'o',
  OSTREE_STATIC_DELTA_OP_WRITE = 'w',
  OSTREE_STATIC_DELTA_OP_SET_READ_SOURCE = 'r',
  OSTREE_STATIC_DELTA_OP_UNSET_READ_SOURCE = 'R',
  OSTREE_STATIC_DELTA_OP_CLOSE = 'c',
  OSTREE_STATIC_DELTA_OP_BSPATCH = 'B'
} OstreeStaticDeltaOpCode;
#define OSTREE_STATIC_DELTA_N_OPS 7

gboolean _ostree_static_delta_part_open (GInputStream *part_in, GBytes *inline_part_bytes,
                                         OstreeStaticDeltaOpenFlags flags,
                                         const char *expected_checksum, guint64 expected_usize,
                                         guint32 expected_n_objects, GVariant **out_part,
                                         GCancellable *cancellable, GError **error);

typedef struct
{
  guint n_ops_executed[OSTREE_STATIC_DELTA_N_OPS];
} OstreeDeltaExecuteStats;

gboolean _ostree_static_delta_part_execute (OstreeRepo *repo, GVariant *header,
                                            GVariant *part_payload, gboolean stats_only,
                                            OstreeDeltaExecuteStats *stats,
                                            GCancellable *cancellable, GError **error);

void _ostree_static_delta_part_execute_async (OstreeRepo *repo, GVariant *header,
                                              GVariant *part_payload, GCancellable *cancellable,
                                              GAsyncReadyCallback callback, gpointer user_data);

gboolean _ostree_static_delta_part_execute_finish (OstreeRepo *repo, GAsyncResult *result,
                                                   GError **error);

gboolean _ostree_static_delta_parse_checksum_array (GVariant *array, guint8 **out_checksums_array,
                                                    guint *out_n_checksums, GError **error);

gboolean _ostree_repo_static_delta_part_have_all_objects (OstreeRepo *repo,
                                                          GVariant *checksum_array,
                                                          gboolean *out_have_all,
                                                          GCancellable *cancellable,
                                                          GError **error);

typedef struct
{
  char *checksum;
  guint64 size;
  GPtrArray *basenames;
} OstreeDeltaContentSizeNames;

void _ostree_delta_content_sizenames_free (gpointer v);

gboolean _ostree_delta_compute_similar_objects (OstreeRepo *repo, GVariant *from_commit,
                                                GVariant *to_commit,
                                                GHashTable *new_reachable_regfile_content,
                                                guint similarity_percent_threshold,
                                                GHashTable **out_modified_regfile_content,
                                                GCancellable *cancellable, GError **error);

gboolean _ostree_repo_static_delta_query_exists (OstreeRepo *repo, const char *delta_id,
                                                 gboolean *out_exists, GCancellable *cancellable,
                                                 GError **error);
GVariant *_ostree_repo_static_delta_superblock_digest (OstreeRepo *repo, const char *from,
                                                       const char *to, GCancellable *cancellable,
                                                       GError **error);

gboolean _ostree_repo_static_delta_dump (OstreeRepo *repo, const char *delta_id,
                                         GCancellable *cancellable, GError **error);

gboolean _ostree_repo_static_delta_delete (OstreeRepo *repo, const char *delta_id,
                                           GCancellable *cancellable, GError **error);
gboolean _ostree_repo_static_delta_reindex (OstreeRepo *repo, const char *opt_to_commit,
                                            GCancellable *cancellable, GError **error);

/* Used for static deltas which due to a historical mistake are
 * inconsistent endian.
 *
 * https://bugzilla.gnome.org/show_bug.cgi?id=762515
 */
static inline guint32
maybe_swap_endian_u32 (gboolean swap, guint32 v)
{
  if (!swap)
    return v;
  return GUINT32_SWAP_LE_BE (v);
}

static inline guint64
maybe_swap_endian_u64 (gboolean swap, guint64 v)
{
  if (!swap)
    return v;
  return GUINT64_SWAP_LE_BE (v);
}

typedef enum
{
  OSTREE_DELTA_ENDIAN_BIG,
  OSTREE_DELTA_ENDIAN_LITTLE,
  OSTREE_DELTA_ENDIAN_INVALID
} OstreeDeltaEndianness;

OstreeDeltaEndianness _ostree_delta_get_endianness (GVariant *superblock,
                                                    gboolean *out_was_heuristic);

gboolean _ostree_delta_needs_byteswap (GVariant *superblock);

G_END_DECLS
