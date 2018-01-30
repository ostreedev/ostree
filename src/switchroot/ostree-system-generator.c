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
  /* Important: if this isn't an ostree-booted system, do nothing; people could
   * have the package installed as a dependency for flatpak or whatever.
   */
  { struct stat stbuf;
    if (fstatat (AT_FDCWD, "/run/ostree-booted", &stbuf, 0) < 0)
      exit (EXIT_SUCCESS);
  }

  if (argc > 1 && argc != 4)
    errx (EXIT_FAILURE, "This program takes three or no arguments");

  if (argc > 1)
    arg_dest = argv[1];
  if (argc > 3)
    arg_dest_late = argv[3];

  char *ostree_cmdline = read_proc_cmdline_ostree ();
  if (!ostree_cmdline)
    errx (EXIT_FAILURE, "Failed to find ostree= kernel argument");

  { g_autoptr(GError) local_error = NULL;
    if (!ostree_cmd__private__()->ostree_system_generator (ostree_cmdline, arg_dest, NULL, arg_dest_late, &local_error))
      errx (EXIT_FAILURE, "%s", local_error->message);
  }

  exit (EXIT_SUCCESS);
}
