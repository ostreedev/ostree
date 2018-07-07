/*
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <err.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>

#include <libglnx.h>

#include "ostree-cmdprivate.h"
#include "ostree-mount-util.h"

static const char *arg_dest = "/tmp";
static const char *arg_dest_late = "/tmp";

/* This program is a simple stub that calls the implementation that
 * lives inside libostree.
 */
int
main(int argc, char *argv[])
{
  /* We conflict with the magic ostree-mount-deployment-var file for ostree-prepare-root */
  { struct stat stbuf;
    if (fstatat (AT_FDCWD, INITRAMFS_MOUNT_VAR, &stbuf, 0) == 0)
      {
        if (unlinkat (AT_FDCWD, INITRAMFS_MOUNT_VAR, 0) < 0)
          err (EXIT_FAILURE, "Can't unlink " INITRAMFS_MOUNT_VAR);
        exit (EXIT_SUCCESS);
      }
  }

  if (argc > 1 && argc != 4)
    errx (EXIT_FAILURE, "This program takes three or no arguments");

  if (argc > 1)
    arg_dest = argv[1];
  if (argc > 3)
    arg_dest_late = argv[3];

  /* If we're installed on a system which isn't using OSTree for boot (e.g.
   * package installed as a dependency for flatpak or whatever), silently
   * exit so that we don't error, but at the same time work where switchroot
   * is PID 1 (and so hasn't created /run/ostree-booted).
   */
  char *ostree_cmdline = read_proc_cmdline_ostree ();
  if (!ostree_cmdline)
    exit (EXIT_SUCCESS);

  /* See comments in ostree-prepare-root.c for this.
   *
   * It's a lot easier for various bits of userspace to check for
   * a file versus parsing the kernel cmdline.  So let's ensure
   * the stamp file is created here too.
   */
  touch_run_ostree ();

  { g_autoptr(GError) local_error = NULL;
    if (!ostree_cmd__private__()->ostree_system_generator (ostree_cmdline, arg_dest, NULL, arg_dest_late, &local_error))
      errx (EXIT_FAILURE, "%s", local_error->message);
  }

  exit (EXIT_SUCCESS);
}
