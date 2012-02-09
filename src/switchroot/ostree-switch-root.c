/* -*- c-file-style: "gnu" -*-
 * Switch to new root directory and start init.
 * 
 * Copyright 2011,2012 Colin Walters <walters@verbum.org>
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

/* remove all files/directories below dirName -- don't cross mountpoints */
static int
recursive_remove (int fd)
{
  struct stat rb;
  DIR *dir;
  int rc = -1;
  int dfd;

  if (!(dir = fdopendir (fd)))
    {
      perrorv ("failed to open directory");
      goto done;
    }

  /* fdopendir() precludes us from continuing to use the input fd */
  dfd = dirfd (dir);

  if (fstat(dfd, &rb))
    {
      perrorv("failed to stat directory");
      goto done;
    }

  while (1)
    {
      struct dirent *d;

      errno = 0;
      if (!(d = readdir (dir)))
	{
	  if (errno)
	    {
	      perrorv ("failed to read directory");
	      goto done;
	    }
	  break;	/* end of directory */
	}
      
      if (!strcmp (d->d_name, ".") || !strcmp (d->d_name, ".."))
	continue;

      if (d->d_type == DT_DIR)
	{
	  struct stat sb;
	  
	  if (fstatat (dfd, d->d_name, &sb, AT_SYMLINK_NOFOLLOW))
	    {
	      perrorv ("failed to stat %s", d->d_name);
	      continue;
	    }

	  /* remove subdirectories if device is same as dir */
	  if (sb.st_dev == rb.st_dev)
	    {
	      int cfd;

	      cfd = openat (dfd, d->d_name, O_RDONLY);
	      if (cfd >= 0)
		{
		  recursive_remove (cfd);
		  close (cfd);
		}
	    }
	  else
	    {
	      continue;
	    }
	}

      if (unlinkat (dfd, d->d_name,
		    d->d_type == DT_DIR ? AT_REMOVEDIR : 0))
	perrorv ("failed to unlink %s", d->d_name);
    }

  rc = 0;	/* success */
  
 done:
  if (dir)
    closedir (dir);
  return rc;
}

