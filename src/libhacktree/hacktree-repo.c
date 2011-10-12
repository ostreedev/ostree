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

#include "hacktree-repo.h"
#include "htutil.h"

enum {
  PROP_0,

  PROP_PATH
};

G_DEFINE_TYPE (HacktreeRepo, hacktree_repo, G_TYPE_OBJECT)

#define GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), HACKTREE_TYPE_REPO, HacktreeRepoPrivate))

typedef struct _HacktreeRepoPrivate HacktreeRepoPrivate;

struct _HacktreeRepoPrivate {
  char *path;
  char *objects_path;

  gboolean inited;
};

static void
hacktree_repo_finalize (GObject *object)
{
  G_OBJECT_CLASS (hacktree_repo_parent_class)->finalize (object);
}

static void
hacktree_repo_set_property(GObject         *object,
			   guint            prop_id,
			   const GValue    *value,
			   GParamSpec      *pspec)
{
  HacktreeRepo *self = HACKTREE_REPO (object);
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);

  switch (prop_id)
    {
    case PROP_PATH:
      priv->path = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
hacktree_repo_get_property(GObject         *object,
			   guint            prop_id,
			   GValue          *value,
			   GParamSpec      *pspec)
{
  HacktreeRepo *self = HACKTREE_REPO (object);
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_string (value, priv->path);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
hacktree_repo_class_init (HacktreeRepoClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (HacktreeRepoPrivate));

  object_class->finalize = hacktree_repo_finalize;

  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
hacktree_repo_init (HacktreeRepo *self)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);
  
  g_assert (priv->path);

  priv->objects_path = g_build_filename (priv->path, "objects", NULL);
}

HacktreeRepo*
hacktree_repo_new (const char *path)
{
  return g_object_new (HACKTREE_TYPE_REPO, "path", path, NULL);
}

gboolean
hacktree_repo_check (HacktreeRepo *self, GError **error)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);
  
  priv->inited = TRUE;
  return TRUE;
}

