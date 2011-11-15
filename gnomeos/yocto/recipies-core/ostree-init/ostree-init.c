/* -*- c-file-style: "gnu" -*-
 * ostree-init.c - switch to new root directory and start init.
 * Copyright 2011 Colin Walters <walters@verbum.org>
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#define _GNU_SOURCE
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <fcntl.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>

#define INIT_PATH "/sbin/init"

static int
perrorv (const char *format, ...) __attribute__ ((format (printf, 1, 2)));

static int
perrorv (const char *format, ...)
{
  va_list args;
  char buf[PATH_MAX];
  char *p;

  p = strerror_r (errno, buf, sizeof (buf));

  va_start (args, format);

  vfprintf (stderr, format, args);
  fprintf (stderr, ": %s\n", p);
  fflush (stderr);

  va_end (args);

  sleep (3);
	
  return 0;
}

int main(int argc, char *argv[])
{
  FILE *cmdline_f = NULL;
  char *ostree_root = NULL;
  const char *p = NULL;
  size_t bytes_read;
  size_t buf_size;
  size_t buf_used;
  char destpath[PATH_MAX];
  char *buf;
  struct stat stbuf;
  char **init_argv = NULL;
  int i;
  int mounted_proc = 0;

  cmdline_f = fopen ("/proc/cmdline", "r");
  if (!cmdline_f)
    {
      if (mount ("procs", "/proc", "proc", 0, NULL) < 0)
	{
	  perrorv ("Failed to mount /proc");
	  return 1;
	}
      mounted_proc = 1;
      cmdline_f = fopen ("/proc/cmdline", "r");
      if (!cmdline_f)
	{
	  perrorv ("Failed to open /proc/cmdline (after mounting)");
	  return 1;
	}
    }

  buf_size = 8;
  buf_used = 0;
  buf = malloc (buf_size);
  assert (buf);

  while ((bytes_read = fread (buf + buf_used, 1, buf_size - buf_used, cmdline_f)) > 0)
    {
      buf_used += bytes_read;
      if (buf_size == buf_used)
	{
	  buf_size *= 2;
	  buf = realloc (buf, buf_size);
	  assert (buf);
	}
    }
  if (bytes_read < 0)
    {
      perrorv ("Failed to read from /proc/cmdline");
      exit (1);
    }

  fprintf (stderr, "ostree-init kernel cmdline: %s\n", buf);
  fflush (stderr);
  p = buf;
  while (p != NULL)
    {
      if (!strncmp (p, "ostree=", strlen ("ostree=")))
	{
	  const char *start = p + strlen ("ostree=");
	  const char *end = strchr (start, ' ');
	  if (end)
	    ostree_root = strndup (start, end - start);
	  else
	    ostree_root = strdup (start);
	  break;
	}
      p = strchr (p, ' ');
      if (p)
	p += 1;
    }

  if (ostree_root)
    {
      snprintf (destpath, sizeof(destpath), "/ostree/%s", ostree_root);
      if (stat (destpath, &stbuf) < 0)
	{
	  perrorv ("Invalid ostree root '%s'", destpath);
	  exit (1);
	}

      snprintf (destpath, sizeof(destpath), "/ostree/%s/var", ostree_root);
      if (mount ("/ostree/var", destpath, NULL, MS_BIND, NULL) < 0)
	{
	  perrorv ("Failed to bind mount / to '%s'", destpath);
	  exit (1);
	}

      snprintf (destpath, sizeof(destpath), "/ostree/%s/sysroot", ostree_root);
      if (mount ("/", destpath, NULL, MS_BIND, NULL) < 0)
	{
	  perrorv ("Failed to bind mount / to '%s'", destpath);
	  exit (1);
	}

      snprintf (destpath, sizeof(destpath), "/ostree/%s", ostree_root);
      if (chroot (destpath) < 0)
	{
	  perrorv ("failed to change root to '%s'", destpath);
	  exit (1);
	}

      if (chdir ("/") < 0)
	{
	  perrorv ("failed to chdir to subroot");
	  exit (1);
	}
    }
  else
    {
      fprintf (stderr, "No ostree= argument specified\n");
      exit (1);
    }

  if (mounted_proc)
    (void)umount ("/proc");

  init_argv = malloc (sizeof (char*)*(argc+1));
  init_argv[0] = INIT_PATH;
  for (i = 1; i < argc; i++)
    init_argv[i] = argv[i];
  init_argv[i] = NULL;
  
  fprintf (stderr, "ostree-init: Running real init (argc=%d)\n", argc);
  fflush (stderr);
  execv (INIT_PATH, init_argv);
  perrorv ("Failed to exec init '%s'", INIT_PATH);
  exit (1);
}

