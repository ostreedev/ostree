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
 * Relicensed with permission to LGPLv2+.
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
 */

#include "config.h"

#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "ostree-mount-util.h"

static char *
parse_ostree_cmdline (void)
{
  FILE *f = fopen("/proc/cmdline", "r");
  char *cmdline = NULL;
  const char *iter;
  char *ret = NULL;
  size_t len;

  if (!f)
    return NULL;
  /* Note that /proc/cmdline will not end in a newline, so getline
   * will fail unelss we provide a length.
   */
  if (getline (&cmdline, &len, f) < 0)
    return NULL;
  /* ... but the length will be the size of the malloc buffer, not
   * strlen().  Fix that.
   */
  len = strlen (cmdline);

  if (cmdline[len-1] == '\n')
    cmdline[len-1] = '\0';

  iter = cmdline;
  while (iter != NULL)
    {
      const char *next = strchr (iter, ' ');
      const char *next_nonspc = next;
      while (next_nonspc && *next_nonspc == ' ')
	next_nonspc += 1;
      if (strncmp (iter, "ostree=", strlen ("ostree=")) == 0)
        {
	  const char *start = iter + strlen ("ostree=");
	  if (next)
	    ret = strndup (start, next - start);
	  else
	    ret = strdup (start);
	  break;
        }
      iter = next_nonspc;
    }

  free (cmdline);
  return ret;
}

/* This is an API for other projects to determine whether or not the
 * currently running system is ostree-controlled.
 */
static void
touch_run_ostree (void)
{
  int fd;
  
  fd = open ("/run/ostree-booted", O_CREAT | O_WRONLY | O_NOCTTY, 0640);
  /* We ignore failures here in case /run isn't mounted...not much we
   * can do about that, but we don't want to fail.
   */
  if (fd == -1)
    return;
  (void) close (fd);
}

int
main(int argc, char *argv[])
{
  const char *readonly_bind_mounts[] = { "/usr", NULL };
  const char *root_mountpoint = NULL;
  char *ostree_target = NULL;
  char *deploy_path = NULL;
  char srcpath[PATH_MAX];
  char destpath[PATH_MAX];
  struct stat stbuf;
  int i;

  if (argc < 2)
    {
      fprintf (stderr, "usage: ostree-prepare-root SYSROOT\n");
      exit (EXIT_FAILURE);
    }

  root_mountpoint = argv[1];

  ostree_target = parse_ostree_cmdline ();
  if (!ostree_target)
    {
      fprintf (stderr, "No OSTree target; expected ostree=/ostree/boot.N/...\n");
      exit (EXIT_FAILURE);
    }

  snprintf (destpath, sizeof(destpath), "%s/%s", root_mountpoint, ostree_target);
  fprintf (stderr, "Examining %s\n", destpath);
  if (lstat (destpath, &stbuf) < 0)
    {
      perrorv ("Couldn't find specified OSTree root '%s': ", destpath);
      exit (EXIT_FAILURE);
    }
  if (!S_ISLNK (stbuf.st_mode))
    {
      fprintf (stderr, "OSTree target is not a symbolic link: %s\n", destpath);
      exit (EXIT_FAILURE);
    }
  deploy_path = realpath (destpath, NULL);
  if (deploy_path == NULL)
    {
      perrorv ("realpath(%s) failed: ", destpath);
      exit (EXIT_FAILURE);
    }
  fprintf (stderr, "Resolved OSTree target to: %s\n", deploy_path);
  
  /* Work-around for a kernel bug: for some reason the kernel
   * refuses switching root if any file systems are mounted
   * MS_SHARED. Hence remount them MS_PRIVATE here as a
   * work-around.
   *
   * https://bugzilla.redhat.com/show_bug.cgi?id=847418 */
  if (mount (NULL, "/", NULL, MS_REC|MS_PRIVATE, NULL) < 0)
    {
      perrorv ("Failed to make \"/\" private mount: %m");
      exit (EXIT_FAILURE);
    }

  /* Make deploy_path a bind mount, so we can move it later */
  if (mount (deploy_path, deploy_path, NULL, MS_BIND, NULL) < 0)
    {
      perrorv ("failed to initial bind mount %s", deploy_path);
      exit (EXIT_FAILURE);
    }

  snprintf (destpath, sizeof(destpath), "%s/sysroot", deploy_path);
  if (mount (root_mountpoint, destpath, NULL, MS_BIND, NULL) < 0)
    {
      perrorv ("Failed to bind mount %s to '%s'", root_mountpoint, destpath);
      exit (EXIT_FAILURE);
    }

  snprintf (srcpath, sizeof(srcpath), "%s/../../var", deploy_path);
  snprintf (destpath, sizeof(destpath), "%s/var", deploy_path);
  if (mount (srcpath, destpath, NULL, MS_MGC_VAL|MS_BIND, NULL) < 0)
    {
      perrorv ("failed to bind mount %s to %s", srcpath, destpath);
      exit (EXIT_FAILURE);
    }

  for (i = 0; readonly_bind_mounts[i] != NULL; i++)
    {
      snprintf (destpath, sizeof(destpath), "%s%s", deploy_path, readonly_bind_mounts[i]);
      if (mount (destpath, destpath, NULL, MS_BIND, NULL) < 0)
	{
	  perrorv ("failed to bind mount (class:readonly) %s", destpath);
	  exit (EXIT_FAILURE);
	}
      if (mount (destpath, destpath, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0)
	{
	  perrorv ("failed to bind mount (class:readonly) %s", destpath);
	  exit (EXIT_FAILURE);
	}
    }

  touch_run_ostree ();

  /* This is a bit hacky - move our deployment to /sysroot, since
   * systemd's initrd-switch-root target hardcodes looking for it
   * there.
   */
  if (mount (deploy_path, root_mountpoint, NULL, MS_MOVE, NULL) < 0)
    {
      perrorv ("failed to MS_MOVE %s to %s", deploy_path, root_mountpoint);
      exit (EXIT_FAILURE);
    }
  
  exit (EXIT_SUCCESS);
}