char *
stat_to_string (struct stat *stbuf)
{
  return g_strdup_printf ("%d:%d:%d:%" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT,
                          stbuf->st_mode,
                          stbuf->st_uid,
                          stbuf->st_gid,
                          stbuf->st_atime,
                          stbuf->st_mtime,
                          stbuf->st_ctime);
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
stat_and_compute_checksum (int dirfd, const char *path,
                           GChecksum **out_checksum,
                           struct stat *out_stbuf,
                           GError **error)
{
  GChecksum *content_sha256 = NULL;
  GChecksum *content_and_meta_sha256 = NULL;
  char *stat_string = NULL;
  ssize_t bytes_read;
  char *xattrs = NULL;
  char *xattrs_canonicalized = NULL;
  int fd = -1;
  char *basename = NULL;
  gboolean ret = FALSE;
  char *symlink_target = NULL;
  char *device_id = NULL;

  if (fstat (fd, out_stbuf) < 0)
    {
      ht_util_set_error_from_errno (error, errno);
      goto out;
    }

  basename = g_path_get_basename (path);

  stat_string = stat_to_string (out_stbuf);

  /* FIXME - Add llistxattrat */
  if (!S_ISLNK(out_stbuf->st_mode))
    bytes_read = flistxattr (fd, NULL, 0);
  else
    bytes_read = llistxattr (path, NULL, 0);

  if (bytes_read < 0)
    {
      if (errno != ENOTSUP)
        {
          ht_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  if (errno != ENOTSUP)
    {
      gboolean tmp;
      xattrs = g_malloc (bytes_read);
      /* FIXME - Add llistxattrat */
      if (!S_ISLNK(out_stbuf->st_mode))
        tmp = flistxattr (fd, xattrs, bytes_read);
      else
        tmp = llistxattr (path, xattrs, bytes_read);
          
      if (!tmp)
        {
          ht_util_set_error_from_errno (error, errno);
          goto out;
        }

      xattrs_canonicalized = canonicalize_xattrs (xattrs, bytes_read);
    }

  content_sha256 = g_checksum_new (G_CHECKSUM_SHA256);
 
  if (S_ISREG(out_stbuf->st_mode))
    {
      guint8 buf[8192];

      fd = ht_util_open_file_read_at (dirfd, basename, error);
      if (fd < 0)
        {
          ht_util_set_error_from_errno (error, errno);
          goto out;
        }
      while ((bytes_read = read (fd, buf, sizeof (buf))) > 0)
        g_checksum_update (content_sha256, buf, bytes_read);
      if (bytes_read < 0)
        {
          ht_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else if (S_ISLNK(out_stbuf->st_mode))
    {
      symlink_target = g_malloc (PATH_MAX);

      if (readlinkat (dirfd, basename, symlink_target, PATH_MAX) < 0)
        {
          ht_util_set_error_from_errno (error, errno);
          goto out;
        }
      g_checksum_update (content_sha256, symlink_target, strlen (symlink_target));
    }
  else if (S_ISDEV(out_stbuf->st_mode))
    {
      device_id = g_strdup_printf ("%d", out_stbuf->st_rdev);
      g_checksum_update (content_sha256, device_id, strlen (device_id));
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

  g_checksum_update (content_and_meta_sha256, stat_string, strlen (stat_string));
  g_checksum_update (content_and_meta_sha256, xattrs_canonicalized, strlen (stat_string));

  *out_checksum = content_and_meta_sha256;
 out:
  if (fd >= 0)
    close (fd);
  g_free (symlink_target);
  g_free (basename);
  g_free (stat_string);
  g_free (xattrs);
  g_free (xattrs_canonicalized);
  if (content_sha256)
    g_checksum_free (content_sha256);

  return ret;
}

static char *
get_object_dir_for_checksum (HacktreeRepo *self,
                             GChecksum    *checksum,
                             GError      **error)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);
  char *checksum_prefix;
  char *path;
  gboolean ret = FALSE;
  GError *temp_error = NULL;

  checksum_prefix = g_strdup (g_checksum_get_string (checksum));
  checksum_prefix[3] = '\0';
  path = g_build_filename (priv->objects_path, checksum_prefix, NULL);

  if (!ht_util_ensure_directory (path, FALSE, error))
    goto out;
  
  ret = TRUE;
 out:
  g_free (checksum_prefix);
  if (ret)
    return path;
  g_free (path);
  return NULL;
}

static gboolean
import_one_file (HacktreeRepo *self, const char *path, GError **error)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);
  char *basename = NULL;
  char *dirname = NULL;
  GChecksum *id = NULL;
  DIR *dir = NULL;
  gboolean ret = FALSE;
  int fd;
  struct stat stbuf;
  char *dest_path = NULL;
  char *checksum_prefix;

  basename = g_path_get_dirname (path);
  dirname = g_path_get_dirname (path);

  dir = opendir (dirname);
  if (dir == NULL)
    {
      ht_util_set_error_from_errno (error, errno);
      goto out;
    }

  if (!stat_and_compute_checksum (dirfd (dir), path, &id, &stbuf, error))
    goto out;
  dest_path = get_object_dir_for_checksum (self, id, error);
  if (!dest_path)
    goto out;
  
  if (linkat (dirfd, basename, -1, dest_path, 0) < 0)
    {
      ht_util_set_error_from_errno (error, errno);
      goto out;
    }

  ret = TRUE;
 out:
  if (id != NULL)
    g_checksum_free (id);
  if (dir != NULL)
    closedir (dir);
  g_free (basename);
  g_free (dirname);
  return ret;
}

gboolean
hacktree_repo_import (HacktreeRepo *self,
                      const char   *path,
                      GError      **error)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);

  g_return_val_if_fail (priv->inited, FALSE);

  return import_one_file (self, path, error);
}
