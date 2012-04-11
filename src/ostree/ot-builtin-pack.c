/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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

#include "config.h"

#include "ot-builtins.h"
#include "ostree.h"

#include <glib/gi18n.h>
#include <glib/gprintf.h>

#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

#define OT_DEFAULT_PACK_SIZE_BYTES (50*1024*1024)
#define OT_GZIP_COMPRESSION_LEVEL (8)

static gboolean opt_analyze_only;
static gboolean opt_reindex_only;
static gboolean opt_keep_loose;
static char* opt_pack_size;
static char* opt_int_compression;
static char* opt_ext_compression;

typedef enum {
  OT_COMPRESSION_NONE,
  OT_COMPRESSION_GZIP,
  OT_COMPRESSION_XZ
} OtCompressionType;

static GOptionEntry options[] = {
  { "pack-size", 0, 0, G_OPTION_ARG_STRING, &opt_pack_size, "Maximum uncompressed size of packfiles in bytes; may be suffixed with k, m, or g", "BYTES" },
  { "internal-compression", 0, 0, G_OPTION_ARG_STRING, &opt_int_compression, "Compress objects using COMPRESSION", "COMPRESSION" },
  { "external-compression", 0, 0, G_OPTION_ARG_STRING, &opt_ext_compression, "Compress entire packfiles using COMPRESSION", "COMPRESSION" },
  { "analyze-only", 0, 0, G_OPTION_ARG_NONE, &opt_analyze_only, "Just analyze current state", NULL },
  { "reindex-only", 0, 0, G_OPTION_ARG_NONE, &opt_reindex_only, "Regenerate pack index", NULL },
  { "keep-loose", 0, 0, G_OPTION_ARG_NONE, &opt_keep_loose, "Don't delete loose objects", NULL },
  { NULL }
};

typedef struct {
  OstreeRepo *repo;

  guint64 pack_size;
  OtCompressionType int_compression;
  OtCompressionType ext_compression;

  gboolean had_error;
  GError **error;
} OtRepackData;

typedef struct {
  GOutputStream *out;
  GPtrArray *compressor_argv;
  GPid compress_child_pid;
} OtBuildRepackFile;

static gint
compare_object_data_by_size (gconstpointer    ap,
                             gconstpointer    bp)
{
  GVariant *a = *(void **)ap;
  GVariant *b = *(void **)bp;
  guint64 a_size;
  guint64 b_size;

  g_variant_get_child (a, 2, "t", &a_size);
  g_variant_get_child (b, 2, "t", &b_size);
  if (a == b)
    return 0;
  else if (a > b)
    return 1;
  else
    return -1;
}

static gboolean
write_bytes_update_checksum (GOutputStream *output,
                             gconstpointer  bytes,
                             gsize          len,
                             GChecksum     *checksum,
                             guint64       *inout_offset,
                             GCancellable  *cancellable,
                             GError       **error)
{
  gboolean ret = FALSE;
  gsize bytes_written;

  if (len > 0)
    {
      g_checksum_update (checksum, (guchar*) bytes, len);
      if (!g_output_stream_write_all (output, bytes, len, &bytes_written,
                                      cancellable, error))
        goto out;
      g_assert_cmpint (bytes_written, ==, len);
      *inout_offset += bytes_written;
    }
  
  ret = TRUE;
 out:
  return ret;
}

static gboolean
write_padding (GOutputStream    *output,
               guint             alignment,
               GChecksum        *checksum,
               guint64          *inout_offset,
               GCancellable     *cancellable,
               GError          **error)
{
  gboolean ret = FALSE;
  guint bits;
  guint padding_len;
  guchar padding_nuls[8] = {0, 0, 0, 0, 0, 0, 0, 0};

  if (alignment == 8)
    bits = ((*inout_offset) & 7);
  else
    bits = ((*inout_offset) & 3);

  if (bits > 0)
    {
      padding_len = alignment - bits;
      if (!write_bytes_update_checksum (output, (guchar*)padding_nuls, padding_len,
                                        checksum, inout_offset, cancellable, error))
        goto out;
    }
  
  ret = TRUE;
 out:
  return ret;
}

