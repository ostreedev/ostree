/* -*- c-file-style: "gnu" -*-
 * Switch to new root directory and start init.
 * 
 * Copyright 2011,2012,2013 Colin Walters <walters@verbum.org>
 *
 * Based on code from util-linux/sys-utils/switch_root.c, 
 * Copyright 2002-2009 Red Hat, Inc.  All rights reserved.
 * Authors:
 *	Peter Jones <pjones@redhat.com>
 *	Jeremy Katz <katzj@redhat.com>
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

static void
parse_ostree_cmdline (char **out_osname,
		      char **out_tree)
{
  FILE *f = fopen("/proc/cmdline", "r");
  char *cmdline = NULL;
  const char *iter;
  size_t len;
  if (!f)
    return;
  if (getline (&cmdline, &len, f) < 0)
    return;

  iter = cmdline;
  while (iter != NULL)
    {
      const char *next = strchr (iter, ' ');
      const char *next_nonspc = next;
      while (*next_nonspc == ' ')
	next_nonspc += 1;
      if (strncmp (iter, "ostree=", strlen ("ostree=")) == 0)
        {
          const char *slash = strchr (iter, '/');
          if (slash)
            {
              const char *start = iter + strlen ("ostree=");
              *out_osname = strndup (start, slash - start);
              if (next)
                *out_tree = strndup (slash + 1, next - slash - 1);
              else
                *out_tree = strdup (slash + 1);
              break;
            }
        }
      iter = next_nonspc;
    }

  free (cmdline);
}

int
main(int argc, char *argv[])
{
  const char *toproot_bind_mounts[] = { "/home", "/root", "/tmp", NULL };
  const char *ostree_bind_mounts[] = { "/var", NULL };
  const char *readonly_bind_mounts[] = { "/usr", NULL };
  const char *root_mountpoint = NULL;
  char *ostree_osname = NULL;
  char *ostree_target = NULL;
  char ostree_target_path[PATH_MAX];
  char *deploy_path = NULL;
  char srcpath[PATH_MAX];
  char destpath[PATH_MAX];
  struct stat stbuf;
  size_t len;
  int i;

  if (argc < 3)
    {
      fprintf (stderr, "usage: ostree-prepare-root SYSROOT OSTREE\n");
      exit (1);
    }

  root_mountpoint = argv[1];

  parse_ostree_cmdline (&ostree_osname, &ostree_target);

  if (!ostree_osname)
    {
      fprintf (stderr, "No OSTree target; expected ostree=OSNAME/TREENAME\n");
      exit (1);
    }

  /* Work-around for a kernel bug: for some reason the kernel
   * refuses switching root if any file systems are mounted
   * MS_SHARED. Hence remount them MS_PRIVATE here as a
   * work-around.
   *
   * https://bugzilla.redhat.com/show_bug.cgi?id=847418 */
  if (mount (NULL, "/", NULL, MS_REC|MS_PRIVATE, NULL) < 0)
    {
      perrorv ("Failed to make \"/\" private mount: %m");
      exit (1);
    }

  snprintf (destpath, sizeof(destpath), "%s/ostree/deploy/%s/%s",
	    root_mountpoint, ostree_osname, ostree_target);
  fprintf (stderr, "Examining %s\n", destpath);
  if (lstat (destpath, &stbuf) < 0)
    {
      perrorv ("Couldn't find specified OSTree root '%s': ", destpath);
      exit (1);
    }
  if (!S_ISLNK (stbuf.st_mode))
    {
      fprintf (stderr, "OSTree target is not a symbolic link: %s\n", destpath);
      exit (1);
    }
  if (readlink (destpath, ostree_target_path, PATH_MAX) < 0)
    {
      perrorv ("readlink(%s) failed: ", destpath);
      exit (1);
    }
  len = strlen (ostree_target_path);
  if (ostree_target_path[len-1] == '/')
    ostree_target_path[len-1] = '\0';
  fprintf (stderr, "Resolved OSTree target to: %s\n", ostree_target_path);
  (void) asprintf (&deploy_path, "%s/ostree/deploy/%s/%s", root_mountpoint,
		   ostree_osname, ostree_target_path);
  
  /* Make deploy_path a bind mount, so we can move it later */
  if (mount (deploy_path, deploy_path, NULL, MS_BIND, NULL) < 0)
    {
      perrorv ("failed to initial bind mount %s", deploy_path);
      exit (1);
    }

  snprintf (destpath, sizeof(destpath), "%s/sysroot", deploy_path);
  if (mount (root_mountpoint, destpath, NULL, MS_BIND, NULL) < 0)
    {
      perrorv ("Failed to bind mount %s to '%s'", root_mountpoint, destpath);
      exit (1);
    }

  snprintf (srcpath, sizeof(srcpath), "%s-etc", deploy_path);
  snprintf (destpath, sizeof(destpath), "%s/etc", deploy_path);
  if (mount (srcpath, destpath, NULL, MS_BIND, NULL) < 0)
    {
      perrorv ("Failed to bind mount '%s' to '%s'", srcpath, destpath);
      exit (1);
    }

  for (i = 0; toproot_bind_mounts[i] != NULL; i++)
    {
      snprintf (srcpath, sizeof(srcpath), "%s%s", root_mountpoint, toproot_bind_mounts[i]);
      snprintf (destpath, sizeof(destpath), "%s%s", deploy_path, toproot_bind_mounts[i]);
      if (mount (srcpath, destpath, NULL, MS_BIND & ~MS_RDONLY, NULL) < 0)
	{
	  perrorv ("failed to bind mount (class:toproot) %s to %s", toproot_bind_mounts[i], destpath);
	  exit (1);
	}
    }

  for (i = 0; ostree_bind_mounts[i] != NULL; i++)
    {
      snprintf (srcpath, sizeof(srcpath), "%s/ostree/deploy/%s%s", root_mountpoint,
		ostree_osname, ostree_bind_mounts[i]);
      snprintf (destpath, sizeof(destpath), "%s%s", deploy_path, ostree_bind_mounts[i]);
      if (mount (srcpath, destpath, NULL, MS_MGC_VAL|MS_BIND, NULL) < 0)
	{
	  perrorv ("failed to bind mount (class:bind) %s to %s", srcpath, destpath);
	  exit (1);
	}
    }

  for (i = 0; readonly_bind_mounts[i] != NULL; i++)
    {
      snprintf (destpath, sizeof(destpath), "%s%s", deploy_path, readonly_bind_mounts[i]);
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

  /* This is a bit hacky - move our deployment to /sysroot, since
   * systemd's initrd-switch-root target hardcodes looking for it
   * there.
   */
  if (mount (deploy_path, root_mountpoint, NULL, MS_MOVE, NULL) < 0)
    {
      perrorv ("failed to MS_MOVE %s to %s", deploy_path, root_mountpoint);
      exit (1);
    }
  
  exit (0);
}

