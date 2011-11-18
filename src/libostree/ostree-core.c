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

gboolean
ostree_validate_checksum_string (const char *sha256,
                                 GError    **error)
{
  if (strlen (sha256) != 64)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid rev '%s'", sha256);
      return FALSE;
    }
  return TRUE;
}

GVariant *
ostree_wrap_metadata_variant (OstreeSerializedVariantType type,
                              GVariant *metadata)
{
  return g_variant_new ("(uv)", GUINT32_TO_BE ((guint32)type), metadata);
}

void
ostree_checksum_update_stat (GChecksum *checksum, guint32 uid, guint32 gid, guint32 mode)
{
  guint32 perms;
  perms = GUINT32_TO_BE (mode & ~S_IFMT);
  uid = GUINT32_TO_BE (uid);
  gid = GUINT32_TO_BE (gid);
  g_checksum_update (checksum, (guint8*) &uid, 4);
  g_checksum_update (checksum, (guint8*) &gid, 4);
  g_checksum_update (checksum, (guint8*) &perms, 4);
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

GVariant *
ostree_get_xattrs_for_file (GFile      *f,
                            GError    **error)
{
  const char *path;
  GVariant *ret = NULL;
  GVariantBuilder builder;
  char *xattr_names = NULL;
  char *xattr_names_canonical = NULL;
  ssize_t bytes_read;

  path = ot_gfile_get_path_cached (f);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ayay)"));

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

  ret = g_variant_builder_end (&builder);
  g_variant_ref_sink (ret);
 out:
  if (!ret)
    g_variant_builder_clear (&builder);
  g_free (xattr_names);
  g_free (xattr_names_canonical);
  return ret;
}

static gboolean
checksum_directory (GFile          *f,
                    GFileInfo      *f_info,
                    GChecksum     **out_checksum,
                    GCancellable   *cancellable,
                    GError        **error)
{
  gboolean ret = FALSE;
  GVariant *dirmeta = NULL;
  GVariant *packed = NULL;
  GChecksum *ret_checksum = NULL;

  if (!ostree_get_directory_metadata (f, f_info, &dirmeta, cancellable, error))
    goto out;
  packed = ostree_wrap_metadata_variant (OSTREE_SERIALIZED_DIRMETA_VARIANT, dirmeta);
  ret_checksum = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (ret_checksum, g_variant_get_data (packed),
                     g_variant_get_size (packed));

  ret = TRUE;
  *out_checksum = ret_checksum;
  ret_checksum = NULL;
 out:
  if (ret_checksum)
    g_checksum_free (ret_checksum);
  if (dirmeta)
    g_variant_unref (dirmeta);
  if (packed)
    g_variant_unref (packed);
  return ret;
}

static gboolean
checksum_nondirectory (GFile            *f,
                       GFileInfo        *file_info,
                       OstreeObjectType objtype,
                       GChecksum       **out_checksum,
                       GCancellable     *cancellable,
                       GError          **error)
{
  gboolean ret = FALSE;
  const char *path = NULL;
  GChecksum *content_sha256 = NULL;
  GChecksum *content_and_meta_sha256 = NULL;
  ssize_t bytes_read;
  GVariant *xattrs = NULL;
  char *basename = NULL;
  GInputStream *input = NULL;
  guint32 unix_mode;

  path = ot_gfile_get_path_cached (f);
  basename = g_path_get_basename (path);

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
      input = (GInputStream*)g_file_read (f, cancellable, error);
      if (!input)
        goto out;
    }

  if (objtype == OSTREE_OBJECT_TYPE_FILE)
    {
      xattrs = ostree_get_xattrs_for_file (f, error);
      if (!xattrs)
        goto out;
    }

  content_sha256 = g_checksum_new (G_CHECKSUM_SHA256);

  unix_mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
 
  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
      guint8 buf[8192];

      while ((bytes_read = g_input_stream_read (input, buf, sizeof (buf), cancellable, error)) > 0)
        g_checksum_update (content_sha256, buf, bytes_read);
      if (bytes_read < 0)
        goto out;
    }
  else if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      const char *symlink_target = g_file_info_get_symlink_target (file_info);

      g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);
      g_assert (symlink_target != NULL);
      
      g_checksum_update (content_sha256, (guint8*)symlink_target, strlen (symlink_target));
    }
  else if (S_ISCHR(unix_mode) || S_ISBLK(unix_mode))
    {
      guint32 rdev = g_file_info_get_attribute_uint32 (file_info, "unix::rdev");
      g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);
      rdev = GUINT32_TO_BE (rdev);
      g_checksum_update (content_sha256, (guint8*)&rdev, 4);
    }
  else if (S_ISFIFO(unix_mode))
    {
      g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);
    }
  else
    {
      g_set_error (error, G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Unsupported file '%s' (must be regular, symbolic link, fifo, or character/block device)",
                   path);
      goto out;
    }

  content_and_meta_sha256 = g_checksum_copy (content_sha256);

  if (objtype == OSTREE_OBJECT_TYPE_FILE)
    {
      ostree_checksum_update_stat (content_and_meta_sha256,
                                   g_file_info_get_attribute_uint32 (file_info, "unix::uid"),
                                   g_file_info_get_attribute_uint32 (file_info, "unix::gid"),
                                   g_file_info_get_attribute_uint32 (file_info, "unix::mode"));
      g_checksum_update (content_and_meta_sha256, (guint8*)g_variant_get_data (xattrs), g_variant_get_size (xattrs));
    }

  *out_checksum = content_and_meta_sha256;
  ret = TRUE;
 out:
  g_clear_object (&input);
  g_free (basename);
  if (xattrs)
    g_variant_unref (xattrs);
  if (content_sha256)
    g_checksum_free (content_sha256);
  return ret;
}

