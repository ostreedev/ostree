/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "ostree-mount-util.h"
#include "otcore.h"

static void
do_remount (const char *target, bool writable)
{
  struct stat stbuf;
  if (lstat (target, &stbuf) < 0)
    return;
  /* Silently ignore symbolic links; we expect these to point to
   * /sysroot, and thus there isn't a bind mount there.
   */
  if (S_ISLNK (stbuf.st_mode))
    return;
  /* If not a mountpoint, skip it */
  struct statvfs stvfsbuf;
  if (statvfs (target, &stvfsbuf) == -1)
    return;

  const bool currently_writable = ((stvfsbuf.f_flag & ST_RDONLY) == 0);
  if (writable == currently_writable)
    return;

  int mnt_flags = MS_REMOUNT | MS_SILENT;
  if (!writable)
    mnt_flags |= MS_RDONLY;
  if (mount (target, target, NULL, mnt_flags, NULL) < 0)
    {
      /* Also ignore EINVAL - if the target isn't a mountpoint
       * already, then assume things are OK.
       */
      if (errno != EINVAL)
        err (EXIT_FAILURE, "failed to remount(%s) %s", writable ? "rw" : "ro", target);
      else
        return;
    }

  printf ("Remounted %s: %s\n", writable ? "rw" : "ro", target);
}

int
main (int argc, char *argv[])
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ostree_run_metadata_v = NULL;
  {
    glnx_autofd int fd = open (OTCORE_RUN_BOOTED, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      {
        /* We really expect that nowadays that everything is done in the initramfs,
         * but historically we created this file here, so we'll continue to do be
         * sure here it exists.  This code should be removed at some point though.
         */
        if (errno == ENOENT)
          {
            int subfd = open (OTCORE_RUN_BOOTED, O_EXCL | O_CREAT | O_WRONLY | O_NOCTTY | O_CLOEXEC,
                              0640);
            if (subfd != -1)
              (void)close (subfd);
          }
        else
          {
            err (EXIT_FAILURE, "failed to open %s", OTCORE_RUN_BOOTED);
          }
      }
    else
      {
        if (!ot_variant_read_fd (fd, 0, G_VARIANT_TYPE_VARDICT, TRUE, &ostree_run_metadata_v,
                                 &error))
          errx (EXIT_FAILURE, "failed to read %s: %s", OTCORE_RUN_BOOTED, error->message);
      }
  }
  g_autoptr (GVariantDict) ostree_run_metadata = g_variant_dict_new (ostree_run_metadata_v);

  /* The /sysroot mount needs to be private to avoid having a mount for e.g. /var/cache
   * also propagate to /sysroot/ostree/deploy/$stateroot/var/cache
   *
   * Today systemd remounts / (recursively) as shared, so we're undoing that as early
   * as possible.  See also a copy of this in ostree-prepare-root.c.
   */
  if (mount ("none", "/sysroot", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
    perror ("warning: While remounting /sysroot MS_PRIVATE");

  gboolean root_is_composefs = FALSE;
  g_variant_dict_lookup (ostree_run_metadata, OTCORE_RUN_BOOTED_KEY_COMPOSEFS, "b",
                         &root_is_composefs);

  if (path_is_on_readonly_fs ("/") && !root_is_composefs)
    {
      /* If / isn't writable, don't do any remounts; we don't want
       * to clear the readonly flag in that case.
       */
      exit (EXIT_SUCCESS);
    }

  /* Handle remounting /sysroot; if it's explicitly marked as read-only (opt in)
   * then ensure it's readonly, otherwise mount writable, the same as /
   */
  gboolean sysroot_configured_readonly = FALSE;
  g_variant_dict_lookup (ostree_run_metadata, OTCORE_RUN_BOOTED_KEY_SYSROOT_RO, "b",
                         &sysroot_configured_readonly);
  do_remount ("/sysroot", !sysroot_configured_readonly);

  /* And also make sure to make /etc rw again. We make this conditional on
   * sysroot_configured_readonly because only in that case is it a bind-mount. */
  if (sysroot_configured_readonly)
    do_remount ("/etc", true);

  /* If /var was created as as an OSTree default bind mount (instead of being a separate
   * filesystem) then remounting the root mount read-only also remounted it. So just like /etc, we
   * need to make it read-write by default. If it was a separate filesystem, we expect it to be
   * writable anyways, so it doesn't hurt to remount it if so.
   *
   * And if we started out with a writable system root, then we need
   * to ensure that the /var bind mount created by the systemd generator
   * is writable too.
   */
  do_remount ("/var", true);

  exit (EXIT_SUCCESS);
}
