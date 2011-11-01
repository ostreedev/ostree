/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Kill a child process when the current directory is deleted
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

#include <gio/gio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

struct TmpdirLifecyleData {
  GMainLoop *loop;
  GPid pid;
  gboolean exited;
};

static void
on_dir_changed (GFileMonitor  *mon,
		GFile *file,
		GFile *other,
		GFileMonitorEvent  event,
		gpointer user_data)
{
  struct TmpdirLifecyleData *data = user_data;

  if (event == G_FILE_MONITOR_EVENT_DELETED)
    g_main_loop_quit (data->loop);
}

static void
on_child_exited (GPid  pid,
                 int status,
                 gpointer user_data)
{
  struct TmpdirLifecyleData *data = user_data;

  data->exited = TRUE;
  g_main_loop_quit (data->loop);
}

int
main (int     argc,
      char  **argv)
{
  GFileMonitor *monitor;
  GFile *curdir;
  GError *error = NULL;
  GPtrArray *new_argv;
  int i;
  GPid pid;
  struct TmpdirLifecyleData data;

  g_type_init ();

  memset (&data, 0, sizeof (data));

  data.loop = g_main_loop_new (NULL, TRUE);

  curdir = g_file_new_for_path (".");
  monitor = g_file_monitor_directory (curdir, 0, NULL, &error);
  if (!monitor)
    exit (1);
  g_signal_connect (monitor, "changed",
		    G_CALLBACK (on_dir_changed), &data);

  new_argv = g_ptr_array_new ();
  for (i = 1; i < argc; i++)
    g_ptr_array_add (new_argv, argv[i]);
  g_ptr_array_add (new_argv, NULL);

  if (!g_spawn_async (NULL, (char**)new_argv->pdata, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                      NULL, NULL, &pid, &error))
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }
  g_child_watch_add (pid, on_child_exited, &data);

  g_main_loop_run (data.loop);

  if (!data.exited)
    kill (data.pid, SIGTERM);

  return 0;
}
