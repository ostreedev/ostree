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

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * OSTREE_MAX_METADATA_SIZE:
 * 
 * Maximum permitted size in bytes of metadata objects.
 */
#define OSTREE_MAX_METADATA_SIZE (1 << 26)

/**
 * OSTREE_MAX_RECURSION:
 * 
 * Maximum depth of metadata.
 */
#define OSTREE_MAX_RECURSION (256)

/**
 * OstreeObjectType:
 * @OSTREE_OBJECT_TYPE_FILE: Content; regular file, symbolic link
 * @OSTREE_OBJECT_TYPE_DIR_TREE: List of children (trees or files), and metadata
 * @OSTREE_OBJECT_TYPE_DIR_META: Directory metadata
 * @OSTREE_OBJECT_TYPE_COMMIT: Toplevel object, refers to tree and dirmeta for root
 *
 * Enumeration for core object types; %OSTREE_OBJECT_TYPE_FILE is for
 * content, the other types are metadata.
 */
typedef enum {
  OSTREE_OBJECT_TYPE_FILE = 1,      /* .file */
  OSTREE_OBJECT_TYPE_DIR_TREE = 2,  /* .dirtree */
  OSTREE_OBJECT_TYPE_DIR_META = 3,  /* .dirmeta */
  OSTREE_OBJECT_TYPE_COMMIT = 4     /* .commit */
} OstreeObjectType;

/**
 * OSTREE_OBJECT_TYPE_IS_META:
 * @t: An #OstreeObjectType
 *
 * Returns: %TRUE if object type is metadata
 */
#define OSTREE_OBJECT_TYPE_IS_META(t) (t >= 2 && t <= 4)

/**
 * OSTREE_OBJECT_TYPE_LAST:
 *
 * Last valid object type; use this to validate ranges.
 */
#define OSTREE_OBJECT_TYPE_LAST OSTREE_OBJECT_TYPE_COMMIT

/**
 * OSTREE_FILE_HEADER_GVARIANT_FORMAT:
 *
 * File objects are stored as a stream, with one #GVariant header,
 * followed by content.
 * 
 * The file header is of the following form:
 *
 * &lt;BE guint32 containing variant length&gt;
 * u - uid
 * u - gid
 * u - mode
 * u - rdev
 * s - symlink target 
 * a(ayay) - xattrs
 *
 * Then the rest of the stream is data.
 */
#define OSTREE_FILE_HEADER_GVARIANT_FORMAT G_VARIANT_TYPE ("(uuuusa(ayay))")

/**
 * OSTREE_DIRMETA_GVARIANT_FORMAT:
 *
 * u - uid
 * u - gid
 * u - mode
 * a(ayay) - xattrs
 */
#define OSTREE_DIRMETA_GVARIANT_FORMAT G_VARIANT_TYPE ("(uuua(ayay))")

/**
 * OSTREE_TREE_GVARIANT_FORMAT:
 *
 * a(say) - array of (filename, checksum) for files
 * a(sayay) - array of (dirname, tree_checksum, meta_checksum) for directories
 */
#define OSTREE_TREE_GVARIANT_FORMAT G_VARIANT_TYPE ("(a(say)a(sayay))")

/**
 * OSTREE_COMMIT_GVARIANT_FORMAT:
 *
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

/**
 * OSTREE_ZLIB_FILE_HEADER_GVARIANT_FORMAT:
 *
 * This is a variation on %OSTREE_FILE_HEADER_GVARIANT_FORMAT, used for
 * storing zlib-compressed content objects.
 *
 * &lt;BE guint32 containing variant length&gt;
 * t - size
 * u - uid
 * u - gid
 * u - mode
 * u - rdev
 * s - symlink target 
 * a(ayay) - xattrs
 * ---
 * zlib-compressed data
 */
#define OSTREE_ZLIB_FILE_HEADER_GVARIANT_FORMAT G_VARIANT_TYPE ("(tuuuusa(ayay))")

const GVariantType *ostree_metadata_variant_type (OstreeObjectType objtype);

gboolean ostree_validate_checksum_string (const char *sha256,
                                          GError    **error);

guchar *ostree_checksum_to_bytes (const char *checksum);
GVariant *ostree_checksum_to_bytes_v (const char *checksum);

char * ostree_checksum_from_bytes (const guchar *csum);
char * ostree_checksum_from_bytes_v (GVariant *csum_v);

void ostree_checksum_inplace_from_bytes (const guchar *csum,
                                         char         *buf);

void ostree_checksum_inplace_to_bytes (const char *checksum,
                                       guchar     *buf);