gboolean
ostree_checksum_file (GFile            *f,
                      OstreeObjectType objtype,
                      GChecksum       **out_checksum,
                      GCancellable     *cancellable,
                      GError          **error)
{
  gboolean ret = FALSE;
  GFileInfo *file_info = NULL;
  GChecksum *ret_checksum = NULL;

  file_info = g_file_query_info (f, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!file_info)
    goto out;

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
    {
      if (!checksum_directory (f, file_info, &ret_checksum, cancellable, error))
        goto out;
    }
  else
    {
      if (!checksum_nondirectory (f, file_info, objtype, &ret_checksum, cancellable, error))
        goto out;
    }

  ret = TRUE;
  *out_checksum = ret_checksum;
  ret_checksum = NULL;
 out:
  g_clear_object (&file_info);
  return ret;
}

gboolean
ostree_get_directory_metadata (GFile        *dir,
                               GFileInfo    *dir_info,
                               GVariant    **out_metadata,
                               GCancellable *cancellable,
                               GError      **error)
{
  gboolean ret = FALSE;
  GVariant *xattrs = NULL;
  GVariant *ret_metadata = NULL;

  xattrs = ostree_get_xattrs_for_file (dir, error);
  if (!xattrs)
    goto out;

  ret_metadata = g_variant_new ("(uuuu@a(ayay))",
                                OSTREE_DIR_META_VERSION,
                                GUINT32_TO_BE (g_file_info_get_attribute_uint32 (dir_info, "unix::uid")),
                                GUINT32_TO_BE (g_file_info_get_attribute_uint32 (dir_info, "unix::gid")),
                                GUINT32_TO_BE (g_file_info_get_attribute_uint32 (dir_info, "unix::mode")),
                                xattrs);
  g_variant_ref_sink (ret_metadata);

  ret = TRUE;
  *out_metadata = ret_metadata;
  ret_metadata = NULL;
 out:
  if (ret_metadata)
    g_variant_unref (ret_metadata);
  if (xattrs)
    g_variant_unref (xattrs);
  return ret;
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
      
      g_variant_unref (value);
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

gboolean
ostree_parse_metadata_file (GFile                       *file,
                            OstreeSerializedVariantType *out_type,
                            GVariant                   **out_variant,
                            GError                     **error)
{
  gboolean ret = FALSE;
  GVariant *ret_variant = NULL;
  GVariant *container = NULL;
  guint32 ret_type;

  if (!ot_util_variant_map (file, G_VARIANT_TYPE (OSTREE_SERIALIZED_VARIANT_FORMAT),
                            &container, error))
    goto out;

  g_variant_get (container, "(uv)",
                 &ret_type, &ret_variant);
  ret_type = GUINT32_FROM_BE (ret_type);
  if (ret_type <= 0 || ret_type > OSTREE_SERIALIZED_VARIANT_LAST)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted metadata object '%s'; invalid type %d",
                   ot_gfile_get_path_cached (file), ret_type);
      goto out;
    }

  ret = TRUE;
  *out_type = ret_type;
  *out_variant = ot_util_variant_take_ref (ret_variant);
  ret_variant = NULL;
 out:
  if (ret_variant)
    g_variant_unref (ret_variant);
  if (container != NULL)
    g_variant_unref (container);
  return ret;
}

