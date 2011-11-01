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

#include <gio/gio.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>

/* Taken from gnome-user-share src/httpd.c under the GPLv2 */
static int
get_port (void)
{
  int sock;
  int saved_errno;
  struct sockaddr_in addr;
  int reuse;
  socklen_t len;

  sock = socket (PF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    {
      return -1;
    }
  
  memset (&addr, 0, sizeof (addr));
  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);

  reuse = 1;
  setsockopt (sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof (reuse));
  if (bind (sock, (struct sockaddr *)&addr, sizeof (addr)) == -1)
    {
      saved_errno = errno;
      close (sock);
      errno = saved_errno;
      return -1;
    }

  len = sizeof (addr);
  if (getsockname (sock, (struct sockaddr *)&addr, &len) == -1)
    {
      saved_errno = errno;
      close (sock);
      errno = saved_errno;
      return -1;
    }

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__APPLE__)
  /* XXX This exposes a potential race condition, but without this,
   * httpd will not start on the above listed platforms due to the fact
   * that SO_REUSEADDR is also needed when Apache binds to the listening
   * socket.  At this time, Apache does not support that socket option.
   */
  close (sock);
#endif
  return ntohs (addr.sin_port);
}

static const char *known_httpd_modules_locations [] = {
  "/usr/libexec/apache2",
  "/usr/lib/apache2/modules",
  "/usr/lib64/httpd/modules",
  "/usr/lib/httpd/modules",
  NULL
};

static gchar*
get_httpd_modules_path ()
{
  int i;

  for (i = 0; known_httpd_modules_locations[i]; i++)
    {
      if (g_file_test (known_httpd_modules_locations[i], G_FILE_TEST_IS_EXECUTABLE)
	  && g_file_test (known_httpd_modules_locations[i], G_FILE_TEST_IS_DIR))
	{
	  return g_strdup (known_httpd_modules_locations[i]);
	}
    }
  return NULL;
}

int
main (int     argc,
      char  **argv)
{
  int port;
  char *listen;
  char *address_string;
  GError *error = NULL;
  GPtrArray *httpd_argv;
  char *modules;

  if (argc != 3)
    {
      fprintf (stderr, "usage: run-apache CONF PORTFILE");
      return 1;
    }

  g_type_init ();

  port = get_port ();
  if (port == -1)
    {
      perror ("Failed to bind port");
      return 1;
    }

  httpd_argv = g_ptr_array_new ();
  g_ptr_array_add (httpd_argv, "httpd");
  g_ptr_array_add (httpd_argv, "-f");
  g_ptr_array_add (httpd_argv, argv[1]);
  g_ptr_array_add (httpd_argv, "-C");
  listen = g_strdup_printf ("Listen 127.0.0.1:%d", port);
  g_ptr_array_add (httpd_argv, listen);
  g_ptr_array_add (httpd_argv, NULL);

  address_string = g_strdup_printf ("http://127.0.0.1:%d\n", port);
  
  if (!g_file_set_contents (argv[2], address_string, -1, &error))
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }

  setenv ("LANG", "C", 1);
  modules = get_httpd_modules_path ();
  if (modules == NULL)
    {
      g_printerr ("Failed to find httpd modules\n");
      return 1;
    }
  if (symlink (modules, "modules") < 0)
    {
      perror ("failed to make modules symlink");
      return 1;
    }
  execvp ("httpd", (char**)httpd_argv->pdata);
  perror ("Failed to run httpd");
  return 1;
}
