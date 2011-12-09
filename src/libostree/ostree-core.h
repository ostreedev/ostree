/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

#ifndef _OSTREE_CORE
#define _OSTREE_CORE

#include <otutil.h>

G_BEGIN_DECLS

#define OSTREE_MAX_METADATA_SIZE (1 << 26)

#define OSTREE_EMPTY_STRING_SHA256 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

typedef enum {
  OSTREE_OBJECT_TYPE_RAW_FILE = 1,   /* .raw */
  OSTREE_OBJECT_TYPE_ARCHIVED_FILE = 2,  /* .archive */
  OSTREE_OBJECT_TYPE_DIR_TREE = 3,  /* .dirtree */
  OSTREE_OBJECT_TYPE_DIR_META = 4,  /* .dirmeta */
  OSTREE_OBJECT_TYPE_COMMIT = 5     /* .commit */
} OstreeObjectType;

#define OSTREE_OBJECT_TYPE_IS_META(t) (t >= 3 && t <= 5)
#define OSTREE_OBJECT_TYPE_LAST OSTREE_OBJECT_TYPE_COMMIT

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

void ostree_checksum_update_stat (GChecksum *checksum, guint32 uid, guint32 gid, guint32 mode);

char *ostree_get_relative_object_path (const char        *checksum,
                                       OstreeObjectType   type);

GVariant *ostree_get_xattrs_for_file (GFile       *f,
                                      GError     **error);

GVariant *ostree_wrap_metadata_variant (OstreeObjectType type, GVariant *metadata);

gboolean ostree_set_xattrs (GFile *f, GVariant *xattrs,
                            GCancellable *cancellable, GError **error);

gboolean ostree_parse_metadata_file (GFile                       *file,
                                     OstreeObjectType            *out_type,
                                     GVariant                   **out_variant,
                                     GError                     **error);

gboolean ostree_checksum_file_from_input (GFileInfo        *file_info,
                                          GVariant         *xattrs,
                                          GInputStream     *in,
                                          OstreeObjectType  objtype,
                                          GChecksum       **out_checksum,
                                          GCancellable     *cancellable,
                                          GError          **error);

gboolean ostree_checksum_file (GFile             *f,
                               OstreeObjectType   type,
                               GChecksum        **out_checksum,
                               GCancellable      *cancellable,
                               GError           **error);

void ostree_checksum_file_async (GFile                 *f,
                                 OstreeObjectType       objtype,
                                 int                    io_priority,
                                 GCancellable          *cancellable,
                                 GAsyncReadyCallback    callback,
                                 gpointer               user_data);

gboolean ostree_checksum_file_async_finish (GFile          *f,
                                            GAsyncResult   *result,
                                            GChecksum     **out_checksum,
                                            GError        **error);

GVariant *ostree_create_directory_metadata (GFileInfo *dir_info,
                                            GVariant  *xattrs);

/* Archived files:
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
#define OSTREE_ARCHIVED_FILE_VARIANT_FORMAT "(uuuua(ayay)t)"

gboolean  ostree_archive_file_for_input (GOutputStream     *output,
                                         GFileInfo         *finfo,
                                         GInputStream      *input,
                                         GVariant          *xattrs,
                                         GChecksum        **out_checksum,
                                         GCancellable     *cancellable,
                                         GError          **error);

gboolean ostree_create_file_from_input (GFile          *file,
                                        GFileInfo      *finfo,
                                        GVariant       *xattrs,
                                        GInputStream   *input,
                                        OstreeObjectType objtype,
                                        GChecksum     **out_checksum,
                                        GCancellable   *cancellable,
                                        GError        **error);

gboolean ostree_create_temp_file_from_input (GFile            *dir,
                                             const char       *prefix,
                                             const char       *suffix,
                                             GFileInfo        *finfo,
                                             GVariant         *xattrs,
                                             GInputStream     *input,
                                             OstreeObjectType objtype,
                                             GFile           **out_file,
                                             GChecksum       **out_checksum,
                                             GCancellable     *cancellable,
                                             GError          **error);

gboolean ostree_create_temp_regular_file (GFile            *dir,
                                          const char       *prefix,
                                          const char       *suffix,
                                          GFile           **out_file,
                                          GOutputStream   **out_stream,
                                          GCancellable     *cancellable,
                                          GError          **error);

gboolean ostree_parse_archived_file (GFile            *file,
                                     GFileInfo       **out_file_info,
                                     GVariant        **out_xattrs,
                                     GInputStream    **out_content,
                                     GCancellable     *cancellable,
                                     GError          **error);

gboolean ostree_unpack_object (GFile             *file,
                               OstreeObjectType  objtype,
                               GFile             *dest,    
                               GChecksum   **out_checksum,
                               GError      **error);


#endif /* _OSTREE_REPO */