char *
ostree_get_relative_object_path (const char *checksum,
                                 OstreeObjectType type,
                                 gboolean         archive)
{
  GString *path;
  const char *type_string;

  g_assert (strlen (checksum) == 64);

  path = g_string_new ("objects/");

  g_string_append_len (path, checksum, 2);
  g_string_append_c (path, '/');
  g_string_append (path, checksum + 2);
  switch (type)
    {
    case OSTREE_OBJECT_TYPE_FILE:
      if (archive)
        type_string = ".packfile";
      else
        type_string = ".file";
      break;
    case OSTREE_OBJECT_TYPE_META:
      type_string = ".meta";
      break;
    default:
      g_assert_not_reached ();
    }
  g_string_append (path, type_string);
  return g_string_free (path, FALSE);
}

gboolean
ostree_pack_object (GOutputStream     *output,
                    GFile             *file,
                    OstreeObjectType  objtype,
                    GCancellable     *cancellable,
                    GError          **error)
{
  gboolean ret = FALSE;
  GFileInfo *finfo = NULL;
  GFileInputStream *instream = NULL;
  gboolean pack_builder_initialized = FALSE;
  GVariantBuilder pack_builder;
  GVariant *pack_variant = NULL;
  GVariant *xattrs = NULL;
  gsize bytes_written;

  finfo = g_file_query_info (file, "standard::type,standard::size,standard::is-symlink,standard::symlink-target,unix::*",
                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
  if (!finfo)
    goto out;

  if (objtype == OSTREE_OBJECT_TYPE_META)
    {
      guint64 object_size_be = GUINT64_TO_BE ((guint64)g_file_info_get_size (finfo));
      if (!g_output_stream_write_all (output, &object_size_be, 8, &bytes_written, cancellable, error))
        goto out;

      instream = g_file_read (file, NULL, error);
      if (!instream)
        goto out;
      
      if (g_output_stream_splice (output, (GInputStream*)instream, 0, cancellable, error) < 0)
        goto out;
    }
  else
    {
      guint32 uid, gid, mode;
      guint32 device = 0;
      guint32 metadata_size_be;
      const char *target = NULL;
      guint64 object_size;

      uid = g_file_info_get_attribute_uint32 (finfo, G_FILE_ATTRIBUTE_UNIX_UID);
      gid = g_file_info_get_attribute_uint32 (finfo, G_FILE_ATTRIBUTE_UNIX_GID);
      mode = g_file_info_get_attribute_uint32 (finfo, G_FILE_ATTRIBUTE_UNIX_MODE);

      g_variant_builder_init (&pack_builder, G_VARIANT_TYPE (OSTREE_PACK_FILE_VARIANT_FORMAT));
      pack_builder_initialized = TRUE;
      g_variant_builder_add (&pack_builder, "u", GUINT32_TO_BE (0));
      g_variant_builder_add (&pack_builder, "u", GUINT32_TO_BE (uid));
      g_variant_builder_add (&pack_builder, "u", GUINT32_TO_BE (gid));
      g_variant_builder_add (&pack_builder, "u", GUINT32_TO_BE (mode));

      xattrs = ostree_get_xattrs_for_file (file, error);
      if (!xattrs)
        goto out;
      g_variant_builder_add (&pack_builder, "@a(ayay)", xattrs);

      if (S_ISREG (mode))
        {
          object_size = (guint64)g_file_info_get_size (finfo);
        }
      else if (S_ISLNK (mode))
        {
          target = g_file_info_get_attribute_byte_string (finfo, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET);
          object_size = strlen (target);
        }
      else if (S_ISBLK (mode) || S_ISCHR (mode))
        {
          device = g_file_info_get_attribute_uint32 (finfo, G_FILE_ATTRIBUTE_UNIX_RDEV);
          object_size = 4;
        }
      else if (S_ISFIFO (mode))
        {
          object_size = 0;
        }
      else
        g_assert_not_reached ();

      g_variant_builder_add (&pack_builder, "t", GUINT64_TO_BE (object_size));
      pack_variant = g_variant_builder_end (&pack_builder);
      pack_builder_initialized = FALSE;

      metadata_size_be = GUINT32_TO_BE (g_variant_get_size (pack_variant));

      if (!g_output_stream_write_all (output, &metadata_size_be, 4,
                                      &bytes_written, cancellable, error))
        goto out;
      g_assert (bytes_written == 4);

      if (!g_output_stream_write_all (output, g_variant_get_data (pack_variant), g_variant_get_size (pack_variant),
                                      &bytes_written, cancellable, error))
        goto out;

      if (S_ISREG (mode))
        {
          instream = g_file_read (file, NULL, error);
          if (!instream)
            goto out;
          bytes_written = g_output_stream_splice (output, (GInputStream*)instream, 0, cancellable, error);
          if (bytes_written < 0)
            goto out;
          if (bytes_written != object_size)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "File size changed unexpectedly");
              goto out;
            }
        }
      else if (S_ISLNK (mode))
        {
          if (!g_output_stream_write_all (output, target, object_size,
                                          &bytes_written, cancellable, error))
            goto out;
        }
      else if (S_ISBLK (mode) || S_ISCHR (mode))
        {
          guint32 device_be = GUINT32_TO_BE (device);
          g_assert (object_size == 4);
          if (!g_output_stream_write_all (output, &device_be, object_size,
                                          &bytes_written, cancellable, error))
            goto out;
          g_assert (bytes_written == 4);
        }
      else if (S_ISFIFO (mode))
        {
        }
      else
        g_assert_not_reached ();
    }
  
  ret = TRUE;
 out:
  g_clear_object (&finfo);
  g_clear_object (&instream);
  if (xattrs)
    g_variant_unref (xattrs);
  if (pack_builder_initialized)
    g_variant_builder_clear (&pack_builder);
  if (pack_variant)
    g_variant_unref (pack_variant);
  return ret;
}

