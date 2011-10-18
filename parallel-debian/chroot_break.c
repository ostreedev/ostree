/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * chroot_break: Exit out of active chroot(2) if any.  Requires root privileges.
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
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>

int
usage (const char *self,
       int         ecode)
{
  fprintf (stderr, "usage: %s PROGRAM [ARGS...]\n", self);

  return ecode;
}

int
main (int  argc,
      char **argv)
{
  const char *template = "/tmp/chroot.XXXXXX";
  char *tmpdir;
  DIR *tmp;
  struct stat current;
  struct stat up;

  if (argc < 2)
    return usage (argv[0], 1);

  tmpdir = strdup (template);
  if (!mkdtemp (tmpdir))
    {
      perror ("mkdtemp");
      return 1;
    }

  tmp = opendir ("/tmp");
  if (!tmp)
    {
      perror ("opening /tmp");
      return 1;
    }

  if (chroot (tmpdir) < 0)
    {
      perror ("chroot into tempdir");
      return 1;
    }

  do 
    {
      if (stat (".", &current) < 0)
	{
	  perror ("stat");
	  return 1;
	}
      if (stat ("..", &up) < 0)
	{
	  perror ("stat");
	  return 1;
	}
      if (current.st_dev == up.st_dev
	  && current.st_ino == up.st_ino)
	break;
      if (chdir ("..") < 0)
	{
	  perror ("chdir");
	  return 1;
	}
    } while (1);

  if (chroot (".") < 0)
    {
      perror ("chroot into real root");
      return 1;
    }

  if (unlinkat (dirfd (tmp), strrchr (tmpdir, '/') + 1, AT_REMOVEDIR) < 0)
    {
      perror ("cleaning up tmpdir");
      return 1;
    }

  closedir (tmp);

  if (execv (argv[1], &argv[1]) < 0)
    {
      perror ("Running child process");
      return 1;
    }
  /* Should not be reached */
  return 1;
}
