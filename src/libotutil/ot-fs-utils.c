/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include "ot-fs-utils.h"
#include "libglnx.h"
#include <sys/xattr.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

/* Convert a fd-relative path to a GFile* - use
 * for legacy code.
 */
GFile *
ot_fdrel_to_gfile (int dfd, const char *path)
{
  g_autofree char *abspath = glnx_fdrel_abspath (dfd, path);
  return g_file_new_for_path (abspath);
}

gboolean
ot_readlinkat_gfile_info (int             dfd,
                          const char     *path,
                          GFileInfo      *target_info,
                          GCancellable   *cancellable,
                          GError        **error)
{
  char targetbuf[PATH_MAX+1];
  ssize_t len;

  if (TEMP_FAILURE_RETRY (len = readlinkat (dfd, path, targetbuf, sizeof (targetbuf) - 1)) < 0)
    return glnx_throw_errno_prefix (error, "readlinkat");
  targetbuf[len] = '\0';
  g_file_info_set_symlink_target (target_info, targetbuf);

  return TRUE;
}


/**
 * ot_openat_read_stream:
 * @dfd: Directory file descriptor
 * @path: Subpath
 * @follow: Whether or not to follow symbolic links
 * @out_istream: (out): Return location for input stream
 * @cancellable: Cancellable
 * @error: Error
 *
 * Open a file for reading starting from @dfd for @path.
 * The @follow parameter determines whether or not to follow
 * if the last element of @path is a symbolic link.  Intermediate
 * symlink path components are always followed.
 */
gboolean
ot_openat_read_stream (int             dfd,
                       const char     *path,
                       gboolean        follow,
                       GInputStream  **out_istream,
                       GCancellable   *cancellable,
                       GError        **error)
{
  int fd = -1;
  int flags = O_RDONLY | O_NOCTTY | O_CLOEXEC;

  if (!follow)
    flags |= O_NOFOLLOW;

  if (TEMP_FAILURE_RETRY (fd = openat (dfd, path, flags, 0)) < 0)
    return glnx_throw_errno_prefix (error, "openat(%s)", path);

  *out_istream = g_unix_input_stream_new (fd, TRUE);
  return TRUE;
}

gboolean
ot_ensure_unlinked_at (int dfd,
                       const char *path,
                       GError **error)
{
  if (unlinkat (dfd, path, 0) != 0)
    {
      if (G_UNLIKELY (errno != ENOENT))
        return glnx_throw_errno_prefix (error, "unlink(%s)", path);
    }
  return TRUE;
}

gboolean
ot_query_exists_at (int dfd, const char *path,
                    gboolean *out_exists,
                    GError **error)
{
  struct stat stbuf;
  gboolean ret_exists;

  if (fstatat (dfd, path, &stbuf, AT_SYMLINK_NOFOLLOW) < 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "fstatat(%s)", path);
      ret_exists = FALSE;
    }
  else
    ret_exists = TRUE;

  *out_exists = ret_exists;
  return TRUE;
}

gboolean
ot_openat_ignore_enoent (int dfd,
                         const char *path,
                         int *out_fd,
                         GError **error)
{
  int target_fd = openat (dfd, path, O_CLOEXEC | O_RDONLY);
  if (target_fd < 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "openat(%s)", path);
    }

  *out_fd = target_fd;
  return TRUE;
}

/* Like glnx_dirfd_iterator_init_at(), but if %ENOENT, then set
 * @out_exists to %FALSE, and return successfully.
 */
gboolean
ot_dfd_iter_init_allow_noent (int dfd,
                              const char *path,
                              GLnxDirFdIterator *dfd_iter,
                              gboolean *out_exists,
                              GError **error)
{
  glnx_fd_close int fd = glnx_opendirat_with_errno (dfd, path, TRUE);
  if (fd < 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "opendirat");
      *out_exists = FALSE;
      return TRUE;
    }
  if (!glnx_dirfd_iterator_init_take_fd (&fd, dfd_iter, error))
    return FALSE;
  *out_exists = TRUE;
  return TRUE;
}

GBytes *
ot_file_mapat_bytes (int dfd,
                     const char *path,
                     GError **error)
{
  glnx_fd_close int fd = openat (dfd, path, O_RDONLY | O_CLOEXEC);
  g_autoptr(GMappedFile) mfile = NULL;

  if (fd < 0)
    return glnx_null_throw_errno_prefix (error, "openat(%s)", path);

  mfile = g_mapped_file_new_from_fd (fd, FALSE, error);
  if (!mfile)
    return FALSE;

  return g_mapped_file_get_bytes (mfile);
}

/* Given an input stream, splice it to an anonymous file (O_TMPFILE).
 * Useful for potentially large but transient files.
 */
GBytes *
ot_map_anonymous_tmpfile_from_content (GInputStream *instream,
                                       GCancellable *cancellable,
                                       GError      **error)
{
  g_auto(GLnxTmpfile) tmpf = { 0, };
  if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &tmpf, error))
    return NULL;

  g_autoptr(GOutputStream) out = g_unix_output_stream_new (tmpf.fd, FALSE);
  gssize n_bytes_written = g_output_stream_splice (out, instream,
                                                   G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                                   G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                                   cancellable, error);
  if (n_bytes_written < 0)
    return NULL;

  g_autoptr(GMappedFile) mfile = g_mapped_file_new_from_fd (tmpf.fd, FALSE, error);
  if (!mfile)
    return NULL;
  return g_mapped_file_get_bytes (mfile);
}