const guchar *ostree_checksum_bytes_peek (GVariant *bytes);

int ostree_cmp_checksum_bytes (const guchar *a, const guchar *b);

gboolean ostree_validate_rev (const char *rev, GError **error);

gboolean ostree_parse_refspec (const char *refspec,
                               char      **out_remote,
                               char      **out_ref,
                               GError    **error);

void ostree_checksum_update_meta (GChecksum *checksum, GFileInfo *file_info, GVariant  *xattrs);

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
                                       OstreeObjectType   type,
                                       gboolean           compressed);

char *ostree_get_relative_archive_content_path (const char        *checksum);

gboolean ostree_get_xattrs_for_file (GFile         *f,
                                     GVariant     **out_xattrs,
                                     GCancellable  *cancellable,
                                     GError       **error);

gboolean ostree_set_xattrs (GFile *f, GVariant *xattrs,
                            GCancellable *cancellable, GError **error);

gboolean ostree_map_metadata_file (GFile                       *file,
                                   OstreeObjectType             expected_type,
                                   GVariant                   **out_variant,
                                   GError                     **error);

gboolean ostree_write_variant_with_size (GOutputStream      *output,
                                         GVariant           *variant,
                                         guint64             alignment_offset,
                                         gsize              *out_bytes_written,
                                         GChecksum          *checksum,
                                         GCancellable       *cancellable,
                                         GError            **error);

GVariant *ostree_file_header_new (GFileInfo         *file_info,
                                  GVariant          *xattrs);
GVariant *ostree_zlib_file_header_new (GFileInfo         *file_info,
                                       GVariant          *xattrs);

gboolean ostree_file_header_parse (GVariant         *data,
                                   GFileInfo       **out_file_info,
                                   GVariant        **out_xattrs,
                                   GError          **error);
gboolean ostree_zlib_file_header_parse (GVariant         *data,
                                        GFileInfo       **out_file_info,
                                        GVariant        **out_xattrs,
                                        GError          **error);

gboolean
ostree_content_stream_parse (gboolean                compressed,
                             GInputStream           *input,
                             guint64                 input_length,
                             gboolean                trusted,
                             GInputStream          **out_input,
                             GFileInfo             **out_file_info,
                             GVariant              **out_xattrs,
                             GCancellable           *cancellable,
                             GError                **error);

gboolean ostree_content_file_parse (gboolean                compressed,
                                    GFile                  *content_path,
                                    gboolean                trusted,
                                    GInputStream          **out_input,
                                    GFileInfo             **out_file_info,
                                    GVariant              **out_xattrs,
                                    GCancellable           *cancellable,
                                    GError                **error);

gboolean ostree_write_file_header_update_checksum (GOutputStream         *out,
                                                   GVariant              *variant,
                                                   GChecksum             *checksum,
                                                   GCancellable          *cancellable,
                                                   GError               **error);

gboolean ostree_raw_file_to_content_stream (GInputStream       *input,
                                            GFileInfo          *file_info,
                                            GVariant           *xattrs,
                                            GInputStream      **out_input,
                                            guint64            *out_length,
                                            GCancellable       *cancellable,
                                            GError            **error);

gboolean ostree_checksum_file_from_input (GFileInfo        *file_info,
                                          GVariant         *xattrs,
                                          GInputStream     *in,
                                          OstreeObjectType  objtype,
                                          guchar          **out_csum,
                                          GCancellable     *cancellable,
                                          GError          **error);

gboolean ostree_checksum_file (GFile             *f,
                               OstreeObjectType   type,
                               guchar           **out_csum,
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
                                            guchar        **out_csum,
                                            GError        **error);

GVariant *ostree_create_directory_metadata (GFileInfo *dir_info,
                                            GVariant  *xattrs);

gboolean ostree_create_file_from_input (GFile          *file,
                                        GFileInfo      *finfo,
                                        GVariant       *xattrs,
                                        GInputStream   *input,
                                        GCancellable   *cancellable,
                                        GError        **error);

gboolean ostree_create_temp_file_from_input (GFile            *dir,
                                             const char       *prefix,
                                             const char       *suffix,
                                             GFileInfo        *finfo,
                                             GVariant         *xattrs,
                                             GInputStream     *input,
                                             GFile           **out_file,
                                             GCancellable     *cancellable,
                                             GError          **error);

gboolean ostree_create_temp_dir (GFile            *dir,
                                 const char       *prefix,
                                 const char       *suffix,
                                 GFile           **out_file,
                                 GCancellable     *cancellable,
                                 GError          **error);

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

gchar *  ostree_commit_get_parent            (GVariant  *commit_variant);

G_END_DECLS