static gboolean
splice_and_checksum (GOutputStream  *out,
                     GInputStream   *in,
                     GChecksum      *checksum,
                     GCancellable   *cancellable,
                     GError        **error)
{
  gboolean ret = FALSE;
  
  if (checksum != NULL)
    {
      gsize bytes_read, bytes_written;
      char buf[4096];
      do
        {
          if (!g_input_stream_read_all (in, buf, sizeof(buf), &bytes_read, cancellable, error))
            goto out;
          if (checksum)
            g_checksum_update (checksum, (guint8*)buf, bytes_read);
          if (!g_output_stream_write_all (out, buf, bytes_read, &bytes_written, cancellable, error))
            goto out;
        }
      while (bytes_read > 0);
    }
  else
    {
      if (g_output_stream_splice (out, in, 0, cancellable, error) < 0)
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
unpack_meta (const char   *path,
             const char   *dest_path,    
             GChecksum   **out_checksum,
             GError      **error)
{
  gboolean ret = FALSE;
  GFile *file = NULL;
  GFile *dest_file = NULL;
  GFileInputStream *in = NULL;
  GChecksum *ret_checksum = NULL;
  GFileOutputStream *out = NULL;

  file = ot_gfile_new_for_path (path);
  dest_file = ot_gfile_new_for_path (dest_path);

  if (out_checksum)
    ret_checksum = g_checksum_new (G_CHECKSUM_SHA256);

  in = g_file_read (file, NULL, error);
  if (!in)
    goto out;

  out = g_file_replace (dest_file, NULL, FALSE, 0, NULL, error);
  if (!out)
    goto out;

  if (!splice_and_checksum ((GOutputStream*)out, (GInputStream*)in, ret_checksum, NULL, error))
    goto out;

  if (!g_output_stream_close ((GOutputStream*)out, NULL, error))
    goto out;

  ret = TRUE;
  if (out_checksum)
    *out_checksum = ret_checksum;
  ret_checksum = NULL;
 out:
  if (!ret)
    (void) unlink (dest_path);
  if (ret_checksum)
    g_checksum_free (ret_checksum);
  g_clear_object (&file);
  g_clear_object (&dest_file);
  g_clear_object (&in);
  return ret;
}

gboolean
ostree_parse_packed_file (GFile            *file,
                          GVariant    **out_metadata,
                          GInputStream **out_content,
                          GCancellable *cancellable,
                          GError      **error)
{
  gboolean ret = FALSE;
  char *metadata_buf = NULL;
  GVariant *ret_metadata = NULL;
  GFileInputStream *in = NULL;
  guint32 metadata_len;
  gsize bytes_read;

  in = g_file_read (file, NULL, error);
  if (!in)
    goto out;
      
  if (!g_input_stream_read_all ((GInputStream*)in, &metadata_len, 4, &bytes_read, NULL, error))
    goto out;
  if (bytes_read != 4)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted packfile; too short while reading metadata length");
      goto out;
    }
      
  metadata_len = GUINT32_FROM_BE (metadata_len);
  if (metadata_len > OSTREE_MAX_METADATA_SIZE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted packfile; metadata length %u is larger than maximum %u",
                   metadata_len, OSTREE_MAX_METADATA_SIZE);
      goto out;
    }
  metadata_buf = g_malloc (metadata_len);

  if (!g_input_stream_read_all ((GInputStream*)in, metadata_buf, metadata_len, &bytes_read, NULL, error))
    goto out;
  if (bytes_read != metadata_len)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted packfile; too short while reading metadata");
      goto out;
    }

  ret_metadata = g_variant_new_from_data (G_VARIANT_TYPE (OSTREE_PACK_FILE_VARIANT_FORMAT),
                                          metadata_buf, metadata_len, FALSE,
                                          (GDestroyNotify)g_free,
                                          metadata_buf);
  metadata_buf = NULL;

  ret = TRUE;
  *out_metadata = ret_metadata;
  ret_metadata = NULL;
  *out_content = (GInputStream*)in;
  in = NULL;
 out:
  g_clear_object (&in);
  if (ret_metadata)
   g_variant_unref (ret_metadata);
  return ret;
}

