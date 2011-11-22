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

#include "ostree.h"
#include "ot-daemon.h"

#include <string.h>
#include <unistd.h>
#include <stdlib.h>

int
main (int    argc,
      char **argv)
{
  OstreeDaemon *daemon = NULL;

  g_type_init ();

  g_set_prgname (argv[0]);

  if (getuid () != 0)
    {
      g_printerr ("This program must be run as root\n");
      exit (1);
    }

  daemon = ostree_daemon_new ();

  g_main_loop_run (daemon->loop);

  return 0;
}
