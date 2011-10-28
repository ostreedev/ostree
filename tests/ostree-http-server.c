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

#include <libsoup/soup-gnome.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>

static void
request_callback (SoupServer *server, SoupMessage *msg,
		  const char *path, GHashTable *query,
		  SoupClientContext *context, gpointer data)
{
  if (msg->method == SOUP_METHOD_GET) 
    {
      GFile *file;
      char *content;
      gsize len;
      
      /* Strip leading / */
      file = g_file_new_for_path (path + 1);
      
      if (g_file_load_contents (file, NULL, &content, &len, NULL, NULL))
	soup_message_set_response (msg, "application/octet-stream", SOUP_MEMORY_TAKE, content, len);
      else
	soup_message_set_status (msg, SOUP_STATUS_NOT_FOUND);
    }
  else
    {
      soup_message_set_status (msg, SOUP_STATUS_METHOD_NOT_ALLOWED);
    }
}

static void
on_dir_changed (GFileMonitor  *mon,
		GFile *file,
		GFile *other,
		GFileMonitorEvent  event,
		gpointer user_data)
{
  GMainLoop *loop = user_data;

  if (event == G_FILE_MONITOR_EVENT_DELETED)
    g_main_loop_quit (loop);
}

int
main (int     argc,
      char  **argv)
{
  SoupAddress *addr;
  SoupServer *server;
  GMainLoop *loop;
  GFileMonitor *monitor;
  GFile *curdir;
  GError *error = NULL;

  g_type_init ();

  loop = g_main_loop_new (NULL, TRUE);

  curdir = g_file_new_for_path (".");
  monitor = g_file_monitor_directory (curdir, 0, NULL, &error);
  if (!monitor)
    exit (1);
  g_signal_connect (monitor, "changed",
		    G_CALLBACK (on_dir_changed), loop);

  addr = soup_address_new ("127.0.0.1", SOUP_ADDRESS_ANY_PORT);
  soup_address_resolve_sync (addr, NULL);

  server = soup_server_new (SOUP_SERVER_INTERFACE, addr,
			    SOUP_SERVER_ASYNC_CONTEXT, g_main_loop_get_context (loop),
			    NULL);
  soup_server_add_handler (server, NULL,
			   request_callback,
			   NULL, NULL);

  soup_server_run_async (server);

  g_print ("http://127.0.0.1:%ld\n", (long)soup_server_get_port (server));

  g_main_loop_run (loop);

  return 0;
}