static gboolean
unpack_file (const char   *path,
             const char   *dest_path,    
             GChecksum   **out_checksum,
             GError      **error)
{
  gboolean ret = FALSE;
  GFile *file = NULL;
  GFile *dest_file = NULL;
  GVariant *metadata = NULL;
  GVariant *xattrs = NULL;
  GInputStream *in = NULL;
  GFileOutputStream *out = NULL;
  GChecksum *ret_checksum = NULL;
  guint32 version, uid, gid, mode;
  guint64 content_len;
  gsize bytes_read;

  file = ot_gfile_new_for_path (path);

  if (!ostree_parse_packed_file (file, &metadata, &in, NULL, error))
    goto out;

  g_variant_get (metadata, "(uuuu@a(ayay)t)",
                 &version, &uid, &gid, &mode,
                 &xattrs, &content_len);
  uid = GUINT32_FROM_BE (uid);
  gid = GUINT32_FROM_BE (gid);
  mode = GUINT32_FROM_BE (mode);
  content_len = GUINT64_FROM_BE (content_len);

  dest_file = ot_gfile_new_for_path (dest_path);
      
  if (out_checksum)
    ret_checksum = g_checksum_new (G_CHECKSUM_SHA256);

  if (S_ISREG (mode))
    {
      out = g_file_replace (dest_file, NULL, FALSE, 0, NULL, error);
      if (!out)
        goto out;

      if (!splice_and_checksum ((GOutputStream*)out, in, ret_checksum, NULL, error))
        goto out;

      if (!g_output_stream_close ((GOutputStream*)out, NULL, error))
        goto out;
    }
  else if (S_ISLNK (mode))
    {
      char target[PATH_MAX+1];

      if (!g_input_stream_read_all (in, target, sizeof(target)-1, &bytes_read, NULL, error))
        goto out;
      target[bytes_read] = '\0';
      if (ret_checksum)
        g_checksum_update (ret_checksum, (guint8*)target, bytes_read);
      if (symlink (target, dest_path) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else if (S_ISCHR (mode) || S_ISBLK (mode))
    {
      guint32 dev;

      if (!g_input_stream_read_all (in, &dev, 4, &bytes_read, NULL, error))
        goto out;
      if (bytes_read != 4)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Corrupted packfile; too short while reading device id");
          goto out;
        }
      dev = GUINT32_FROM_BE (dev);
      if (ret_checksum)
        g_checksum_update (ret_checksum, (guint8*)&dev, 4);
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
                   "Corrupted packfile; invalid mode %u", mode);
      goto out;
    }

  if (!S_ISLNK (mode))
    {
      if (chmod (dest_path, mode) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  if (!ostree_set_xattrs (file, xattrs, NULL, error))
    goto out;

  if (ret_checksum)
    {
      ostree_checksum_update_stat (ret_checksum, uid, gid, mode);
      g_checksum_update (ret_checksum, (guint8*)g_variant_get_data (xattrs), g_variant_get_size (xattrs));
    }

  ret = TRUE;
  if (out_checksum)
    *out_checksum = ret_checksum;
  ret_checksum = NULL;
 out:
  if (!ret)
    (void) unlink (dest_path);
  if (ret_checksum)
    g_checksum_free (ret_checksum);
  g_clear_object (&file);
  g_clear_object (&dest_file);
  g_clear_object (&in);
  g_clear_object (&out);
  if (metadata)
   g_variant_unref (metadata);
  if (xattrs)
    g_variant_unref (xattrs);
  return ret;
}

gboolean
ostree_unpack_object (const char   *path,
                      OstreeObjectType  objtype,
                      const char   *dest_path,    
                      GChecksum   **out_checksum,
                      GError      **error)
{
  if (objtype == OSTREE_OBJECT_TYPE_META)
    return unpack_meta (path, dest_path, out_checksum, error);
  else
    return unpack_file (path, dest_path, out_checksum, error);
}
  