static gint
compare_index_content (gconstpointer         ap,
                       gconstpointer         bp)
{
  gpointer a = *((gpointer*)ap);
  gpointer b = *((gpointer*)bp);
  GVariant *a_v = a;
  GVariant *b_v = b;
  guchar a_objtype;
  guchar b_objtype;
  guint64 a_offset;
  guint64 b_offset;
  int c;
  ot_lvariant GVariant *a_csum_bytes = NULL;
  ot_lvariant GVariant *b_csum_bytes = NULL;

  g_variant_get (a_v, "(y@ayt)", &a_objtype, &a_csum_bytes, &a_offset);      
  g_variant_get (b_v, "(y@ayt)", &b_objtype, &b_csum_bytes, &b_offset);      
  c = ostree_cmp_checksum_bytes (ostree_checksum_bytes_peek (a_csum_bytes),
                                 ostree_checksum_bytes_peek (b_csum_bytes));
  if (c == 0)
    {
      if (a_objtype < b_objtype)
        c = -1;
      else if (a_objtype > b_objtype)
        c = 1;
    }
  return c;
}

static gboolean
delete_loose_object (OtRepackData     *data,
                     const char       *checksum,
                     OstreeObjectType  objtype,
                     GCancellable     *cancellable,
                     GError          **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *object_path = NULL;
  ot_lobj GFile *content_object_path = NULL;
  ot_lvariant GVariant *archive_meta = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lvariant GVariant *xattrs = NULL;

  object_path = ostree_repo_get_object_path (data->repo, checksum, objtype);
  
  if (!ot_gfile_unlink (object_path, cancellable, error))
    {
      g_prefix_error (error, "Failed to delete archived file metadata '%s'",
                      ot_gfile_get_path_cached (object_path));
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
pack_one_meta_object (OtRepackData        *data,
                      const char          *checksum,
                      OstreeObjectType     objtype,
                      GVariant           **out_packed_object,
                      GCancellable        *cancellable,
                      GError             **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *object_path = NULL;
  ot_lvariant GVariant *metadata_v = NULL;
  ot_lvariant GVariant *ret_packed_object = NULL;

  object_path = ostree_repo_get_object_path (data->repo, checksum, objtype);

  if (!ot_util_variant_map (object_path, ostree_metadata_variant_type (objtype),
                            &metadata_v, error))
    goto out;

  ret_packed_object = g_variant_new ("(y@ayv)", (guchar) objtype,
                                     ostree_checksum_to_bytes_v (checksum),
                                     metadata_v);
      
  ret = TRUE;
  ot_transfer_out_value (out_packed_object, &ret_packed_object);
 out:
  return ret;
}

static gboolean
pack_one_data_object (OtRepackData        *data,
                      const char          *checksum,
                      OstreeObjectType     objtype,
                      guint64              expected_objsize,
                      GVariant           **out_packed_object,
                      GCancellable        *cancellable,
                      GError             **error)
{
  gboolean ret = FALSE;
  guchar entry_flags = 0;
  GInputStream *read_object_in; /* nofree */
  ot_lobj GFile *object_path = NULL;
  ot_lobj GInputStream *input = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lvariant GVariant *xattrs = NULL;
  ot_lobj GMemoryOutputStream *object_data_stream = NULL;
  ot_lobj GConverter *compressor = NULL;
  ot_lobj GConverterInputStream *compressed_object_input = NULL;
  ot_lvariant GVariant *file_header = NULL;
  ot_lvariant GVariant *ret_packed_object = NULL;

  switch (data->int_compression)
    {
    case OT_COMPRESSION_GZIP:
      {
        entry_flags |= OSTREE_PACK_FILE_ENTRY_FLAG_GZIP;
        break;
      }
    default:
      {
        g_assert_not_reached ();
      }
    }

  object_path = ostree_repo_get_object_path (data->repo, checksum, objtype);

  if (!ostree_repo_load_file (data->repo, checksum, &input, &file_info, &xattrs,
                              cancellable, error))
    goto out;

  file_header = ostree_file_header_new (file_info, xattrs);
      
  object_data_stream = (GMemoryOutputStream*)g_memory_output_stream_new (NULL, 0, g_realloc, g_free);
  if (input != NULL)
    {
      if (entry_flags & OSTREE_PACK_FILE_ENTRY_FLAG_GZIP)
        {
          compressor = (GConverter*)g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, OT_GZIP_COMPRESSION_LEVEL);
          compressed_object_input = (GConverterInputStream*)g_object_new (G_TYPE_CONVERTER_INPUT_STREAM,
                                                                          "converter", compressor,
                                                                          "base-stream", input,
                                                                          "close-base-stream", TRUE,
                                                                          NULL);
          read_object_in = (GInputStream*)compressed_object_input;
        }
      else
        {
          read_object_in = (GInputStream*)input;
        }

      if (!g_output_stream_splice ((GOutputStream*)object_data_stream, read_object_in,
                                   G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                   cancellable, error))
        goto out;
    }

  {
    guchar *data = g_memory_output_stream_get_data (object_data_stream);
    gsize data_len = g_memory_output_stream_get_data_size (object_data_stream);
    ret_packed_object = g_variant_new ("(@ayy@(uuuusa(ayay))@ay)",
                                       ostree_checksum_to_bytes_v (checksum),
                                       entry_flags,
                                       file_header,
                                       ot_gvariant_new_bytearray (data, data_len));
  }

  ret = TRUE;
  ot_transfer_out_value (out_packed_object, &ret_packed_object);
 out:
  return ret;
}

static gboolean
create_pack_file (OtRepackData        *data,
                  gboolean             is_meta,
                  GPtrArray           *objects,
                  GCancellable        *cancellable,
                  GError             **error)
{
  gboolean ret = FALSE;
  guint i;
  guint64 offset;
  gsize bytes_written;
  ot_lobj GFile *pack_dir = NULL;
  ot_lobj GFile *index_temppath = NULL;
  ot_lobj GOutputStream *index_out = NULL;
  ot_lobj GFile *pack_temppath = NULL;
  ot_lobj GOutputStream *pack_out = NULL;
  ot_lptrarray GPtrArray *index_content_list = NULL;
  ot_lvariant GVariant *pack_header = NULL;
  ot_lvariant GVariant *index_content = NULL;
  ot_lfree char *pack_name = NULL;
  ot_lobj GFile *pack_file_path = NULL;
  ot_lobj GFile *pack_index_path = NULL;
  GVariantBuilder index_content_builder;
  GChecksum *pack_checksum = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (!ostree_create_temp_regular_file (ostree_repo_get_tmpdir (data->repo),
                                        "pack-index", NULL,
                                        &index_temppath,
                                        &index_out,
                                        cancellable, error))
    goto out;
  
  if (!ostree_create_temp_regular_file (ostree_repo_get_tmpdir (data->repo),
                                        "pack-content", NULL,
                                        &pack_temppath,
                                        &pack_out,
                                        cancellable, error))
    goto out;

  index_content_list = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  offset = 0;
  pack_checksum = g_checksum_new (G_CHECKSUM_SHA256);

  pack_header = g_variant_new ("(s@a{sv}t)",
                               is_meta ? "OSTv0PACKMETAFILE" : "OSTv0PACKDATAFILE",
                               g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0),
                               (guint64)objects->len);

  if (!ostree_write_variant_with_size (pack_out, pack_header, offset, &bytes_written, pack_checksum,
                                       cancellable, error))
    goto out;
  offset += bytes_written;
  
  for (i = 0; i < objects->len; i++)
    {
      GVariant *object_data = objects->pdata[i];
      const char *checksum;
      guint32 objtype_u32;
      OstreeObjectType objtype;
      guint64 expected_objsize;
      ot_lvariant GVariant *packed_object = NULL;
      ot_lvariant GVariant *index_entry = NULL;

      g_variant_get (object_data, "(&sut)", &checksum, &objtype_u32, &expected_objsize);
                     
      objtype = (OstreeObjectType) objtype_u32;

      if (is_meta)
        {
          if (!pack_one_meta_object (data, checksum, objtype, &packed_object,
                                     cancellable, error))
            goto out;
        }
      else
        {
          if (!pack_one_data_object (data, checksum, objtype, expected_objsize,
                                     &packed_object, cancellable, error))
            goto out;
        }

      if (!write_padding (pack_out, 4, pack_checksum, &offset, cancellable, error))
        goto out;

      /* offset points to aligned header size */
      index_entry = g_variant_new ("(y@ayt)",
                                   (guchar)objtype,
                                   ostree_checksum_to_bytes_v (checksum),
                                   GUINT64_TO_BE (offset));
      g_ptr_array_add (index_content_list, g_variant_ref_sink (index_entry));
      index_entry = NULL;
      
      bytes_written = 0;
      if (!ostree_write_variant_with_size (pack_out, packed_object, offset, &bytes_written, 
                                           pack_checksum, cancellable, error))
        goto out;
      offset += bytes_written;
    }
  
  if (!g_output_stream_close (pack_out, cancellable, error))
    goto out;

  g_variant_builder_init (&index_content_builder, G_VARIANT_TYPE ("a(yayt)"));
  g_ptr_array_sort (index_content_list, compare_index_content);
  for (i = 0; i < index_content_list->len; i++)
    {
      GVariant *index_item = index_content_list->pdata[i];
      g_variant_builder_add_value (&index_content_builder, index_item);
    }
  index_content = g_variant_new ("(s@a{sv}@a(yayt))",
                                 "OSTv0PACKINDEX",
                                 g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0),
                                 g_variant_builder_end (&index_content_builder));

  if (!g_output_stream_write_all (index_out,
                                  g_variant_get_data (index_content),
                                  g_variant_get_size (index_content),
                                  &bytes_written,
                                  cancellable,
                                  error))
    goto out;

  if (!g_output_stream_close (index_out, cancellable, error))
    goto out;

  if (!ostree_repo_add_pack_file (data->repo,
                                  g_checksum_get_string (pack_checksum),
                                  is_meta,
                                  index_temppath,
                                  pack_temppath,
                                  cancellable,
                                  error))
    goto out;

  if (!ostree_repo_regenerate_pack_index (data->repo, cancellable, error))
    goto out;

  g_print ("Created pack file '%s' with %u objects\n", g_checksum_get_string (pack_checksum), objects->len);

  if (!opt_keep_loose)
    {
      for (i = 0; i < objects->len; i++)
        {
          GVariant *object_data = objects->pdata[i];
          const char *checksum;
          guint32 objtype_u32;
          OstreeObjectType objtype;
          guint64 expected_objsize;

          g_variant_get (object_data, "(&sut)", &checksum, &objtype_u32, &expected_objsize);
          
          objtype = (OstreeObjectType) objtype_u32;

          if (!delete_loose_object (data, checksum, objtype, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  if (index_temppath)
    (void) unlink (ot_gfile_get_path_cached (index_temppath));
  if (pack_temppath)
    (void) unlink (ot_gfile_get_path_cached (pack_temppath));
  if (pack_checksum)
    g_checksum_free (pack_checksum);
  return ret;
}

static void
cluster_one_object_chain (OtRepackData     *data,
                          GPtrArray        *object_list,
                          GPtrArray        *inout_clusters)
{
  guint i;
  guint64 current_size;
  guint current_offset;

  current_size = 0;
  current_offset = 0;
  for (i = 0; i < object_list->len; i++)
    { 
      GVariant *objdata = object_list->pdata[i];
      guint64 objsize;

      g_variant_get_child (objdata, 2, "t", &objsize);

      if (current_size + objsize > data->pack_size || i == (object_list->len - 1))
        {
          guint j;
          GPtrArray *current;

          if (current_offset < i)
            {
              current = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
              for (j = current_offset; j <= i; j++)
                {
                  g_ptr_array_add (current, g_variant_ref (object_list->pdata[j]));
                }
              g_ptr_array_add (inout_clusters, current);
              current_size = objsize;
              current_offset = i+1;
            }
        }
      else if (objsize > data->pack_size)
        {
          break;
        }
      else
        {
          current_size += objsize;
        }
    }
}

/**
 * cluster_objects_stupidly:
 * @objects: Map from serialized object name to objdata
 * @out_meta_clusters: (out): [Array of [Array of object data]].  Free with g_ptr_array_unref().
 * @out_data_clusters: (out): [Array of [Array of object data]].  Free with g_ptr_array_unref().
 *
 * Just sorts by size currently.  Also filters out non-regular object
 * content.
 */
static gboolean
cluster_objects_stupidly (OtRepackData      *data,
                          GHashTable        *objects,
                          GPtrArray        **out_meta_clusters,
                          GPtrArray        **out_data_clusters,
                          GCancellable      *cancellable,
                          GError           **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  ot_lptrarray GPtrArray *ret_meta_clusters = NULL;
  ot_lptrarray GPtrArray *ret_data_clusters = NULL;
  ot_lptrarray GPtrArray *meta_object_list = NULL;
  ot_lptrarray GPtrArray *data_object_list = NULL;
  ot_lobj GFile *object_path = NULL;
  ot_lobj GFileInfo *object_info = NULL;

  meta_object_list = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
  data_object_list = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  g_hash_table_iter_init (&hash_iter, objects);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;
      guint64 size;
      GVariant *v;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      g_clear_object (&object_path);
      object_path = ostree_repo_get_object_path (data->repo, checksum, objtype);

      g_clear_object (&object_info);
      object_info = g_file_query_info (object_path, OSTREE_GIO_FAST_QUERYINFO,
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       cancellable, error);
      if (!object_info)
        goto out;

      if (g_file_info_get_file_type (object_info) != G_FILE_TYPE_REGULAR)
        continue;

      size = g_file_info_get_attribute_uint64 (object_info, G_FILE_ATTRIBUTE_STANDARD_SIZE);

      v = g_variant_ref_sink (g_variant_new ("(sut)", checksum, (guint32)objtype, size));
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        g_ptr_array_add (meta_object_list, v);
      else
        g_ptr_array_add (data_object_list, v);
    }

  g_ptr_array_sort (meta_object_list, compare_object_data_by_size);
  g_ptr_array_sort (data_object_list, compare_object_data_by_size);

  ret_meta_clusters = g_ptr_array_new_with_free_func ((GDestroyNotify)g_ptr_array_unref);
  ret_data_clusters = g_ptr_array_new_with_free_func ((GDestroyNotify)g_ptr_array_unref);

  cluster_one_object_chain (data, meta_object_list, ret_meta_clusters);
  cluster_one_object_chain (data, data_object_list, ret_data_clusters);

  ret = TRUE;
  ot_transfer_out_value (out_meta_clusters, &ret_meta_clusters);
  ot_transfer_out_value (out_data_clusters, &ret_data_clusters);
 out:
  return ret;
}

static gboolean
parse_size_spec_with_suffix (const char *spec,
                             guint64     default_value,
                             guint64    *out_size,
                             GError    **error)
{
  gboolean ret = FALSE;
  char *endptr = NULL;
  guint64 ret_size;

  if (spec == NULL)
    {
      ret_size = default_value;
      endptr = NULL;
    }
  else
    {
      ret_size = g_ascii_strtoull (spec, &endptr, 10);
  
      if (endptr && *endptr)
        {
          char suffix = *endptr;
      
          switch (suffix)
            {
            case 'k':
            case 'K':
              {
                ret_size *= 1024;
                break;
              }
            case 'm':
            case 'M':
              {
                ret_size *= (1024 * 1024);
                break;
              }
            case 'g':
            case 'G':
              {
                ret_size *= (1024 * 1024 * 1024);
                break;
              }
            default:
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid size suffix '%c'", suffix);
              goto out;
            }
        }
    }

  ret = TRUE;
  *out_size = ret_size;
 out:
  return ret;
}

static gboolean
parse_compression_string (const char *compstr,
                          OtCompressionType *out_comptype,
                          GError           **error)
{
  gboolean ret = FALSE;
  OtCompressionType ret_comptype;
  
  if (compstr == NULL)
    ret_comptype = OT_COMPRESSION_NONE;
  else if (strcmp (compstr, "gzip") == 0)
    ret_comptype = OT_COMPRESSION_GZIP;
  else if (strcmp (compstr, "xz") == 0)
    ret_comptype = OT_COMPRESSION_XZ;
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid compression '%s'", compstr);
      goto out;
    }

  ret = TRUE;
  *out_comptype = ret_comptype;
 out:
  return ret;
}

static gboolean
do_stats_gather_loose (OtRepackData  *data,
                       GHashTable    *objects,
                       GHashTable   **out_loose,
                       GCancellable  *cancellable,
                       GError       **error)
{
  gboolean ret = FALSE;
  guint n_loose = 0;
  guint n_loose_and_packed = 0;
  guint n_packed = 0;
  guint n_dup_packed = 0;
  guint n_commits = 0;
  guint n_dirmeta = 0;
  guint n_dirtree = 0;
  guint n_files = 0;
  GHashTableIter hash_iter;
  gpointer key, value;
  ot_lhash GHashTable *ret_loose = NULL;

  ret_loose = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                     (GDestroyNotify) g_variant_unref,
                                     NULL);

  g_hash_table_iter_init (&hash_iter, objects);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      GVariant *objdata = value;
      const char *checksum;
      OstreeObjectType objtype;
      gboolean is_loose;
      gboolean is_packed;
      GVariant *pack_array;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      g_variant_get (objdata, "(b@as)", &is_loose, &pack_array);

      is_packed = g_variant_n_children (pack_array) > 0;
      
      if (is_loose && is_packed)
        {
          n_loose_and_packed++;
        }
      else if (is_loose)
        {
          GVariant *copy = g_variant_ref (serialized_key);
          g_hash_table_replace (ret_loose, copy, copy);
          n_loose++;
        }
      else if (g_variant_n_children (pack_array) > 1)
        {
          n_dup_packed++;
        }
      else
        {
          n_packed++;
        }
          
      switch (objtype)
        {
        case OSTREE_OBJECT_TYPE_COMMIT:
          n_commits++;
          break;
        case OSTREE_OBJECT_TYPE_DIR_TREE:
          n_dirtree++;
          break;
        case OSTREE_OBJECT_TYPE_DIR_META:
          n_dirmeta++;
          break;
        case OSTREE_OBJECT_TYPE_FILE:
          n_files++;
          break;
        default:
          g_assert_not_reached ();
        }
    }

  g_print ("Commits: %u\n", n_commits);
  g_print ("Tree contents: %u\n", n_dirtree);
  g_print ("Tree meta: %u\n", n_dirmeta);
  g_print ("Files: %u\n", n_files);
  g_print ("\n");
  g_print ("Loose+packed objects: %u\n", n_loose_and_packed);
  g_print ("Loose-only objects: %u\n", n_loose);
  g_print ("Duplicate packed objects: %u\n", n_dup_packed);
  g_print ("Packed-only objects: %u\n", n_packed);

  ret = TRUE;
  ot_transfer_out_value (out_loose, &ret_loose);
 /* out: */
  return ret;
}

