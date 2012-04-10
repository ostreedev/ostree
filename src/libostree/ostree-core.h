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
  OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT = 2,  /* .archive-content */
  OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META = 3,  /* .archive-meta */
  OSTREE_OBJECT_TYPE_DIR_TREE = 4,  /* .dirtree */
  OSTREE_OBJECT_TYPE_DIR_META = 5,  /* .dirmeta */
  OSTREE_OBJECT_TYPE_COMMIT = 6     /* .commit */
} OstreeObjectType;

#define OSTREE_OBJECT_TYPE_IS_META(t) (t >= 3 && t <= 6)
#define OSTREE_OBJECT_TYPE_LAST OSTREE_OBJECT_TYPE_COMMIT

#define OSTREE_SERIALIZED_VARIANT_FORMAT G_VARIANT_TYPE("(uv)")

/*
 * xattr objects:
 * a(ayay) - array of (name, value) pairs, both binary data, though name is a bytestring
 */
#define OSTREE_XATTR_GVARIANT_FORMAT "a(ayay)"

/*
 * dirmeta objects:
 * u - uid
 * u - gid
 * u - mode
 * a(ayay) - xattrs
 */
#define OSTREE_DIRMETA_GVARIANT_FORMAT G_VARIANT_TYPE ("(uuua(ayay))")

/*
 * Tree objects:
 * a(say) - array of (filename, checksum) for files
 * a(sayay) - array of (dirname, tree_checksum, meta_checksum) for directories
 */
#define OSTREE_TREE_GVARIANT_FORMAT G_VARIANT_TYPE ("(a(say)a(sayay))")

/*
 * Commit objects:
 * a{sv} - Metadata
 * ay - parent checksum (empty string for initial)
 * a(say) - Related objects
 * s - subject 
 * s - body
 * t - Timestamp in seconds since the epoch (UTC)
 * ay - Root tree contents
 * ay - Root tree metadata
 */
#define OSTREE_COMMIT_GVARIANT_FORMAT G_VARIANT_TYPE ("(a{sv}aya(say)sstayay)")

/* Archive file objects:
 * u - uid
 * u - gid
 * u - mode
 * u - rdev
 * s - symlink target
 * a(ayay) - xattrs
 */
#define OSTREE_ARCHIVED_FILE_VARIANT_FORMAT G_VARIANT_TYPE ("(uuuusa(ayay))")

/* Pack super index
 * s - OSTv0SUPERPACKINDEX
 * a{sv} - Metadata
 * a(ayay) - metadata packs (pack file checksum, bloom filter)
 * a(ayay) - data packs (pack file checksum, bloom filter)
 */
#define OSTREE_PACK_SUPER_INDEX_VARIANT_FORMAT G_VARIANT_TYPE ("(sa{sv}a(ayay)a(ayay))")

/* Pack index
 * s - OSTv0PACKINDEX
 * a{sv} - Metadata
 * a(yayt) - (objtype, checksum, offset into packfile)
 */
#define OSTREE_PACK_INDEX_VARIANT_FORMAT G_VARIANT_TYPE ("(sa{sv}a(yayt))")

typedef enum {
  OSTREE_PACK_FILE_ENTRY_FLAG_NONE = 0,
  OSTREE_PACK_FILE_ENTRY_FLAG_GZIP = (1 << 0)
} OstreePackFileEntryFlag;

/* Data Pack files
 * s - OSTv0PACKDATAFILE
 * a{sv} - Metadata
 * t - number of entries
 *
 * Repeating pair of:
 * <padding to alignment of 8>
 * ( yyayay ) - objtype, flags, checksum, data
 */
#define OSTREE_PACK_DATA_FILE_VARIANT_FORMAT G_VARIANT_TYPE ("(yyayay)")

/* Meta Pack files
 * s - OSTv0PACKMETAFILE
 * a{sv} - Metadata
 * t - number of entries
 *
 * Repeating pair of:
 * <padding to alignment of 8>
 * ( yayv ) - objtype, checksum, data
 */
#define OSTREE_PACK_META_FILE_VARIANT_FORMAT G_VARIANT_TYPE ("(yayv)")

gboolean ostree_validate_checksum_string (const char *sha256,
                                          GError    **error);

guchar *ostree_checksum_to_bytes (const char *checksum);
GVariant *ostree_checksum_to_bytes_v (const char *checksum);

char * ostree_checksum_from_bytes (const guchar *bytes);
char * ostree_checksum_from_bytes_v (GVariant *bytes);

