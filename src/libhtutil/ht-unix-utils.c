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

#include "ht-unix-utils.h"

#include <glib-unix.h>
#include <gio/gio.h>

#include <string.h>
#include <sys/types.h>
#include <dirent.h>

void
ht_util_set_error_from_errno (GError **error,
                              gint     saved_errno)
{
  g_set_error_literal (error,
                       G_UNIX_ERROR,
                       0,
                       g_strerror (saved_errno));
  errno = saved_errno;
}

int
ht_util_open_file_read (const char *path, GError **error)
{
  char *dirname = NULL;
  char *basename = NULL;
  DIR *dir = NULL;
  int fd = -1;

  dirname = g_path_get_dirname (path);
  basename = g_path_get_basename (path);
  dir = opendir (dirname);
  if (dir == NULL)
    {
      ht_util_set_error_from_errno (error, errno);
      goto out;
    }

  fd = ht_util_open_file_read_at (dirfd (dir), basename, error);

 out:
  g_free (basename);
  g_free (dirname);
  if (dir != NULL)
    closedir (dir);
  return fd;
}

int
ht_util_open_file_read_at (int dirfd, const char *name, GError **error)
{
  int fd;
  int flags = O_RDONLY;
  
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOATIME
  flags |= O_NOATIME;
#endif
  fd = openat (dirfd, name, flags);
  if (fd < 0)
    ht_util_set_error_from_errno (error, errno);
  return fd;
}
