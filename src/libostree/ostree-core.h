/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#ifndef _OSTREE_CORE
#define _OSTREE_CORE

#include <otutil.h>

G_BEGIN_DECLS

#define OSTREE_EMPTY_STRING_SHA256 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

typedef enum {
  OSTREE_OBJECT_TYPE_FILE = 1,
  OSTREE_OBJECT_TYPE_META = 2,
} OstreeObjectType;

typedef enum {
  OSTREE_SERIALIZED_TREE_VARIANT = 1,
  OSTREE_SERIALIZED_COMMIT_VARIANT = 2,
  OSTREE_SERIALIZED_DIRMETA_VARIANT = 3,
  OSTREE_SERIALIZED_XATTR_VARIANT = 4
} OstreeSerializedVariantType;
#define OSTREE_SERIALIZED_VARIANT_LAST 4

#define OSTREE_SERIALIZED_VARIANT_FORMAT "(uv)"

/*
 * xattr objects:
 * a(ayay) - array of (name, value) pairs, both binary data, though name is a bytestring
 */
#define OSTREE_XATTR_GVARIANT_FORMAT "a(ayay)"

#define OSTREE_DIR_META_VERSION 0
/*
 * dirmeta objects:
 * u - Version
 * u - uid
 * u - gid
 * u - mode
 * a(ayay) - xattrs
 */
#define OSTREE_DIRMETA_GVARIANT_FORMAT "(uuuua(ayay))"

#define OSTREE_TREE_VERSION 0
/*
 * Tree objects:
 * u - Version
 * a{sv} - Metadata
 * a(ss) - array of (filename, checksum) for files
 * a(sss) - array of (dirname, tree_checksum, meta_checksum) for directories
 */
#define OSTREE_TREE_GVARIANT_FORMAT "(ua{sv}a(ss)a(sss)"

#define OSTREE_COMMIT_VERSION 0
/*
 * Commit objects:
 * u - Version
 * a{sv} - Metadata
 * s - parent checksum (empty string for initial)
 * s - subject 
 * s - body
 * t - Timestamp in seconds since the epoch (UTC)
 * s - Root tree contents
 * s - Root tree metadata
 */
#define OSTREE_COMMIT_GVARIANT_FORMAT "(ua{sv}ssstss)"

gboolean ostree_validate_checksum_string (const char *sha256,
                                          GError    **error);

char *ostree_get_relative_object_path (const char *checksum,
                                       OstreeObjectType type,
                                       gboolean         archive);

GVariant *ostree_get_xattrs_for_path (const char   *path,
                                      GError     **error);

gboolean ostree_set_xattrs (const char *path, GVariant *xattrs, GError **error);

gboolean ostree_parse_metadata_file (const char                  *path,
                                     OstreeSerializedVariantType *out_type,
                                     GVariant                   **out_variant,
                                     GError                     **error);

gboolean ostree_stat_and_checksum_file (int dirfd, const char *path,
                                        OstreeObjectType type,
                                        GChecksum **out_checksum,
                                        struct stat *out_stbuf,
                                        GError **error);

/* Packed files:
 *
 * guint32 metadata_length [metadata gvariant] [content]
 *
 * metadata variant:
 * u - Version
 * u - uid
 * u - gid
 * u - mode
 * a(ayay) - xattrs
 * t - content length
 *
 * And then following the end of the variant is the content.  If
 * symlink, then this is the target; if device, then device ID as
 * network byte order uint32.
 */
#define OSTREE_PACK_FILE_VARIANT_FORMAT "(uuuua(ayay)t)"

gboolean  ostree_pack_object (GOutputStream     *output,
                              GFile             *path,
                              OstreeObjectType  objtype,
                              GCancellable     *cancellable,
                              GError          **error);

void ostree_checksum_update_stat (GChecksum *checksum, guint32 uid, guint32 gid, guint32 mode);


#endif /* _OSTREE_REPO */