const guchar *ostree_checksum_bytes_peek (GVariant *bytes);

int ostree_cmp_checksum_bytes (const guchar *a, const guchar *b);

gboolean ostree_validate_rev (const char *rev, GError **error);

void ostree_checksum_update_stat (GChecksum *checksum, guint32 uid, guint32 gid, guint32 mode);

const char * ostree_object_type_to_string (OstreeObjectType objtype);

OstreeObjectType ostree_object_type_from_string (const char *str);

guint ostree_hash_object_name (gconstpointer a);

GVariant *ostree_object_name_serialize (const char *checksum,
                                        OstreeObjectType objtype);

void ostree_object_name_deserialize (GVariant         *variant,
                                     const char      **out_checksum,
                                     OstreeObjectType *out_objtype);

char * ostree_object_to_string (const char *checksum,
                                OstreeObjectType objtype);

void ostree_object_from_string (const char *str,
                                gchar     **out_checksum,
                                OstreeObjectType *out_objtype);

char *ostree_get_relative_object_path (const char        *checksum,
                                       OstreeObjectType   type);

char *ostree_get_pack_index_name (gboolean        is_meta,
                                  const char     *checksum);
char *ostree_get_pack_data_name (gboolean        is_meta,
                                 const char     *checksum);

char *ostree_get_relative_pack_index_path (gboolean        is_meta,
                                           const char     *checksum);
char *ostree_get_relative_pack_data_path (gboolean        is_meta,
                                          const char     *checksum);

gboolean ostree_get_xattrs_for_file (GFile         *f,
                                     GVariant     **out_xattrs,
                                     GCancellable  *cancellable,
                                     GError       **error);

GVariant *ostree_wrap_metadata_variant (OstreeObjectType type, GVariant *metadata);

gboolean ostree_unwrap_metadata (GVariant              *container,
                                 OstreeObjectType       expected_type,
                                 GVariant             **out_variant,
                                 GError               **error);

gboolean ostree_set_xattrs (GFile *f, GVariant *xattrs,
                            GCancellable *cancellable, GError **error);

gboolean ostree_map_metadata_file (GFile                       *file,
                                   OstreeObjectType             expected_type,
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

gboolean ostree_create_temp_hardlink (GFile            *dir,
                                      GFile            *src,
                                      const char       *prefix,
                                      const char       *suffix,
                                      GFile           **out_file,
                                      GCancellable     *cancellable,
                                      GError          **error);

GVariant *ostree_create_archive_file_metadata (GFileInfo   *file_info,
                                               GVariant    *xattrs);

gboolean ostree_parse_archived_file_meta (GVariant         *data,
                                          GFileInfo       **out_file_info,
                                          GVariant        **out_xattrs,
                                          GError          **error);

gboolean ostree_read_pack_entry_raw (guchar           *pack_data,
                                     guint64           pack_len,
                                     guint64           object_offset,
                                     gboolean          trusted,
                                     gboolean          is_meta,
                                     GVariant        **out_entry,
                                     GCancellable     *cancellable,
                                     GError          **error);

GInputStream *ostree_read_pack_entry_as_stream (GVariant *pack_entry);

gboolean ostree_read_pack_entry_variant (GVariant         *pack_entry,
                                         OstreeObjectType  expected_objtype,
                                         gboolean          trusted,
                                         GVariant        **out_variant,
                                         GCancellable     *cancellable,
                                         GError          **error);

gboolean ostree_pack_index_search (GVariant            *index,
                                   GVariant           *csum_bytes,
                                   OstreeObjectType    objtype,
                                   guint64            *out_offset);

/** VALIDATION **/

gboolean ostree_validate_structureof_objtype (guchar    objtype,
                                              GError   **error);

gboolean ostree_validate_structureof_csum_v (GVariant  *checksum,
                                             GError   **error);

gboolean ostree_validate_structureof_checksum_string (const char *checksum,
                                                      GError   **error);

gboolean ostree_validate_structureof_file_mode (guint32            mode,
                                                GError           **error);

gboolean ostree_validate_structureof_commit (GVariant      *index,
                                             GError       **error);

gboolean ostree_validate_structureof_dirtree (GVariant      *index,
                                              GError       **error);

gboolean ostree_validate_structureof_dirmeta (GVariant      *index,
                                              GError       **error);

gboolean ostree_validate_structureof_pack_index (GVariant      *index,
                                                 GError       **error);

gboolean ostree_validate_structureof_pack_superindex (GVariant      *superindex,
                                                      GError       **error);

#endif /* _OSTREE_REPO */