static gboolean
do_incremental_pack (OtRepackData          *data,
                     GCancellable          *cancellable,
                     GError               **error)
{
  gboolean ret = FALSE;
  guint i;
  ot_lhash GHashTable *objects = NULL;
  ot_lptrarray GPtrArray *meta_clusters = NULL;
  ot_lptrarray GPtrArray *data_clusters = NULL;
  ot_lhash GHashTable *loose_objects = NULL;

  if (!ostree_repo_list_objects (data->repo, OSTREE_REPO_LIST_OBJECTS_ALL, &objects,
                                 cancellable, error))
    goto out;

  if (!do_stats_gather_loose (data, objects, &loose_objects, cancellable, error))
    goto out;

  g_print ("\n");
  g_print ("Using pack size: %" G_GUINT64_FORMAT "\n", data->pack_size);

  if (!cluster_objects_stupidly (data, loose_objects, &meta_clusters, &data_clusters,
                                 cancellable, error))
    goto out;
  
  if (meta_clusters->len > 0 || data_clusters->len > 0)
    g_print ("Going to create %u meta packfiles, %u data packfiles\n",
             meta_clusters->len, data_clusters->len);
  else
    g_print ("Nothing to do\n");

  if (!opt_analyze_only)
    {
      for (i = 0; i < meta_clusters->len; i++)
        {
          GPtrArray *cluster = meta_clusters->pdata[i];
          
          if (!create_pack_file (data, TRUE, cluster, cancellable, error))
            goto out;
        }
      for (i = 0; i < data_clusters->len; i++)
        {
          GPtrArray *cluster = data_clusters->pdata[i];
          
          if (!create_pack_file (data, FALSE, cluster, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_builtin_pack (int argc, char **argv, GFile *repo_path, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  GCancellable *cancellable = NULL;
  OtRepackData data;
  ot_lobj OstreeRepo *repo = NULL;

  memset (&data, 0, sizeof (data));

  context = g_option_context_new ("- Recompress objects");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (ostree_repo_get_mode (repo) != OSTREE_REPO_MODE_ARCHIVE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Can't repack bare repositories yet");
      goto out;
    }

  data.repo = repo;
  data.error = error;

  if (!parse_size_spec_with_suffix (opt_pack_size, OT_DEFAULT_PACK_SIZE_BYTES, &data.pack_size, error))
    goto out;
  /* Default internal compression to gzip */
  if (!parse_compression_string (opt_int_compression ? opt_int_compression : "gzip", &data.int_compression, error))
    goto out;
  if (!parse_compression_string (opt_ext_compression, &data.ext_compression, error))
    goto out;

  if (opt_reindex_only)
    {
      if (!ostree_repo_regenerate_pack_index (repo, cancellable, error))
        goto out;
    }
  else
    {
      if (!do_incremental_pack (&data, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
