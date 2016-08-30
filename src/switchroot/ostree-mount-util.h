/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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
 */

#ifndef __OSTREE_MOUNT_UTIL_H_
#define __OSTREE_MOUNT_UTIL_H_

#include <err.h>
#include <stdlib.h>
#include <sys/statvfs.h>

static inline int
path_is_on_readonly_fs (char *path)
{
  struct statvfs stvfsbuf;

  if (statvfs (path, &stvfsbuf) == -1)
    err (EXIT_FAILURE, "statvfs(%s)", path);

  return (stvfsbuf.f_flag & ST_RDONLY) != 0;
}

#endif /* __OSTREE_MOUNT_UTIL_H_ */
