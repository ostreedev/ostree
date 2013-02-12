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
  const char *readonly_bind_mounts[] = { "/usr", NULL };
  const char *root_mountpoint = NULL;
  const char *ostree_target = NULL;
  const char *ostree_subinit = NULL;
  const char *p = NULL;
  char *ostree_osname = NULL;
  char ostree_target_path[PATH_MAX];
  char *deploy_path = NULL;
  char srcpath[PATH_MAX];
  char destpath[PATH_MAX];
  struct stat stbuf;
  char **init_argv = NULL;
  size_t len;
  int initramfs_fd;
  int i;
  int before_init_argc = 0;
  pid_t cleanup_pid;

  if (argc < 4)
    {
      fprintf (stderr, "usage: ostree-switch-root NEWROOT TARGET INIT [ARGS...]\n");
      exit (1);
    }

  before_init_argc++;
  root_mountpoint = argv[1];
  before_init_argc++;
  ostree_target = argv[2];
  before_init_argc++;
  ostree_subinit = argv[3];
  before_init_argc++;

  p = strchr (ostree_target, '/');
  if (p == NULL)
    {
      fprintf (stderr, "Malformed OSTree target %s; expected OSNAME/TREENAME\n", ostree_target);
      exit (1);
    }
  ostree_osname = strndup (ostree_target, p - ostree_target);

  snprintf (destpath, sizeof(destpath), "%s/ostree/deploy/%s",
	    root_mountpoint, ostree_target);
  if (stat (destpath, &stbuf) < 0)
    {
      perrorv ("Invalid ostree root '%s'", destpath);
      exit (1);
    }

  /* Work-around for a kernel bug: for some reason the kernel
   * refuses switching root if any file systems are mounted
   * MS_SHARED. Hence remount them MS_PRIVATE here as a
   * work-around.
   *
   * https://bugzilla.redhat.com/show_bug.cgi?id=847418 */
  if (mount(NULL, "/", NULL, MS_REC|MS_PRIVATE, NULL) < 0)
    {
      perrorv ("mount(/, MS_PRIVATE): ");
      exit (1);
    }

  initramfs_fd = open ("/", O_RDONLY);

  for (i = 0; initramfs_move_mounts[i] != NULL; i++)
    {
      const char *path = initramfs_move_mounts[i];
      snprintf (destpath, sizeof(destpath), "%s/ostree/deploy/%s%s", root_mountpoint, ostree_target, path);
      if (mount (path, destpath, NULL, MS_MOVE, NULL) < 0)
	{
	  perrorv ("failed to move mount of %s to %s", path, destpath);
	  exit (1);
	}
    }

  snprintf (destpath, sizeof(destpath), "%s/ostree/deploy/%s", root_mountpoint, ostree_target);
  fprintf (stderr, "Examining %s\n", destpath);
  if (lstat (destpath, &stbuf) < 0)
    {
      perrorv ("Second stat of ostree root '%s' failed: ", destpath);
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
  asprintf (&deploy_path, "%s/ostree/deploy/%s/%s", root_mountpoint,
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

  if (chdir (deploy_path) < 0)
    {
      perrorv ("failed to chdir to subroot (initial)");
      exit (1);
    }

  if (mount (deploy_path, "/", NULL, MS_MOVE, NULL) < 0)
    {
      perrorv ("failed to MS_MOVE %s to /", deploy_path);
      exit (1);
    }

  if (chroot (".") < 0)
    {
      perrorv ("failed to change root to '%s'", deploy_path);
      exit (1);
    }

  if (chdir ("/") < 0)
    {
      perrorv ("failed to chdir to / (after MS_MOVE of /)");
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

