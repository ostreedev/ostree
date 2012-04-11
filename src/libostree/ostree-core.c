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

#include "config.h"

#include "ostree.h"
#include "otutil.h"

#include <sys/types.h>
#include <attr/xattr.h>

#define ALIGN_VALUE(this, boundary) \
  (( ((unsigned long)(this)) + (((unsigned long)(boundary)) -1)) & (~(((unsigned long)(boundary))-1)))

const GVariantType *
ostree_metadata_variant_type (OstreeObjectType objtype)
{
  switch (objtype)
    {
    case OSTREE_OBJECT_TYPE_DIR_TREE:
      return OSTREE_TREE_GVARIANT_FORMAT;
    case OSTREE_OBJECT_TYPE_DIR_META:
      return OSTREE_DIRMETA_GVARIANT_FORMAT;
    case OSTREE_OBJECT_TYPE_COMMIT:
      return OSTREE_COMMIT_GVARIANT_FORMAT;
    default:
      g_assert_not_reached ();
    }
}

gboolean
ostree_validate_checksum_string (const char *sha256,
                                 GError    **error)
{
  return ostree_validate_structureof_checksum_string (sha256, error);
}

gboolean
ostree_validate_rev (const char *rev,
                     GError **error)
{
  gboolean ret = FALSE;
  ot_lptrarray GPtrArray *components = NULL;

  if (!ot_util_path_split_validate (rev, &components, error))
    goto out;

  if (components->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid empty rev");
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static char *
canonicalize_xattrs (char *xattr_string, size_t len)
{
  char *p;
  GSList *xattrs = NULL;
  GSList *iter;
  GString *result;

  result = g_string_new (0);

  p = xattr_string;
  while (p < xattr_string+len)
    {
      xattrs = g_slist_prepend (xattrs, p);
      p += strlen (p) + 1;
    }

  xattrs = g_slist_sort (xattrs, (GCompareFunc) strcmp);
  for (iter = xattrs; iter; iter = iter->next)
    g_string_append (result, iter->data);

  g_slist_free (xattrs);
  return g_string_free (result, FALSE);
}

static gboolean
read_xattr_name_array (const char *path,
                       const char *xattrs,
                       size_t      len,
                       GVariantBuilder *builder,
                       GError  **error)
{
  gboolean ret = FALSE;
  const char *p;

  p = xattrs;
  while (p < xattrs+len)
    {
      ssize_t bytes_read;
      char *buf;

      bytes_read = lgetxattr (path, p, NULL, 0);
      if (bytes_read < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
      if (bytes_read == 0)
        continue;

      buf = g_malloc (bytes_read);
      if (lgetxattr (path, p, buf, bytes_read) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          g_free (buf);
          goto out;
        }
      
      g_variant_builder_add (builder, "(@ay@ay)",
                             g_variant_new_bytestring (p),
                             g_variant_new_from_data (G_VARIANT_TYPE ("ay"),
                                                      buf, bytes_read, FALSE, g_free, buf));

      p = p + strlen (p) + 1;
    }
  
  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_get_xattrs_for_file (GFile         *f,
                            GVariant     **out_xattrs,
                            GCancellable  *cancellable,
                            GError       **error)
{
  gboolean ret = FALSE;
  const char *path;
  ssize_t bytes_read;
  ot_lvariant GVariant *ret_xattrs = NULL;
  ot_lfree char *xattr_names = NULL;
  ot_lfree char *xattr_names_canonical = NULL;
  GVariantBuilder builder;
  gboolean builder_initialized = FALSE;

  path = ot_gfile_get_path_cached (f);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ayay)"));
  builder_initialized = TRUE;

  bytes_read = llistxattr (path, NULL, 0);

  if (bytes_read < 0)
    {
      if (errno != ENOTSUP)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else if (bytes_read > 0)
    {
      xattr_names = g_malloc (bytes_read);
      if (llistxattr (path, xattr_names, bytes_read) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
      xattr_names_canonical = canonicalize_xattrs (xattr_names, bytes_read);
      
      if (!read_xattr_name_array (path, xattr_names_canonical, bytes_read, &builder, error))
        goto out;
    }

  ret_xattrs = g_variant_builder_end (&builder);
  g_variant_ref_sink (ret_xattrs);
  
  ret = TRUE;
  ot_transfer_out_value (out_xattrs, &ret_xattrs);
 out:
  if (!builder_initialized)
    g_variant_builder_clear (&builder);
  return ret;
}

GVariant *
ostree_file_header_new (GFileInfo         *file_info,
                        GVariant          *xattrs)
{
  guint32 uid;
  guint32 gid;
  guint32 mode;
  guint32 rdev;
  const char *symlink_target;
  GVariant *ret;
  ot_lvariant GVariant *tmp_xattrs = NULL;

  uid = g_file_info_get_attribute_uint32 (file_info, "unix::uid");
  gid = g_file_info_get_attribute_uint32 (file_info, "unix::gid");
  mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
  rdev = g_file_info_get_attribute_uint32 (file_info, "unix::rdev");

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_SYMBOLIC_LINK)
    symlink_target = g_file_info_get_symlink_target (file_info);
  else
    symlink_target = "";

  if (xattrs == NULL)
    tmp_xattrs = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("(ayay)"), NULL, 0));

  ret = g_variant_new ("(uuuus@a(ayay))", GUINT32_TO_BE (uid),
                       GUINT32_TO_BE (gid), GUINT32_TO_BE (mode), GUINT32_TO_BE (rdev),
                       symlink_target, xattrs ? xattrs : tmp_xattrs);
  g_variant_ref_sink (ret);
  return ret;
}

static gboolean
write_padding (GOutputStream    *output,
               guint             alignment,
               gsize             offset,
               gsize            *out_bytes_written,
               GChecksum        *checksum,
               GCancellable     *cancellable,
               GError          **error)
{
  gboolean ret = FALSE;
  guint bits;
  guint padding_len;
  guchar padding_nuls[8] = {0, 0, 0, 0, 0, 0, 0, 0};

  if (alignment == 8)
    bits = ((offset) & 7);
  else
    bits = ((offset) & 3);

  if (bits > 0)
    {
      padding_len = alignment - bits;
      if (!ot_gio_write_update_checksum (output, (guchar*)padding_nuls, padding_len,
                                         out_bytes_written, checksum,
                                         cancellable, error))
        goto out;
    }
  
  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_write_variant_with_size (GOutputStream      *output,
                                GVariant           *variant,
                                guint64             alignment_offset,
                                gsize              *out_bytes_written,
                                GChecksum          *checksum,
                                GCancellable       *cancellable,
                                GError            **error)
{
  gboolean ret = FALSE;
  guint64 variant_size;
  guint32 variant_size_u32_be;
  gsize bytes_written;
  gsize ret_bytes_written = 0;

  /* Write variant size */
  variant_size = g_variant_get_size (variant);
  g_assert (variant_size < G_MAXUINT32);
  variant_size_u32_be = GUINT32_TO_BE((guint32) variant_size);

  bytes_written = 0;
  if (!ot_gio_write_update_checksum (output, &variant_size_u32_be, 4,
                                     &bytes_written, checksum,
                                     cancellable, error))
    goto out;
  ret_bytes_written += bytes_written;
  alignment_offset += bytes_written;

  bytes_written = 0;
  /* Pad to offset of 8, write variant */
  if (!write_padding (output, 8, alignment_offset, &bytes_written, checksum,
                      cancellable, error))
    goto out;
  ret_bytes_written += bytes_written;

  bytes_written = 0;
  if (!ot_gio_write_update_checksum (output, g_variant_get_data (variant),
                                     variant_size, &bytes_written, checksum,
                                     cancellable, error))
    goto out;
  ret_bytes_written += bytes_written;

  ret = TRUE;
  if (out_bytes_written)
    *out_bytes_written = ret_bytes_written;
 out:
  return ret;
}

gboolean
ostree_write_file_header_update_checksum (GOutputStream         *out,
                                          GVariant              *header,
                                          GChecksum             *checksum,
                                          GCancellable          *cancellable,
                                          GError               **error)
{
  gboolean ret = FALSE;
  gsize bytes_written;

  if (!ostree_write_variant_with_size (out, header, 0, &bytes_written, checksum,
                                       cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_raw_file_to_content_stream (GInputStream       *input,
                                   GFileInfo          *file_info,
                                   GVariant           *xattrs,
                                   GInputStream      **out_input,
                                   GCancellable       *cancellable,
                                   GError            **error)
{
  gboolean ret = FALSE;
  gpointer header_data;
  gsize header_size;
  ot_lobj GInputStream *ret_input = NULL;
  ot_lvariant GVariant *file_header = NULL;
  ot_lptrarray GPtrArray *streams = NULL;
  ot_lobj GOutputStream *header_out_stream = NULL;
  ot_lobj GInputStream *header_in_stream = NULL;

  file_header = ostree_file_header_new (file_info, xattrs);

  header_out_stream = g_memory_output_stream_new (NULL, 0, g_realloc, g_free);

  if (!ostree_write_variant_with_size (header_out_stream, file_header, 0, NULL, NULL,
                                       cancellable, error))
    goto out;

  header_size = g_memory_output_stream_get_data_size ((GMemoryOutputStream*) header_out_stream);
  header_data = g_memory_output_stream_steal_data ((GMemoryOutputStream*) header_out_stream);
  header_in_stream = g_memory_input_stream_new_from_data (header_data, header_size, g_free);

  streams = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  g_ptr_array_add (streams, g_object_ref (header_in_stream));
  g_ptr_array_add (streams, g_object_ref (input));
  
  ret_input = (GInputStream*)ostree_chain_input_stream_new (streams);

  ret = TRUE;
  ot_transfer_out_value (out_input, &ret_input);
 out:
  return ret;
}

gboolean
ostree_content_file_parse (GFile                  *content_path,
                           gboolean                trusted,
                           GInputStream          **out_input,
                           GFileInfo             **out_file_info,
                           GVariant              **out_xattrs,
                           GCancellable           *cancellable,
                           GError                **error)
{
  gboolean ret = FALSE;
  guint64 length;
  guint32 archive_header_size;
  guchar dummy[4];
  gsize bytes_read;
  ot_lobj GInputStream *ret_input = NULL;
  ot_lobj GFileInfo *content_file_info = NULL;
  ot_lobj GFileInfo *ret_file_info = NULL;
  ot_lvariant GVariant *ret_xattrs = NULL;
  ot_lvariant GVariant *file_header = NULL;
  ot_lfree guchar *buf = NULL;

  ret_input = (GInputStream*)g_file_read (content_path, cancellable, error);
  if (!ret_input)
    goto out;

  content_file_info = g_file_input_stream_query_info ((GFileInputStream*)ret_input,
                                                      OSTREE_GIO_FAST_QUERYINFO,
                                                      cancellable, error);
  if (!content_file_info)
    goto out;

  length = g_file_info_get_size (content_file_info);

  if (!g_input_stream_read_all (ret_input,
                                &archive_header_size, 4, &bytes_read,
                                cancellable, error))
    goto out;
  archive_header_size = GUINT32_FROM_BE (archive_header_size);
  if (archive_header_size > length)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "File header size %u exceeds size %" G_GUINT64_FORMAT,
                   (guint)archive_header_size, length);
      goto out;
    }

  /* Skip over padding */
  if (!g_input_stream_read_all (ret_input,
                                dummy, 4, &bytes_read,
                                cancellable, error))
    goto out;

  buf = g_malloc (archive_header_size);
  if (!g_input_stream_read_all (ret_input, buf, archive_header_size, &bytes_read,
                                cancellable, error))
    goto out;
  file_header = g_variant_new_from_data (OSTREE_FILE_HEADER_GVARIANT_FORMAT,
                                         buf, archive_header_size, trusted,
                                         g_free, buf);
  buf = NULL;

  if (!ostree_file_header_parse (file_header,
                                 out_file_info ? &ret_file_info : NULL,
                                 out_xattrs ? &ret_xattrs : NULL,
                                 error))
    goto out;
  if (ret_file_info)
    g_file_info_set_size (ret_file_info, length - archive_header_size - 8);
  
  if (g_file_info_get_file_type (ret_file_info) != G_FILE_TYPE_REGULAR)
    {
      g_clear_object (&ret_input);
    }

  /* Now give the input stream at its current position as return value */

  ret = TRUE;
  ot_transfer_out_value (out_input, &ret_input);
  ot_transfer_out_value (out_file_info, &ret_file_info);
  ot_transfer_out_value (out_xattrs, &ret_xattrs);
 out:
  return ret;
}

gboolean
ostree_checksum_file_from_input (GFileInfo        *file_info,
                                 GVariant         *xattrs,
                                 GInputStream     *in,
                                 OstreeObjectType  objtype,
                                 guchar          **out_csum,
                                 GCancellable     *cancellable,
                                 GError          **error)
{
  gboolean ret = FALSE;
  ot_lfree guchar *ret_csum = NULL;
  GChecksum *checksum = NULL;

  checksum = g_checksum_new (G_CHECKSUM_SHA256);

  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    {
      if (!ot_gio_splice_update_checksum (NULL, in, checksum, cancellable, error))
        goto out;
    }
  else if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
    {
      ot_lvariant GVariant *dirmeta = ostree_create_directory_metadata (file_info, xattrs);
      g_checksum_update (checksum, g_variant_get_data (dirmeta),
                         g_variant_get_size (dirmeta));
      
    }
  else
    {
      ot_lvariant GVariant *file_header = NULL;

      file_header = ostree_file_header_new (file_info, xattrs);

      if (!ostree_write_file_header_update_checksum (NULL, file_header, checksum,
                                                     cancellable, error))
        goto out;

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
        {
          if (!ot_gio_splice_update_checksum (NULL, in, checksum, cancellable, error))
            goto out;
        }
    }

  ret_csum = ot_csum_from_gchecksum (checksum);

  ret = TRUE;
  ot_transfer_out_value (out_csum, &ret_csum);
 out:
  ot_clear_checksum (&checksum);
  return ret;
}

gboolean
ostree_checksum_file (GFile            *f,
                      OstreeObjectType  objtype,
                      guchar          **out_csum,
                      GCancellable     *cancellable,
                      GError          **error)
{
  gboolean ret = FALSE;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GInputStream *in = NULL;
  ot_lvariant GVariant *xattrs = NULL;
  ot_lfree guchar *ret_csum = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  file_info = g_file_query_info (f, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!file_info)
    goto out;

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
      in = (GInputStream*)g_file_read (f, cancellable, error);
      if (!in)
        goto out;
    }

  if (objtype == OSTREE_OBJECT_TYPE_FILE)
    {
      if (!ostree_get_xattrs_for_file (f, &xattrs, cancellable, error))
        goto out;
    }

  if (!ostree_checksum_file_from_input (file_info, xattrs, in, objtype,
                                        &ret_csum, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_csum, &ret_csum);
 out:
  return ret;
}

typedef struct {
  GFile  *f;
  OstreeObjectType objtype;
  guchar *csum;
} ChecksumFileAsyncData;

static void
checksum_file_async_thread (GSimpleAsyncResult  *res,
                            GObject             *object,
                            GCancellable        *cancellable)
{
  GError *error = NULL;
  ChecksumFileAsyncData *data;
  guchar *csum = NULL;

  data = g_simple_async_result_get_op_res_gpointer (res);
  if (!ostree_checksum_file (data->f, data->objtype, &csum, cancellable, &error))
    g_simple_async_result_take_error (res, error);
  else
    data->csum = csum;
}

static void
checksum_file_async_data_free (gpointer datap)
{
  ChecksumFileAsyncData *data = datap;

  g_object_unref (data->f);
  g_free (data->csum);
  g_free (data);
}
  
void
ostree_checksum_file_async (GFile                 *f,
                            OstreeObjectType       objtype,
                            int                    io_priority,
                            GCancellable          *cancellable,
                            GAsyncReadyCallback    callback,
                            gpointer               user_data)
{
  GSimpleAsyncResult  *res;
  ChecksumFileAsyncData *data;

  data = g_new0 (ChecksumFileAsyncData, 1);
  data->f = g_object_ref (f);
  data->objtype = objtype;

  res = g_simple_async_result_new (G_OBJECT (f), callback, user_data, ostree_checksum_file_async);
  g_simple_async_result_set_op_res_gpointer (res, data, (GDestroyNotify)checksum_file_async_data_free);
  
  g_simple_async_result_run_in_thread (res, checksum_file_async_thread, io_priority, cancellable);
  g_object_unref (res);
}

gboolean
ostree_checksum_file_async_finish (GFile          *f,
                                   GAsyncResult   *result,
                                   guchar        **out_csum,
                                   GError        **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  ChecksumFileAsyncData *data;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == ostree_checksum_file_async);

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  data = g_simple_async_result_get_op_res_gpointer (simple);
  /* Transfer ownership */
  *out_csum = data->csum;
  data->csum = NULL;
  return TRUE;
}

GVariant *
ostree_create_directory_metadata (GFileInfo    *dir_info,
                                  GVariant     *xattrs)
{
  GVariant *ret_metadata = NULL;

  ret_metadata = g_variant_new ("(uuu@a(ayay))",
                                GUINT32_TO_BE (g_file_info_get_attribute_uint32 (dir_info, "unix::uid")),
                                GUINT32_TO_BE (g_file_info_get_attribute_uint32 (dir_info, "unix::gid")),
                                GUINT32_TO_BE (g_file_info_get_attribute_uint32 (dir_info, "unix::mode")),
                                xattrs ? xattrs : g_variant_new_array (G_VARIANT_TYPE ("(ayay)"), NULL, 0));
  g_variant_ref_sink (ret_metadata);

  return ret_metadata;
}

gboolean
ostree_set_xattrs (GFile  *f, 
                   GVariant *xattrs, 
                   GCancellable *cancellable, 
                   GError **error)
{
  const char *path;
  gboolean ret = FALSE;
  int i, n;

  path = ot_gfile_get_path_cached (f);

  n = g_variant_n_children (xattrs);
  for (i = 0; i < n; i++)
    {
      const guint8* name;
      GVariant *value;
      const guint8* value_data;
      gsize value_len;
      gboolean loop_err;

      g_variant_get_child (xattrs, i, "(^&ay@ay)",
                           &name, &value);
      value_data = g_variant_get_fixed_array (value, &value_len, 1);
      
      loop_err = lsetxattr (path, (char*)name, (char*)value_data, value_len, XATTR_REPLACE) < 0;
      ot_clear_gvariant (&value);
      if (loop_err)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

const char *
ostree_object_type_to_string (OstreeObjectType objtype)
{
  switch (objtype)
    {
    case OSTREE_OBJECT_TYPE_FILE:
      return "file";
    case OSTREE_OBJECT_TYPE_DIR_TREE:
      return "dirtree";
    case OSTREE_OBJECT_TYPE_DIR_META:
      return "dirmeta";
    case OSTREE_OBJECT_TYPE_COMMIT:
      return "commit";
    default:
      g_assert_not_reached ();
      return NULL;
    }
}

OstreeObjectType
ostree_object_type_from_string (const char *str)
{
  if (!strcmp (str, "file"))
    return OSTREE_OBJECT_TYPE_FILE;
  else if (!strcmp (str, "dirtree"))
    return OSTREE_OBJECT_TYPE_DIR_TREE;
  else if (!strcmp (str, "dirmeta"))
    return OSTREE_OBJECT_TYPE_DIR_META;
  else if (!strcmp (str, "commit"))
    return OSTREE_OBJECT_TYPE_COMMIT;
  g_assert_not_reached ();
  return 0;
}

char *
ostree_object_to_string (const char *checksum,
                         OstreeObjectType objtype)
{
  return g_strconcat (checksum, ".", ostree_object_type_to_string (objtype), NULL);
}

void
ostree_object_from_string (const char *str,
                           gchar     **out_checksum,
                           OstreeObjectType *out_objtype)
{
  const char *dot;

  dot = strrchr (str, '.');
  g_assert (dot != NULL);
  *out_checksum = g_strndup (str, dot - str);
  *out_objtype = ostree_object_type_from_string (dot + 1);
}

guint
ostree_hash_object_name (gconstpointer a)
{
  GVariant *variant = (gpointer)a;
  const char *checksum;
  OstreeObjectType objtype;
  gint objtype_int;
  
  ostree_object_name_deserialize (variant, &checksum, &objtype);
  objtype_int = (gint) objtype;
  return g_str_hash (checksum) + g_int_hash (&objtype_int);
}

int
ostree_cmp_checksum_bytes (const guchar *a,
                           const guchar *b)
{
  return memcmp (a, b, 32);
}

GVariant *
ostree_object_name_serialize (const char *checksum,
                              OstreeObjectType objtype)
{
  return g_variant_new ("(su)", checksum, (guint32)objtype);
}

void
ostree_object_name_deserialize (GVariant         *variant,
                                const char      **out_checksum,
                                OstreeObjectType *out_objtype)
{
  guint32 objtype_u32;
  g_variant_get (variant, "(&su)", out_checksum, &objtype_u32);
  *out_objtype = (OstreeObjectType)objtype_u32;
}

static void
checksum_to_bytes (const char *checksum,
                   guchar     *buf)
{
  guint i;
  guint j;

  for (i = 0, j = 0; i < 32; i += 1, j += 2)
    {
      gint big, little;

      g_assert (checksum[j]);
      g_assert (checksum[j+1]);

      big = g_ascii_xdigit_value (checksum[j]);
      little = g_ascii_xdigit_value (checksum[j+1]);

      g_assert (big != -1);
      g_assert (little != -1);

      buf[i] = (big << 4) | little;
    }
}

guchar *
ostree_checksum_to_bytes (const char *checksum)
{
  guchar *ret = g_malloc (32);
  checksum_to_bytes (checksum, ret);
  return ret;
}

GVariant *
ostree_checksum_to_bytes_v (const char *checksum)
{
  guchar result[32];
  checksum_to_bytes (checksum, result);
  return ot_gvariant_new_bytearray ((guchar*)result, 32);
}

char *
ostree_checksum_from_bytes (const guchar *csum)
{
  static const gchar hexchars[] = "0123456789abcdef";
  char *ret;
  guint i, j;

  ret = g_malloc (65);
  
  for (i = 0, j = 0; i < 32; i++, j += 2)
    {
      guchar byte = csum[i];
      ret[j] = hexchars[byte >> 4];
      ret[j+1] = hexchars[byte & 0xF];
    }
  ret[j] = '\0';

  return ret;
}

char *
ostree_checksum_from_bytes_v (GVariant *csum_bytes)
{
  return ostree_checksum_from_bytes (ostree_checksum_bytes_peek (csum_bytes));
}

const guchar *
ostree_checksum_bytes_peek (GVariant *bytes)
{
  gsize n_elts;
  return g_variant_get_fixed_array (bytes, &n_elts, 1);
}

char *
ostree_get_relative_object_path (const char *checksum,
                                 OstreeObjectType type)
{
  GString *path;

  g_assert (strlen (checksum) == 64);

  path = g_string_new ("objects/");

  g_string_append_len (path, checksum, 2);
  g_string_append_c (path, '/');
  g_string_append (path, checksum + 2);
  g_string_append_c (path, '.');
  g_string_append (path, ostree_object_type_to_string (type));

  return g_string_free (path, FALSE);
}

static char *
get_pack_name (gboolean        is_meta,
               gboolean        is_index,
               const char     *prefix,
               const char     *checksum)
{
  GString *path;

  g_assert (strlen (checksum) == 64);

  path = g_string_new (prefix);
  if (is_meta)
    g_string_append (path, "ostmetapack-");
  else
    g_string_append (path, "ostdatapack-");
  g_string_append (path, checksum);
  if (is_index)
    g_string_append (path, ".index");
  else
    g_string_append (path, ".data");

  return g_string_free (path, FALSE);
}

char *
ostree_get_pack_index_name (gboolean        is_meta,
                            const char     *checksum)
{
  return get_pack_name (is_meta, TRUE, "", checksum);
}

char *
ostree_get_pack_data_name (gboolean        is_meta,
                           const char     *checksum)
{
  return get_pack_name (is_meta, FALSE, "", checksum);
}

char *
ostree_get_relative_pack_index_path (gboolean        is_meta,
                                     const char     *checksum)
{
  return get_pack_name (is_meta, TRUE, "objects/pack/", checksum);
}

char *
ostree_get_relative_pack_data_path (gboolean        is_meta,
                                    const char     *checksum)
{
  return get_pack_name (is_meta, FALSE, "objects/pack/", checksum);
}

gboolean
ostree_file_header_parse (GVariant         *metadata,
                          GFileInfo       **out_file_info,
                          GVariant        **out_xattrs,
                          GError          **error)
{
  gboolean ret = FALSE;
  guint32 uid, gid, mode, rdev;
  const char *symlink_target;
  ot_lobj GFileInfo *ret_file_info = NULL;
  ot_lvariant GVariant *ret_xattrs = NULL;

  g_variant_get (metadata, "(uuuu&s@a(ayay))",
                 &uid, &gid, &mode, &rdev,
                 &symlink_target, &ret_xattrs);

  uid = GUINT32_FROM_BE (uid);
  gid = GUINT32_FROM_BE (gid);
  mode = GUINT32_FROM_BE (mode);
  rdev = GUINT32_FROM_BE (rdev);

  ret_file_info = g_file_info_new ();
  g_file_info_set_attribute_uint32 (ret_file_info, "standard::type", ot_gfile_type_for_mode (mode));
  g_file_info_set_attribute_boolean (ret_file_info, "standard::is-symlink", S_ISLNK (mode));
  g_file_info_set_attribute_uint32 (ret_file_info, "unix::uid", uid);
  g_file_info_set_attribute_uint32 (ret_file_info, "unix::gid", gid);
  g_file_info_set_attribute_uint32 (ret_file_info, "unix::mode", mode);

  if (S_ISREG (mode))
    {
      ;
    }
  else if (S_ISLNK (mode))
    {
      g_file_info_set_attribute_byte_string (ret_file_info, "standard::symlink-target", symlink_target);
    }
  else if (S_ISCHR (mode) || S_ISBLK (mode))
    {
      g_file_info_set_attribute_uint32 (ret_file_info, "unix::rdev", rdev);
    }
  else if (S_ISFIFO (mode))
    {
      ;
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted archive file; invalid mode %u", mode);
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value(out_file_info, &ret_file_info);
  ot_transfer_out_value(out_xattrs, &ret_xattrs);
 out:
  return ret;
}

gboolean
ostree_create_file_from_input (GFile            *dest_file,
                               GFileInfo        *finfo,
                               GVariant         *xattrs,
                               GInputStream     *input,
                               GCancellable     *cancellable,
                               GError          **error)
{
  gboolean ret = FALSE;
  const char *dest_path;
  guint32 uid, gid, mode;
  ot_lobj GFileOutputStream *out = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (finfo != NULL)
    {
      mode = g_file_info_get_attribute_uint32 (finfo, "unix::mode");
    }
  else
    {
      mode = S_IFREG | 0664;
    }
  dest_path = ot_gfile_get_path_cached (dest_file);

  if (S_ISDIR (mode))
    {
      if (mkdir (ot_gfile_get_path_cached (dest_file), mode) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else if (S_ISREG (mode))
    {
      out = g_file_create (dest_file, 0, cancellable, error);
      if (!out)
        goto out;

      if (input)
        {
          if (g_output_stream_splice ((GOutputStream*)out, input, 0,
                                      cancellable, error) < 0)
            goto out;
        }

      if (!g_output_stream_close ((GOutputStream*)out, NULL, error))
        goto out;
    }
  else if (S_ISLNK (mode))
    {
      const char *target = g_file_info_get_attribute_byte_string (finfo, "standard::symlink-target");
      if (symlink (target, dest_path) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else if (S_ISCHR (mode) || S_ISBLK (mode))
    {
      guint32 dev = g_file_info_get_attribute_uint32 (finfo, "unix::rdev");
      if (mknod (dest_path, mode, dev) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else if (S_ISFIFO (mode))
    {
      if (mkfifo (dest_path, mode) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid mode %u", mode);
      goto out;
    }

  if (finfo != NULL)
    {
      uid = g_file_info_get_attribute_uint32 (finfo, "unix::uid");
      gid = g_file_info_get_attribute_uint32 (finfo, "unix::gid");
      
      if (lchown (dest_path, uid, gid) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  if (!S_ISLNK (mode))
    {
      if (chmod (dest_path, mode) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  if (xattrs != NULL)
    {
      if (!ostree_set_xattrs (dest_file, xattrs, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  if (!ret && !S_ISDIR(mode))
    {
      (void) unlink (dest_path);
    }
  return ret;
}

static GString *
create_tmp_string (const char *dirpath,
                   const char *prefix,
                   const char *suffix)
{
  GString *tmp_name = NULL;

  if (!prefix)
    prefix = "tmp";
  if (!suffix)
    suffix = "tmp";

  tmp_name = g_string_new (dirpath);
  g_string_append_c (tmp_name, '/');
  g_string_append (tmp_name, prefix);
  g_string_append (tmp_name, "-XXXXXXXXXXXX.");
  g_string_append (tmp_name, suffix);

  return tmp_name;
}

static char *
subst_xxxxxx (const char *string)
{
  static const char table[] = "ABCEDEFGHIJKLMNOPQRSTUVWXYZabcedefghijklmnopqrstuvwxyz0123456789";
  char *ret = g_strdup (string);
  guint8 *xxxxxx = (guint8*)strstr (ret, "XXXXXX");

  g_assert (xxxxxx != NULL);

  while (*xxxxxx == 'X')
    {
      int offset = g_random_int_range (0, sizeof (table) - 1);
      *xxxxxx = (guint8)table[offset];
      xxxxxx++;
    }

  return ret;
}

gboolean
ostree_create_temp_file_from_input (GFile            *dir,
                                    const char       *prefix,
                                    const char       *suffix,
                                    GFileInfo        *finfo,
                                    GVariant         *xattrs,
                                    GInputStream     *input,
                                    GFile           **out_file,
                                    GCancellable     *cancellable,
                                    GError          **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  int i = 0;
  ot_lfree char *possible_name = NULL;
  ot_lobj GFile *possible_file = NULL;
  ot_lfree guchar *ret_csum = NULL;
  GString *tmp_name = NULL;

  tmp_name = create_tmp_string (ot_gfile_get_path_cached (dir),
                                prefix, suffix);
  
  /* 128 attempts seems reasonable... */
  for (i = 0; i < 128; i++)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        goto out;

      g_free (possible_name);
      possible_name = subst_xxxxxx (tmp_name->str);
      g_clear_object (&possible_file);
      possible_file = g_file_get_child (dir, possible_name);
      
      if (!ostree_create_file_from_input (possible_file, finfo, xattrs, input,
                                          cancellable, &temp_error))
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            {
              g_clear_error (&temp_error);
              continue;
            }
          else
            {
              g_propagate_error (error, temp_error);
              goto out;
            }
        }
      else
        {
          break;
        }
    }
  if (i >= 128)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exhausted 128 attempts to create a temporary file");
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value(out_file, &possible_file);
 out:
  if (tmp_name)
    g_string_free (tmp_name, TRUE);
  return ret;
}

gboolean
ostree_create_temp_regular_file (GFile            *dir,
                                 const char       *prefix,
                                 const char       *suffix,
                                 GFile           **out_file,
                                 GOutputStream   **out_stream,
                                 GCancellable     *cancellable,
                                 GError          **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *ret_file = NULL;
  ot_lobj GOutputStream *ret_stream = NULL;

  if (!ostree_create_temp_file_from_input (dir, prefix, suffix, NULL, NULL, NULL,
                                           &ret_file, cancellable, error))
    goto out;
  
  ret_stream = (GOutputStream*)g_file_append_to (ret_file, 0, cancellable, error);
  if (ret_stream == NULL)
    goto out;
  
  ret = TRUE;
  ot_transfer_out_value(out_file, &ret_file);
  ot_transfer_out_value(out_stream, &ret_stream);
 out:
  return ret;
}

gboolean
ostree_create_temp_hardlink (GFile            *dir,
                             GFile            *src,
                             const char       *prefix,
                             const char       *suffix,
                             GFile           **out_file,
                             GCancellable     *cancellable,
                             GError          **error)
{
  gboolean ret = FALSE;
  int i = 0;
  ot_lfree char *possible_name = NULL;
  ot_lobj GFile *possible_file = NULL;
  GString *tmp_name = NULL;

  tmp_name = create_tmp_string (ot_gfile_get_path_cached (dir),
                                prefix, suffix);
  
  /* 128 attempts seems reasonable... */
  for (i = 0; i < 128; i++)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        goto out;

      g_free (possible_name);
      possible_name = subst_xxxxxx (tmp_name->str);
      g_clear_object (&possible_file);
      possible_file = g_file_get_child (dir, possible_name);

      if (link (ot_gfile_get_path_cached (src), ot_gfile_get_path_cached (possible_file)) < 0)
        {
          if (errno == EEXIST)
            continue;
          else
            {
              ot_util_set_error_from_errno (error, errno);
              goto out;
            }
        }
      else
        {
          break;
        }
    }
  if (i >= 128)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exhausted 128 attempts to create a temporary file");
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value(out_file, &possible_file);
 out:
  if (tmp_name)
    g_string_free (tmp_name, TRUE);
  return ret;
}

gboolean
ostree_read_pack_entry_raw (guchar        *pack_data,
                            guint64        pack_len,
                            guint64        offset,
                            gboolean       trusted,
                            gboolean       is_meta,
                            GVariant     **out_entry,
                            GCancellable  *cancellable,
                            GError       **error)
{
  gboolean ret = FALSE;
  guint64 entry_start;
  guint64 entry_end;
  guint32 entry_len;
  ot_lvariant GVariant *ret_entry = NULL;

  if (G_UNLIKELY (!(offset <= pack_len)))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted pack index; out of range offset %" G_GUINT64_FORMAT,
                   offset);
      goto out;
    }
  if (G_UNLIKELY (!((offset & 0x3) == 0)))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted pack index; unaligned offset %" G_GUINT64_FORMAT,
                   offset);
      goto out;
    }

  entry_start = ALIGN_VALUE (offset + 4, 8);
  if (G_UNLIKELY (!(entry_start <= pack_len)))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted pack index; out of range data offset %" G_GUINT64_FORMAT,
                   entry_start);
      goto out;
    }

  g_assert ((((guint64)pack_data+offset) & 0x3) == 0);
  entry_len = GUINT32_FROM_BE (*((guint32*)(pack_data+offset)));

  entry_end = entry_start + entry_len;
  if (G_UNLIKELY (!(entry_end <= pack_len)))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted pack index; out of range entry length %u",
                   entry_len);
      goto out;
    }

  ret_entry = g_variant_new_from_data (is_meta ? OSTREE_PACK_META_FILE_VARIANT_FORMAT :
                                       OSTREE_PACK_DATA_FILE_VARIANT_FORMAT,
                                       pack_data+entry_start, entry_len,
                                       trusted, NULL, NULL);
  g_variant_ref_sink (ret_entry);
  ret = TRUE;
  ot_transfer_out_value (out_entry, &ret_entry);
 out:
  return ret;
}

gboolean
ostree_parse_file_pack_entry (GVariant       *pack_entry,
                              GInputStream  **out_input,
                              GFileInfo     **out_info,
                              GVariant      **out_xattrs,
                              GCancellable   *cancellable,
                              GError        **error)
{
  gboolean ret = FALSE;
  guchar entry_flags;
  ot_lvariant GVariant *file_header = NULL;
  ot_lvariant GVariant *pack_data = NULL;
  ot_lobj GInputStream *memory_input = NULL;
  ot_lobj GInputStream *ret_input = NULL;
  ot_lobj GFileInfo *ret_info = NULL;
  ot_lvariant GVariant *ret_xattrs = NULL;

  g_variant_get_child (pack_entry, 1, "y", &entry_flags);
  g_variant_get_child (pack_entry, 2, "@(uuuusa(ayay))", &file_header); 
  g_variant_get_child (pack_entry, 3, "@ay", &pack_data);

  if (!ostree_file_header_parse (file_header, &ret_info, &ret_xattrs,
                                 error))
    goto out;
  g_file_info_set_size (ret_info, g_variant_get_size (pack_data));

  if (g_file_info_get_file_type (ret_info) == G_FILE_TYPE_REGULAR)
    {
      memory_input = ot_variant_read (pack_data);

      if (entry_flags & OSTREE_PACK_FILE_ENTRY_FLAG_GZIP)
        {
          ot_lobj GConverter *decompressor = NULL;

          decompressor = (GConverter*)g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
          ret_input = (GInputStream*)g_object_new (G_TYPE_CONVERTER_INPUT_STREAM,
                                                   "converter", decompressor,
                                                   "base-stream", memory_input,
                                                   "close-base-stream", TRUE,
                                                   NULL);
        }
      else
        {
          ret_input = g_object_ref (memory_input);
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_input, &ret_input);
  ot_transfer_out_value (out_info, &ret_info);
  ot_transfer_out_value (out_xattrs, &ret_xattrs);
 out:
  return ret;
}

gboolean
ostree_pack_index_search (GVariant   *index,
                          GVariant   *csum_v,
                          OstreeObjectType objtype,
                          guint64    *out_offset)
{
  gboolean ret = FALSE;
  gsize imax, imin;
  gsize n;
  guint32 target_objtype;
  const guchar *csum;
  ot_lvariant GVariant *index_contents = NULL;

  csum = ostree_checksum_bytes_peek (csum_v);

  index_contents = g_variant_get_child_value (index, 2);

  target_objtype = (guint32) objtype;

  n = g_variant_n_children (index_contents);

  if (n == 0)
    goto out;

  imax = n - 1;
  imin = 0;
  while (imax >= imin)
    {
      GVariant *cur_csum_bytes;
      guchar cur_objtype;
      guint64 cur_offset;
      gsize imid;
      int c;

      imid = (imin + imax) / 2;

      g_variant_get_child (index_contents, imid, "(y@ayt)", &cur_objtype,
                           &cur_csum_bytes, &cur_offset);      

      c = ostree_cmp_checksum_bytes (ostree_checksum_bytes_peek (cur_csum_bytes), csum);
      if (c == 0)
        {
          if (cur_objtype < target_objtype)
            c = -1;
          else if (cur_objtype > target_objtype)
            c = 1;
        }
      g_variant_unref (cur_csum_bytes);

      if (c < 0)
        imin = imid + 1;
      else if (c > 0)
        {
          if (imid == 0)
            goto out;
          imax = imid - 1;
        }
      else
        {
          if (out_offset)
            *out_offset = GUINT64_FROM_BE (cur_offset);
          ret = TRUE;
          goto out;
        } 
    }

 out:
  return ret;
}

gboolean
ostree_validate_structureof_objtype (guchar    objtype,
                                     GError   **error)
{
  OstreeObjectType objtype_v = (OstreeObjectType) objtype;
  if (objtype_v < OSTREE_OBJECT_TYPE_FILE 
      || objtype_v > OSTREE_OBJECT_TYPE_COMMIT)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid object type '%u'", objtype);
      return FALSE;
    }
  return TRUE;
}

gboolean
ostree_validate_structureof_csum_v (GVariant  *checksum,
                                    GError   **error)
{
  gsize n_children = g_variant_n_children (checksum);
  if (n_children != 32)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid checksum of length %" G_GUINT64_FORMAT
                   " expected 32", (guint64) n_children);
      return FALSE;
    }
  return TRUE;
}

gboolean
ostree_validate_structureof_checksum_string (const char *checksum,
                                             GError   **error)
{
  int i = 0;
  size_t len = strlen (checksum);

  if (len != 64)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid rev '%s'", checksum);
      return FALSE;
    }

  for (i = 0; i < len; i++)
    {
      guint8 c = ((guint8*) checksum)[i];

      if (!((c >= 48 && c <= 57)
            || (c >= 97 && c <= 102)))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid character '%d' in rev '%s'",
                       c, checksum);
          return FALSE;
        }
    }
  return TRUE;
}

static gboolean
validate_variant (GVariant           *variant,
                  const GVariantType *variant_type,
                  GError            **error)
{
  if (!g_variant_is_normal_form (variant))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Not normal form");
      return FALSE;
    }
  if (!g_variant_is_of_type (variant, variant_type))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Doesn't match variant type '%s'",
                   (char*)variant_type);
      return FALSE;
    }
  return TRUE;
}

gboolean
ostree_validate_structureof_commit (GVariant      *commit,
                                    GError       **error)
{
  gboolean ret = FALSE;
  ot_lvariant GVariant *parent_csum_v = NULL;
  ot_lvariant GVariant *content_csum_v = NULL;
  ot_lvariant GVariant *metadata_csum_v = NULL;
  gsize n_elts;

  if (!validate_variant (commit, OSTREE_COMMIT_GVARIANT_FORMAT, error))
    goto out;

  g_variant_get_child (commit, 1, "@ay", &parent_csum_v);
  (void) g_variant_get_fixed_array (parent_csum_v, &n_elts, 1);
  if (n_elts > 0)
    {
      if (!ostree_validate_structureof_csum_v (parent_csum_v, error))
        goto out;
    }

  g_variant_get_child (commit, 6, "@ay", &content_csum_v);
  if (!ostree_validate_structureof_csum_v (content_csum_v, error))
    goto out;

  g_variant_get_child (commit, 7, "@ay", &metadata_csum_v);
  if (!ostree_validate_structureof_csum_v (metadata_csum_v, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_validate_structureof_dirtree (GVariant      *dirtree,
                                     GError       **error)
{
  gboolean ret = FALSE;
  const char *filename;
  ot_lvariant GVariant *content_csum_v = NULL;
  ot_lvariant GVariant *meta_csum_v = NULL;
  GVariantIter *contents_iter = NULL;

  if (!validate_variant (dirtree, OSTREE_TREE_GVARIANT_FORMAT, error))
    goto out;

  g_variant_get_child (dirtree, 0, "a(say)", &contents_iter);

  while (g_variant_iter_loop (contents_iter, "(&s@ay)",
                              &filename, &content_csum_v))
    {
      if (!ot_util_filename_validate (filename, error))
        goto out;
      if (!ostree_validate_structureof_csum_v (content_csum_v, error))
        goto out;
    }
  content_csum_v = NULL;

  g_variant_iter_free (contents_iter);
  g_variant_get_child (dirtree, 1, "a(sayay)", &contents_iter);

  while (g_variant_iter_loop (contents_iter, "(&s@ay@ay)",
                              &filename, &content_csum_v, &meta_csum_v))
    {
      if (!ot_util_filename_validate (filename, error))
        goto out;
      if (!ostree_validate_structureof_csum_v (content_csum_v, error))
        goto out;
      if (!ostree_validate_structureof_csum_v (meta_csum_v, error))
        goto out;
    }
  content_csum_v = NULL;
  meta_csum_v = NULL;

  ret = TRUE;
 out:
  if (contents_iter)
    g_variant_iter_free (contents_iter);
  return ret;
}

static gboolean
validate_stat_mode_perms (guint32        mode,
                          GError       **error)
{
  gboolean ret = FALSE;
  guint32 otherbits = (~S_IFMT & ~S_IRWXU & ~S_IRWXG & ~S_IRWXO &
                       ~S_ISUID & ~S_ISGID & ~S_ISVTX);

  if (mode & otherbits)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid mode %u; invalid bits in mode", mode);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_validate_structureof_file_mode (guint32            mode,
                                       GError           **error)
{
  gboolean ret = FALSE;

  if (!(S_ISREG (mode)
        || S_ISLNK (mode)
        || S_ISCHR (mode)
        || S_ISBLK (mode)
        || S_ISFIFO (mode)))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid file metadata mode %u; not a valid file type", mode);
      goto out;
    }

  if (!validate_stat_mode_perms (mode, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_validate_structureof_dirmeta (GVariant      *dirmeta,
                                     GError       **error)
{
  gboolean ret = FALSE;
  guint32 mode;

  if (!validate_variant (dirmeta, OSTREE_DIRMETA_GVARIANT_FORMAT, error))
    goto out;

  g_variant_get_child (dirmeta, 2, "u", &mode); 
  mode = GUINT32_FROM_BE (mode);

  if (!S_ISDIR (mode))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid directory metadata mode %u; not a directory", mode);
      goto out;
    }

  if (!validate_stat_mode_perms (mode, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_validate_structureof_pack_index (GVariant      *index,
                                        GError       **error)
{
  gboolean ret = FALSE;
  const char *header;
  guchar objtype_u8;
  guint64 offset;
  ot_lvariant GVariant *csum_v = NULL;
  GVariantIter *content_iter = NULL;

  if (!validate_variant (index, OSTREE_PACK_INDEX_VARIANT_FORMAT, error))
    goto out;

  g_variant_get_child (index, 0, "&s", &header);

  if (strcmp (header, "OSTv0PACKINDEX") != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid pack index; doesn't match header");
      goto out;
    }

  g_variant_get_child (index, 2, "a(yayt)", &content_iter);

  while (g_variant_iter_loop (content_iter, "(y@ayt)",
                              &objtype_u8, &csum_v, &offset))
    {
      if (!ostree_validate_structureof_objtype (objtype_u8, error))
        goto out;
      if (!ostree_validate_structureof_csum_v (csum_v, error))
        goto out;
    }
  csum_v = NULL;

  ret = TRUE;
 out:
  if (content_iter)
    g_variant_iter_free (content_iter);
  return ret;
}

gboolean
ostree_validate_structureof_pack_superindex (GVariant      *superindex,
                                             GError       **error)
{
  gboolean ret = FALSE;
  const char *header;
  ot_lvariant GVariant *csum_v = NULL;
  ot_lvariant GVariant *bloom = NULL;
  GVariantIter *content_iter = NULL;

  if (!validate_variant (superindex, OSTREE_PACK_SUPER_INDEX_VARIANT_FORMAT, error))
    goto out;

  g_variant_get_child (superindex, 0, "&s", &header);

  if (strcmp (header, "OSTv0SUPERPACKINDEX") != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid pack superindex; doesn't match header");
      goto out;
    }

  g_variant_get_child (superindex, 2, "a(ayay)", &content_iter);
  while (g_variant_iter_loop (content_iter, "(@ay@ay)",
                              &csum_v, &bloom))
    {
      if (!ostree_validate_structureof_csum_v (csum_v, error))
        goto out;
    }
  csum_v = NULL;

  g_variant_get_child (superindex, 3, "a(ayay)", &content_iter);
  while (g_variant_iter_loop (content_iter, "(@ay@ay)",
                              &csum_v, &bloom))
    {
      if (!ostree_validate_structureof_csum_v (csum_v, error))
        goto out;
    }
  csum_v = NULL;

  ret = TRUE;
 out:
  if (content_iter)
    g_variant_iter_free (content_iter);
  return ret;
}
