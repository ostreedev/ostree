/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "ostree-mount-util.h"

/* Having a writeable /var is necessary for full system functioning.
 * If /var isn't writeable, we mount tmpfs over it. While this is
 * somewhat outside of ostree's scope, having all /var twiddling
 * in one place will make future maintenance easier.
 */
static void
maybe_mount_tmpfs_on_var (void)
{
  if (!path_is_on_readonly_fs ("/var"))
    return;

  if (umount ("/var") < 0 && errno != EINVAL)
    {
      perror ("failed to unmount /var prior to mounting tmpfs, mounting over");
    }

  if (mount ("tmpfs", "/var", "tmpfs", 0, NULL) < 0)
    {
      perror ("failed to mount tmpfs on /var");
      exit (EXIT_FAILURE);
    }
}

int
main(int argc, char *argv[])
{
  const char *remounts[] = { "/sysroot", "/etc", "/home", "/root", "/tmp", "/var", NULL };
  struct stat stbuf;
  int i;

  if (path_is_on_readonly_fs ("/"))
    {
      /* If / isn't writable, don't do any remounts; we don't want
       * to clear the readonly flag in that case.
       */

      maybe_mount_tmpfs_on_var ();

      exit (EXIT_SUCCESS);
    }

  for (i = 0; remounts[i] != NULL; i++)
    {
      const char *target = remounts[i];
      if (lstat (target, &stbuf) < 0)
        continue;
      /* Silently ignore symbolic links; we expect these to point to
       * /sysroot, and thus there isn't a bind mount there.
       */
      if (S_ISLNK (stbuf.st_mode))
        continue;
      if (mount (target, target, NULL, MS_REMOUNT | MS_SILENT, NULL) < 0)
	{
          /* Also ignore ENINVAL - if the target isn't a mountpoint
           * already, then assume things are OK.
           */
          if (errno != EINVAL)
            {
              perrorv ("failed to remount %s", target);
              exit (EXIT_FAILURE);
            }
	}
    }

  maybe_mount_tmpfs_on_var ();

  exit (EXIT_SUCCESS);
}
