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

#include "ostree-linuxfsutil.h"
#include "otutil.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <ext2fs/ext2_fs.h>

/**
 * _ostree_linuxfs_fd_alter_immutable_flag:
 * @fd: A file descriptor
 * @new_immutable_state: Set this to %TRUE to make the file immutable, %FALSE to unset the flag
 * @cancellable: Cancellable
 * @error: GError
 *
 * Alter the immutable flag of object referred to by @fd; may be a
 * regular file or a directory.
 *
 * If the operation is not supported by the underlying filesystem, or
 * we are running without sufficient privileges, this function will
 * silently do nothing.
 */
gboolean
_ostree_linuxfs_fd_alter_immutable_flag (int            fd,
                                         gboolean       new_immutable_state,
                                         GCancellable  *cancellable,
                                         GError       **error)
{
  gboolean ret = FALSE;
  unsigned long flags;
  int r;
  static gint no_alter_immutable = 0;

  if (g_atomic_int_get (&no_alter_immutable))
    return TRUE;

  r = ioctl (fd, EXT2_IOC_GETFLAGS, &flags);
  if (r == -1)
    {
      int errsv = errno;
      if (errsv == EPERM)
        g_atomic_int_set (&no_alter_immutable, 1);
      else if (errsv == EOPNOTSUPP || errsv == ENOTTY)
        ;
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "ioctl(EXT2_IOC_GETFLAGS): %s",
                       g_strerror (errsv));
          goto out;
        }
    }
  else
    {
      if (new_immutable_state)
        flags |= EXT2_IMMUTABLE_FL;
      else
        flags &= ~EXT2_IMMUTABLE_FL;
      r = ioctl (fd, EXT2_IOC_SETFLAGS, &flags);
      if (r == -1)
        {
          int errsv = errno;
          if (errsv == EPERM)
            g_atomic_int_set (&no_alter_immutable, 1);
          else if (errsv == EOPNOTSUPP || errsv == ENOTTY)
            ;
          else
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "ioctl(EXT2_IOC_GETFLAGS): %s",
                           g_strerror (errsv));
              goto out;
            }
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
_ostree_linuxfs_alter_immutable_flag (GFile         *path,
                                      gboolean       new_immutable_state,
                                      GCancellable  *cancellable,
                                      GError       **error)
{
  gboolean ret = FALSE;
  glnx_fd_close int fd = -1;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  fd = open (gs_file_get_path_cached (path), O_RDONLY|O_NONBLOCK|O_LARGEFILE);
  if (fd == -1)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "open(%s): %s",
                   gs_file_get_path_cached (path),
                   g_strerror (errsv));
      goto out;
    }

  if (!_ostree_linuxfs_fd_alter_immutable_flag (fd, new_immutable_state,
                                                cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