int
main(int argc, char *argv[])
{
  const char *initramfs_move_mounts[] = { "/dev", "/proc", "/sys", "/run", NULL };
  const char *toproot_bind_mounts[] = { "/home", "/root", "/tmp", NULL };
  const char *ostree_bind_mounts[] = { "/var", NULL };
  const char *readonly_bind_mounts[] = { "/bin", "/etc", "/lib", "/sbin", "/usr",
					 NULL };
  const char *root_mountpoint = NULL;
  const char *ostree_target = NULL;
  const char *ostree_subinit = NULL;
  char srcpath[PATH_MAX];
  char destpath[PATH_MAX];
  struct stat stbuf;
  char **init_argv = NULL;
  int initramfs_fd;
  int i;
  int before_init_argc = 0;
  pid_t cleanup_pid;

  if (argc < 3)
    {
      fprintf (stderr, "usage: ostree-switch-root NEWROOT INIT [ARGS...]\n");
      exit (1);
    }

  before_init_argc++;
  root_mountpoint = argv[1];
  before_init_argc++;
  ostree_target = argv[2];
  before_init_argc++;
  ostree_subinit = argv[3];
  before_init_argc++;

  /* For now, we just remount the root filesystem read/write.  This is
   * kind of ugly, but to do this properly we'd basically have to have
   * to be fully integrated into the init process.
   */
  if (mount (NULL, root_mountpoint, NULL, MS_MGC_VAL|MS_REMOUNT, NULL) < 0)
    {
      perrorv ("Failed to remount %s read/write", root_mountpoint);
      exit (1);
    }

  snprintf (destpath, sizeof(destpath), "%s/ostree/%s",
	    root_mountpoint, ostree_target);
  if (stat (destpath, &stbuf) < 0)
    {
      perrorv ("Invalid ostree root '%s'", destpath);
      exit (1);
    }
  
  for (i = 0; initramfs_move_mounts[i] != NULL; i++)
    {
      const char *path = initramfs_move_mounts[i];
      snprintf (srcpath, sizeof(srcpath), path);
      snprintf (destpath, sizeof(destpath), "%s/ostree/%s%s", root_mountpoint, ostree_target, path);
      if (mount (srcpath, destpath, NULL, MS_MOVE, NULL) < 0)
	{
	  perrorv ("failed to move mount of %s to %s", srcpath, destpath);
	  exit (1);
	}
    }

  if (chdir (root_mountpoint) < 0)
    {
      perrorv ("failed to chdir to %s", root_mountpoint);
      exit (1);
    }

  initramfs_fd = open ("/", O_RDONLY);

  if (mount (root_mountpoint, "/", NULL, MS_MOVE, NULL) < 0)
    {
      perrorv ("failed move %s to /", root_mountpoint);
      return -1;
    }

  if (chroot (".") < 0)
    {
      perrorv ("failed to chroot to .");
      exit (1);
    }
  
  if (initramfs_fd >= 0)
    {
      cleanup_pid = fork ();
      if (cleanup_pid == 0)
	{
	  recursive_remove (initramfs_fd);
	  exit (0);
	}
      close (initramfs_fd);
    }

  /* From this point on we're chrooted into the real root filesystem,
   * so we no longer refer to root_mountpoint.
   */
  
  snprintf (destpath, sizeof(destpath), "/ostree/%s/sysroot", ostree_target);
  if (mount ("/", destpath, NULL, MS_BIND, NULL) < 0)
    {
      perrorv ("Failed to bind mount / to '%s'", destpath);
      exit (1);
    }

  snprintf (srcpath, sizeof(srcpath), "%s", "/ostree/var");
  snprintf (destpath, sizeof(destpath), "/ostree/%s/var", ostree_target);
  if (mount (srcpath, destpath, NULL, MS_BIND, NULL) < 0)
    {
      perrorv ("Failed to bind mount '%s' to '%s'", srcpath, destpath);
      exit (1);
    }

  for (i = 0; toproot_bind_mounts[i] != NULL; i++)
    {
      snprintf (destpath, sizeof(destpath), "/ostree/%s%s", ostree_target, toproot_bind_mounts[i]);
      if (mount (toproot_bind_mounts[i], destpath, NULL, MS_BIND & ~MS_RDONLY, NULL) < 0)
	{
	  perrorv ("failed to bind mount (class:toproot) %s to %s", toproot_bind_mounts[i], destpath);
	  exit (1);
	}
    }

  for (i = 0; ostree_bind_mounts[i] != NULL; i++)
    {
      snprintf (srcpath, sizeof(srcpath), "/ostree/%s", ostree_bind_mounts[i]);
      snprintf (destpath, sizeof(destpath), "/ostree/%s%s", ostree_target, ostree_bind_mounts[i]);
      if (mount (srcpath, destpath, NULL, MS_MGC_VAL|MS_BIND, NULL) < 0)
	{
	  perrorv ("failed to bind mount (class:bind) %s to %s", srcpath, destpath);
	  exit (1);
	}
    }

  for (i = 0; readonly_bind_mounts[i] != NULL; i++)
    {
      snprintf (destpath, sizeof(destpath), "/ostree/%s%s", ostree_target, readonly_bind_mounts[i]);
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
  
  snprintf (destpath, sizeof(destpath), "/ostree/%s", ostree_target);
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

  init_argv = malloc (sizeof (char*)*((argc-before_init_argc)+2));
  init_argv[0] = (char*)ostree_subinit;
  for (i = 0; i < argc-before_init_argc; i++)
    init_argv[i+1] = argv[i+before_init_argc];
  init_argv[i+1] = NULL;
  
  fprintf (stderr, "ostree-init: Running real init %s (argc=%d)\n", init_argv[0], argc-before_init_argc);
  fflush (stderr);
  execv (init_argv[0], init_argv);
  perrorv ("Failed to exec init '%s'", init_argv[0]);
  exit (1);
}

