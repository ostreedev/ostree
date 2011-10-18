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

static char *
stat_to_string (struct stat *stbuf)
{
  return g_strdup_printf ("%u:%u:%u",
                          (guint32)(stbuf->st_mode & ~S_IFMT),
                          (guint32)stbuf->st_uid, 
                          (guint32)stbuf->st_gid);
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
                             g_variant_new_fixed_array (G_VARIANT_TYPE ("y"), buf, bytes_read, 1));

      g_free (buf);
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
      const char *p;
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
 out:
  if (!ret)
    g_variant_builder_clear (&builder);
  g_free (xattr_names);
  g_free (xattr_names_canonical);
  return ret;
}

gboolean
ostree_stat_and_checksum_file (int dir_fd, const char *path,
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

  if (!S_ISLNK(stbuf.st_mode))
    {
      fd = ot_util_open_file_read_at (dir_fd, basename, error);
      if (fd < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  stat_string = stat_to_string (&stbuf);
  xattrs = ostree_get_xattrs_for_path (path, error);
  if (!xattrs)
    goto out;

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

      if (readlinkat (dir_fd, basename, symlink_target, PATH_MAX) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
      g_checksum_update (content_sha256, (guint8*)symlink_target, strlen (symlink_target));
    }
  else if (S_ISCHR(stbuf.st_mode) || S_ISBLK(stbuf.st_mode))
    {
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

  g_checksum_update (content_and_meta_sha256, (guint8*)stat_string, strlen (stat_string));
  g_checksum_update (content_and_meta_sha256, (guint8*)g_variant_get_data (xattrs), g_variant_get_size (xattrs));

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
