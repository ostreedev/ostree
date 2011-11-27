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

static OstreeDaemonConfig config;

static GOptionEntry entries[] = {
  {
    "dummy-test-path", 0, 0, G_OPTION_ARG_FILENAME, &config.dummy_test_path, "Run against the given tree on the session bus", "path"},
  { NULL }
};

int
main (int    argc,
      char **argv)
{
  OstreeDaemon *daemon = NULL;
  GError *error = NULL;
  GOptionContext *context = NULL;

  g_type_init ();

  context = g_option_context_new ("- OSTree system daemon");
  g_option_context_add_main_entries (context, entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, &error))
    goto out;

  daemon = ostree_daemon_new ();

  if (!ostree_daemon_config (daemon, &config, &error))
    goto out;

  g_main_loop_run (daemon->loop);

 out:
  ostree_daemon_free (daemon);
  if (error)
    {
      g_printerr ("%s\n", error->message);
      g_clear_error (&error);
      exit (1);
    }
  return 0;
}
