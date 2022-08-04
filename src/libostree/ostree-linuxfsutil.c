/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ostree-linuxfsutil.h"
#include "otutil.h"

#include <fcntl.h>
#include <sys/ioctl.h>
// This should be the only file including linux/fs.h; see
// https://sourceware.org/glibc/wiki/Release/2.36#Usage_of_.3Clinux.2Fmount.h.3E_and_.3Csys.2Fmount.h.3E
// https://github.com/ostreedev/ostree/issues/2685
#include <linux/fs.h>
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
  static gint no_alter_immutable = 0;

  if (g_atomic_int_get (&no_alter_immutable))
    return TRUE;

  int flags = 0;
  int r = ioctl (fd, EXT2_IOC_GETFLAGS, &flags);
  if (r == -1)
    {
      if (errno == EPERM)
        g_atomic_int_set (&no_alter_immutable, 1);
      else if (errno == EOPNOTSUPP || errno == ENOTTY)
        ;
      else
        return glnx_throw_errno_prefix (error, "ioctl(EXT2_IOC_GETFLAGS)");
    }
  else
    {
      gboolean prev_immutable_state = (flags & EXT2_IMMUTABLE_FL) > 0;
      if (prev_immutable_state == new_immutable_state)
        return TRUE;  /* Nothing to do */

      if (new_immutable_state)
        flags |= EXT2_IMMUTABLE_FL;
      else
        flags &= ~EXT2_IMMUTABLE_FL;
      r = ioctl (fd, EXT2_IOC_SETFLAGS, &flags);
      if (r == -1)
        {
          if (errno == EPERM)
            g_atomic_int_set (&no_alter_immutable, 1);
          else if (errno == EOPNOTSUPP || errno == ENOTTY)
            ;
          else
            return glnx_throw_errno_prefix (error, "ioctl(EXT2_IOC_SETFLAGS)");
        }
    }

  return TRUE;
}

/* Wrapper for FIFREEZE ioctl.
 * This is split into a separate wrapped API for
 * reasons around conflicts between glibc and linux/fs.h
 * includes; see above.
 */
int
_ostree_linuxfs_filesystem_freeze (int fd)
{
  return TEMP_FAILURE_RETRY (ioctl (fd, FIFREEZE, 0));
}

/* Wrapper for FITHAW ioctl.  See above. */
int
_ostree_linuxfs_filesystem_thaw (int fd)
{
  return TEMP_FAILURE_RETRY (ioctl (fd, FITHAW, 0));
}
