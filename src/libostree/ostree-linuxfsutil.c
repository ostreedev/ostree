/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#include "config.h"

#include "ostree-linuxfsutil.h"
#include "otutil.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <ext2fs/ext2_fs.h>

#include "otutil.h"

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

  unsigned long flags;
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
