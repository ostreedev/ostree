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

GBytes *
ot_lgetxattrat (int            dfd,
                const char    *path,
                const char    *attribute,
                GError       **error)
{
  /* A workaround for the lack of lgetxattrat(), thanks to Florian Weimer:
   * https://mail.gnome.org/archives/ostree-list/2014-February/msg00017.html
   */
  gs_free char *full_path = g_strdup_printf ("/proc/self/fd/%d/%s", dfd, path);
  GBytes *bytes = NULL;
  ssize_t bytes_read, real_size;
  char *buf;

  do
    bytes_read = lgetxattr (full_path, attribute, NULL, 0);
  while (G_UNLIKELY (bytes_read < 0 && errno == EINTR));
  if (G_UNLIKELY (bytes_read < 0))
    {
      gs_set_error_from_errno (error, errno);
      goto out;
    }

  buf = g_malloc (bytes_read);
  do
    real_size = lgetxattr (full_path, attribute, buf, bytes_read);
  while (G_UNLIKELY (real_size < 0 && errno == EINTR));
  if (G_UNLIKELY (real_size < 0))
    {
      gs_set_error_from_errno (error, errno);
      g_free (buf);
      goto out;
    }

  bytes = g_bytes_new_take (buf, real_size);
 out:
  return bytes;
}

gboolean
ot_lsetxattrat (int            dfd,
                const char    *path,
                const char    *attribute,
                const void    *value,
                gsize          value_size,
                int            flags,
                GError       **error)
{
  /* A workaround for the lack of lsetxattrat(), thanks to Florian Weimer:
   * https://mail.gnome.org/archives/ostree-list/2014-February/msg00017.html
   */
  gs_free char *full_path = g_strdup_printf ("/proc/self/fd/%d/%s", dfd, path);
  int res;

  do
    res = lsetxattr (full_path, "user.ostreemeta", value, value_size, flags);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (G_UNLIKELY (res == -1))
    {
      gs_set_error_from_errno (error, errno);
      return FALSE;
    }

  return TRUE;
}
