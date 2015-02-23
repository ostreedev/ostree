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
#include "libgsystem.h"
#include <attr/xattr.h>
#include <gio/gunixinputstream.h>

int
ot_opendirat (int dfd, const char *path, gboolean follow)
{
  int flags = O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY;
  if (!follow)
    flags |= O_NOFOLLOW;
  return openat (dfd, path, flags);
}

gboolean
ot_gopendirat (int             dfd,
               const char     *path,
               gboolean        follow,
               int            *out_fd,
               GError        **error)
{
  int ret = ot_opendirat (dfd, path, follow);
  if (ret == -1)
    {
      gs_set_error_from_errno (error, errno);
      return FALSE;
    }
  *out_fd = ret;
  return TRUE;
}

gboolean
ot_readlinkat_gfile_info (int             dfd,
                          const char     *path,
                          GFileInfo      *target_info,
                          GCancellable   *cancellable,
                          GError        **error)
{
  gboolean ret = FALSE;
  char targetbuf[PATH_MAX+1];
  ssize_t len;

  do
    len = readlinkat (dfd, path, targetbuf, sizeof (targetbuf) - 1);
  while (G_UNLIKELY (len == -1 && errno == EINTR));
  if (len == -1)
    {
      gs_set_error_from_errno (error, errno);
      goto out;
    }
  targetbuf[len] = '\0';
  g_file_info_set_symlink_target (target_info, targetbuf);

  ret = TRUE;
 out:
  return ret;
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
  gboolean ret = FALSE;
  int fd = -1;
  int flags = O_RDONLY | O_NOCTTY | O_CLOEXEC;

  if (!follow)
    flags |= O_NOFOLLOW;

  do
    fd = openat (dfd, path, flags, 0);
  while (G_UNLIKELY (fd == -1 && errno == EINTR));
  if (fd == -1)
    {
      gs_set_error_from_errno (error, errno);
      goto out;
    }

  *out_istream = g_unix_input_stream_new (fd, TRUE);
  ret = TRUE;
 out:
  return ret;
}
