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
#include "otutil.h"
#include <sys/stat.h>

G_BEGIN_DECLS

/* It's what gzip does, 9 is too slow */
#define OSTREE_ARCHIVE_DEFAULT_COMPRESSION_LEVEL (6)

/* Note the permissive group bits. We want to be liberal here and let individual machines
 * narrow permissions as needed via umask. This is important in setups where group ownership
 * can matter for repo management (like OpenShift). */
#define DEFAULT_DIRECTORY_MODE 0775
#define DEFAULT_REGFILE_MODE 0660

/* This exists in glibc's sys/stat.h, but not on musl */
#ifndef ALLPERMS
#define ALLPERMS (S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO)
#endif

/* This file contains private implementation data format definitions
 * read by multiple implementation .c files.
 */

/*
 * File objects are stored as a stream, with one #GVariant header,
 * followed by content.
 *
 * The file header is of the following form:
 *
 * &lt;BE guint32 containing variant length&gt;
 * u - uid
 * u - gid
 * u - mode
 * u - rdev (must be 0)
 * s - symlink target
 * a(ayay) - xattrs
 *
 * Then the rest of the stream is data.
 */
#define _OSTREE_FILE_HEADER_GVARIANT_FORMAT G_VARIANT_TYPE ("(uuuusa(ayay))")

/*
 * A variation on %OSTREE_FILE_HEADER_GVARIANT_FORMAT, used for
 * storing zlib-compressed content objects.
 *
 * &lt;BE guint32 containing variant length&gt;
 * t - size
 * u - uid
 * u - gid
 * u - mode
 * u - rdev (must be 0)
 * s - symlink target
 * a(ayay) - xattrs
 * ---
 * zlib-compressed data
 */
#define _OSTREE_ZLIB_FILE_HEADER_GVARIANT_FORMAT G_VARIANT_TYPE ("(tuuuusa(ayay))")

GBytes *_ostree_file_header_new (GFileInfo *file_info, GVariant *xattrs);

GBytes *_ostree_zlib_file_header_new (GFileInfo *file_info, GVariant *xattrs);

gboolean _ostree_make_temporary_symlink_at (int tmp_dirfd, const char *target, char **out_name,
                                            GCancellable *cancellable, GError **error);

GFileInfo *_ostree_stbuf_to_gfileinfo (const struct stat *stbuf);
void _ostree_gfileinfo_to_stbuf (GFileInfo *file_info, struct stat *out_stbuf);
gboolean _ostree_gfileinfo_equal (GFileInfo *a, GFileInfo *b);
gboolean _ostree_stbuf_equal (struct stat *stbuf_a, struct stat *stbuf_b);
GFileInfo *_ostree_mode_uidgid_to_gfileinfo (mode_t mode, uid_t uid, gid_t gid);
gboolean _ostree_validate_structureof_xattrs (GVariant *xattrs, GError **error);

static inline void
_ostree_checksum_inplace_from_bytes_v (GVariant *csum_v, char *buf)
{
  const guint8 *csum = ostree_checksum_bytes_peek (csum_v);
  g_assert (csum);
  ostree_checksum_inplace_from_bytes (csum, buf);
}

/* XX/checksum-2.extension, but let's just use 256 for a
 * bit of overkill.
 */
#define _OSTREE_LOOSE_PATH_MAX (256)

/* GVariant format for ostree.sizes metadata entries. */
#define _OSTREE_OBJECT_SIZES_ENTRY_SIGNATURE "ay"

char *_ostree_get_relative_object_path (const char *checksum, OstreeObjectType type,
                                        gboolean compressed);

char *_ostree_get_relative_static_delta_path (const char *from, const char *to, const char *target);

char *_ostree_get_relative_static_delta_superblock_path (const char *from, const char *to);

char *_ostree_get_relative_static_delta_detachedmeta_path (const char *from, const char *to);

char *_ostree_get_relative_static_delta_part_path (const char *from, const char *to, guint i);

char *_ostree_get_relative_static_delta_index_path (const char *to);

static inline char *
_ostree_get_commitpartial_path (const char *checksum)
{
  return g_strconcat ("state/", checksum, ".commitpartial", NULL);
}

gboolean _ostree_validate_ref_fragment (const char *fragment, GError **error);

gboolean _ostree_validate_bareuseronly_mode (guint32 mode, const char *checksum, GError **error);
static inline gboolean
_ostree_validate_bareuseronly_mode_finfo (GFileInfo *finfo, const char *checksum, GError **error)
{
  const guint32 content_mode = g_file_info_get_attribute_uint32 (finfo, "unix::mode");
  return _ostree_validate_bareuseronly_mode (content_mode, checksum, error);
}

gboolean _ostree_compare_object_checksum (OstreeObjectType objtype, const char *expected,
                                          const char *actual, GError **error);

gboolean _ostree_parse_delta_name (const char *delta_name, char **out_from, char **out_to,
                                   GError **error);

void _ostree_loose_path (char *buf, const char *checksum, OstreeObjectType objtype,
                         OstreeRepoMode repo_mode);

gboolean _ostree_validate_structureof_metadata (OstreeObjectType objtype, GVariant *commit,
                                                GError **error);

gboolean _ostree_verify_metadata_object (OstreeObjectType objtype, const char *expected_checksum,
                                         GVariant *metadata, GError **error);

#define _OSTREE_METADATA_GPGSIGS_NAME "ostree.gpgsigs"
#define _OSTREE_METADATA_GPGSIGS_TYPE G_VARIANT_TYPE ("aay")

static inline gboolean
_ostree_repo_mode_is_bare (OstreeRepoMode mode)
{
  return mode == OSTREE_REPO_MODE_BARE || mode == OSTREE_REPO_MODE_BARE_USER
         || mode == OSTREE_REPO_MODE_BARE_USER_ONLY || mode == OSTREE_REPO_MODE_BARE_SPLIT_XATTRS;
}

#ifndef OSTREE_DISABLE_GPGME
GVariant *_ostree_detached_metadata_append_gpg_sig (GVariant *existing_metadata,
                                                    GBytes *signature_bytes);
#endif

GFile *_ostree_get_default_sysroot_path (void);

_OSTREE_PUBLIC
gboolean _ostree_raw_file_to_archive_stream (GInputStream *input, GFileInfo *file_info,
                                             GVariant *xattrs, guint compression_level,
                                             GInputStream **out_input, GCancellable *cancellable,
                                             GError **error);

gboolean _ostree_compare_timestamps (const char *current_rev, guint64 current_ts,
                                     const char *new_rev, guint64 new_ts, GError **error);

G_END_DECLS
