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
      ot_util_set_error_from_errno (error, errno);
      return FALSE;
    }
  *out_fd = ret;
  return TRUE;
}

