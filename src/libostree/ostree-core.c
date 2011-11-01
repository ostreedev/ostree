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


void
ostree_checksum_update_stat (GChecksum *checksum, guint32 uid, guint32 gid, guint32 mode)
{
  guint32 perms = (mode & ~S_IFMT);
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
ostree_get_xattrs_for_path (const char *path,
                              GError    **error)
{
  GVariant *ret = NULL;
  GVariantBuilder builder;
  char *xattr_names = NULL;
  char *xattr_names_canonical = NULL;
  ssize_t bytes_read;

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

gboolean
ostree_stat_and_checksum_file (int dir_fd, const char *path,
                               OstreeObjectType objtype,
                               GChecksum **out_checksum,
                               struct stat *out_stbuf,
                               GError **error)
{
  GChecksum *content_sha256 = NULL;
  GChecksum *content_and_meta_sha256 = NULL;
  char *stat_string = NULL;
  ssize_t bytes_read;
  GVariant *xattrs = NULL;
  int fd = -1;
  DIR *temp_dir = NULL;
  char *basename = NULL;
  gboolean ret = FALSE;
  char *symlink_target = NULL;
  char *device_id = NULL;
  struct stat stbuf;

  basename = g_path_get_basename (path);

  if (dir_fd == -1)
    {
      char *dirname = g_path_get_dirname (path);
      temp_dir = opendir (dirname);
      if (temp_dir == NULL)
        {
          ot_util_set_error_from_errno (error, errno);
          g_free (dirname);
        }
      g_free (dirname);
      dir_fd = dirfd (temp_dir);
    }

  if (fstatat (dir_fd, basename, &stbuf, AT_SYMLINK_NOFOLLOW) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  if (S_ISREG(stbuf.st_mode))
    {
      fd = ot_util_open_file_read_at (dir_fd, basename, error);
      if (fd < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  if (objtype == OSTREE_OBJECT_TYPE_FILE)
    {
      xattrs = ostree_get_xattrs_for_path (path, error);
      if (!xattrs)
        goto out;
    }

  content_sha256 = g_checksum_new (G_CHECKSUM_SHA256);
 
  if (S_ISREG(stbuf.st_mode))
    {
      guint8 buf[8192];

      while ((bytes_read = read (fd, buf, sizeof (buf))) > 0)
        g_checksum_update (content_sha256, buf, bytes_read);
      if (bytes_read < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else if (S_ISLNK(stbuf.st_mode))
    {
      symlink_target = g_malloc (PATH_MAX);

      g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);
      
      bytes_read = readlinkat (dir_fd, basename, symlink_target, PATH_MAX);
      if (bytes_read < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
      g_checksum_update (content_sha256, (guint8*)symlink_target, bytes_read);
    }
  else if (S_ISCHR(stbuf.st_mode) || S_ISBLK(stbuf.st_mode))
    {
      g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);
      device_id = g_strdup_printf ("%u", (guint)stbuf.st_rdev);
      g_checksum_update (content_sha256, (guint8*)device_id, strlen (device_id));
    }
  else
    {
      g_set_error (error, G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Unsupported file '%s' (must be regular, symbolic link, or device)",
                   path);
      goto out;
    }

  content_and_meta_sha256 = g_checksum_copy (content_sha256);

  if (objtype == OSTREE_OBJECT_TYPE_FILE)
    {
      ostree_checksum_update_stat (content_and_meta_sha256, stbuf.st_uid,
                                   stbuf.st_gid, stbuf.st_mode);
      g_checksum_update (content_and_meta_sha256, (guint8*)g_variant_get_data (xattrs), g_variant_get_size (xattrs));
    }

  *out_stbuf = stbuf;
  *out_checksum = content_and_meta_sha256;
  ret = TRUE;
 out:
  if (fd >= 0)
    close (fd);
  if (temp_dir != NULL)
    closedir (temp_dir);
  g_free (symlink_target);
  g_free (basename);
  g_free (stat_string);
  if (xattrs)
    g_variant_unref (xattrs);
  if (content_sha256)
    g_checksum_free (content_sha256);
  return ret;
}

gboolean
ostree_set_xattrs (const char *path, GVariant *xattrs, GError **error)
{
  gboolean ret = FALSE;
  int i, n;

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
ostree_parse_metadata_file (const char                  *path,
                            OstreeSerializedVariantType *out_type,
                            GVariant                   **out_variant,
                            GError                     **error)
{
  GMappedFile *mfile = NULL;
  gboolean ret = FALSE;
  GVariant *ret_variant = NULL;
  GVariant *container = NULL;
  guint32 ret_type;

  mfile = g_mapped_file_new (path, FALSE, error);
  if (mfile == NULL)
    {
      goto out;
    }
  else
    {
      container = g_variant_new_from_data (G_VARIANT_TYPE (OSTREE_SERIALIZED_VARIANT_FORMAT),
                                           g_mapped_file_get_contents (mfile),
                                           g_mapped_file_get_length (mfile),
                                           FALSE,
                                           (GDestroyNotify) g_mapped_file_unref,
                                           mfile);
      g_variant_get (container, "(uv)",
                     &ret_type, &ret_variant);
      if (ret_type <= 0 || ret_type > OSTREE_SERIALIZED_VARIANT_LAST)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Corrupted metadata object '%s'; invalid type %d", path, ret_type);
          goto out;
        }
      mfile = NULL;
    }

  ret = TRUE;
  *out_type = ret_type;
  *out_variant = ret_variant;
  ret_variant = NULL;
 out:
  if (ret_variant)
    g_variant_unref (ret_variant);
  if (container != NULL)
    g_variant_unref (container);
  if (mfile != NULL)
    g_mapped_file_unref (mfile);
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
  char *path = NULL;
  GFileInfo *finfo = NULL;
  GFileInputStream *instream = NULL;
  gboolean pack_builder_initialized = FALSE;
  GVariantBuilder pack_builder;
  GVariant *pack_variant = NULL;
  GVariant *xattrs = NULL;
  gsize bytes_written;

  path = g_file_get_path (file);

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

      xattrs = ostree_get_xattrs_for_path (path, error);
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
          device = g_file_info_get_attribute_uint32 (finfo, G_FILE_ATTRIBUTE_UNIX_DEVICE);
          object_size = 4;
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
      else
        g_assert_not_reached ();
    }
  
  ret = TRUE;
 out:
  g_free (path);
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

