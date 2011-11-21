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

static char *
parse_arg (const char *cmdline, const char *arg)
{
  const char *p;
  int arglen;
  char *ret = NULL;
  int is_eq;

  arglen = strlen (arg);
  assert (arglen > 0);
  is_eq = *(arg+arglen-1) == '=';

  p = cmdline;
  while (p != NULL)
    {
      if (!strncmp (p, arg, arglen))
	{
	  const char *start = p + arglen;
	  const char *end = strchr (start, ' ');

	  if (!end)
	    end = strchr (start, '\n');

	  if (is_eq)
	    {
	      if (end)
		ret = strndup (start, end - start);
	      else
		ret = strdup (start);
	    }
	  else if (!end || end == start)
	    {
	      ret = strdup (arg);
	    }
	  break;
	}
      p = strchr (p, ' ');
      if (p)
	p += 1;
    }
  return ret;
}

static char *
get_file_contents (const char *path, size_t *len)
{
  FILE *f = NULL;
  char *ret = NULL;
  int saved_errno;
  char *buf = NULL;
  size_t bytes_read;
  size_t buf_size;
  size_t buf_used;

  f = fopen (path, "r");
  if (!f)
    {
      saved_errno = errno;
      goto out;
    }

  buf_size = 1024;
  buf_used = 0;
  buf = malloc (buf_size);
  assert (buf);

  while ((bytes_read = fread (buf + buf_used, 1, buf_size - buf_used, f)) > 0)
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
      saved_errno = errno;
      goto out;
    }

  ret = buf;
  buf = NULL;
  *len = buf_used;
 out:
  if (f)
    fclose (f);
  free (buf);
  errno = saved_errno;
  return ret;
}

int
main(int argc, char *argv[])
{
  const char *toproot_bind_mounts[] = { "/home", "/root", "/tmp", NULL };
  const char *ostree_bind_mounts[] = { "/var", NULL };
  const char *readonly_bind_mounts[] = { "/bin", "/etc", "/lib", "/sbin", "/usr",
					 NULL };
  char *ostree_root = NULL;
  char *ostree_subinit = NULL;
  char srcpath[PATH_MAX];
  char destpath[PATH_MAX];
  struct stat stbuf;
  char **init_argv = NULL;
  char *cmdline = NULL;
  size_t len;
  int i;
  int mounted_proc = 0;
  char *tmp;
  int readonly;

  cmdline = get_file_contents ("/proc/cmdline", &len);
  if (!cmdline)
    {
      if (mount ("proc", "/proc", "proc", 0, NULL) < 0)
	{
	  perrorv ("Failed to mount /proc");
	  return 1;
	}
      cmdline = get_file_contents ("/proc/cmdline", &len);
      if (!cmdline)
	{
	  perrorv ("Failed to read /proc/cmdline");
	  return 1;
	}
    }

  fprintf (stderr, "ostree-init kernel cmdline: %s\n", cmdline);
  fflush (stderr);

  ostree_root = parse_arg (cmdline, "ostree=");
  ostree_subinit = parse_arg (cmdline, "ostree-subinit=");

  tmp = parse_arg (cmdline, "ro");
  readonly = tmp != NULL;
  free (tmp);

  if (!ostree_root)
    {
      fprintf (stderr, "No ostree= argument specified\n");
      exit (1);
    }

  if (!readonly)
    {
      if (mount ("/dev/root", "/", NULL, MS_MGC_VAL|MS_REMOUNT, NULL) < 0)
	{
	  perrorv ("Failed to remount / read/write");
	  exit (1);
	}
    }

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

  snprintf (destpath, sizeof(destpath), "/ostree/%s/dev", ostree_root);
  if (mount ("udev", destpath, "devtmpfs",
	     MS_MGC_VAL | MS_NOSUID,
	     "seclabel,relatime,size=1960040k,nr_inodes=49010,mode=755") < 0)
    {
      perrorv ("Failed to mount devtmpfs on '%s'", destpath);
      exit (1);
    }

  for (i = 0; toproot_bind_mounts[i] != NULL; i++)
    {
      snprintf (destpath, sizeof(destpath), "/ostree/%s%s", ostree_root, toproot_bind_mounts[i]);
      if (mount (toproot_bind_mounts[i], destpath, NULL, MS_BIND & ~MS_RDONLY, NULL) < 0)
	{
	  perrorv ("failed to bind mount (class:toproot) %s to %s", toproot_bind_mounts[i], destpath);
	  exit (1);
	}
    }

  for (i = 0; ostree_bind_mounts[i] != NULL; i++)
    {
      snprintf (srcpath, sizeof(srcpath), "/ostree/%s", ostree_bind_mounts[i]);
      snprintf (destpath, sizeof(destpath), "/ostree/%s%s", ostree_root, ostree_bind_mounts[i]);
      if (mount (srcpath, destpath, NULL, MS_MGC_VAL|MS_BIND, NULL) < 0)
	{
	  perrorv ("failed to bind mount (class:bind) %s to %s", srcpath, destpath);
	  exit (1);
	}
    }

  for (i = 0; readonly_bind_mounts[i] != NULL; i++)
    {
      snprintf (destpath, sizeof(destpath), "/ostree/%s%s", ostree_root, readonly_bind_mounts[i]);
      if (mount (destpath, destpath, NULL, MS_BIND, NULL) < 0)
	{
	  perrorv ("failed to bind mount (class:readonly) %s", destpath);
	  exit (1);
	}
      if (mount (destpath, destpath, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0)
	{
	  perrorv ("failed to bind mount (class:readonly) %s", destpath);
	  exit (1);
	}
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

  if (mounted_proc)
    (void)umount ("/proc");

  init_argv = malloc (sizeof (char*)*(argc+1));
  if (ostree_subinit)
    init_argv[0] = ostree_subinit;
  else
    init_argv[0] = INIT_PATH;
  for (i = 1; i < argc; i++)
    init_argv[i] = argv[i];
  init_argv[i] = NULL;
  
  fprintf (stderr, "ostree-init: Running real init %s (argc=%d)\n", init_argv[0], argc);
  fflush (stderr);
  execv (init_argv[0], init_argv);
  perrorv ("Failed to exec init '%s'", INIT_PATH);
  exit (1);
}

