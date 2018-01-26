/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
#include <err.h>
#include <errno.h>

#include "ostree-mount-util.h"

int
main(int argc, char *argv[])
{
  const char *remounts[] = { "/sysroot", "/var", NULL };
  struct stat stbuf;
  int i;

  if (path_is_on_readonly_fs ("/"))
    {
      /* If / isn't writable, don't do any remounts; we don't want
       * to clear the readonly flag in that case.
       */

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
      /* If not a mountpoint, skip it */
      struct statvfs stvfsbuf;
      if (statvfs (target, &stvfsbuf) == -1)
        continue;
      /* If no read-only flag, skip it */
      if ((stvfsbuf.f_flag & ST_RDONLY) == 0)
        continue;
      /* It's a mounted, read-only fs; remount it */
      if (mount (target, target, NULL, MS_REMOUNT | MS_SILENT, NULL) < 0)
        {
          /* Also ignore ENINVAL - if the target isn't a mountpoint
           * already, then assume things are OK.
           */
          if (errno != EINVAL)
            err (EXIT_FAILURE, "failed to remount %s", target);
        }
      else
        printf ("Remounted: %s\n", target);
    }

  exit (EXIT_SUCCESS);
}
