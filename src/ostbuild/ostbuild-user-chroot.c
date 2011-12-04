/* -*- mode: c; tab-width: 2; indent-tabs-mode: nil -*-
 *
 * user-chroot: A setuid program that allows non-root users to safely chroot(2)
 *
 * Copyright 2011 Colin Walters <walters@verbum.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <linux/securebits.h>

static void
fatal_errno (const char *message) __attribute__ ((noreturn));

static void
fatal_errno (const char *message)
{
  perror (message);
  exit (1);
}

int
main (int      argc,
      char   **argv)
{
  const char *chroot_dir;
  const char *program;
  uid_t ruid, euid, suid;
  gid_t rgid, egid, sgid;

  if (argc < 3)
    {
      fprintf (stderr, "usage: %s DIR PROGRAM ARGS...\n", argv[0]);
      exit (1);
    }
  chroot_dir = argv[1];
  program = argv[2];

  if (getresgid (&rgid, &egid, &sgid) < 0)
    fatal_errno ("getresgid");
  if (getresuid (&ruid, &euid, &suid) < 0)
    fatal_errno ("getresuid");

  if (ruid == 0)
    {
      fprintf (stderr, "error: ruid is 0\n");
      exit (1);
    }
  if (rgid == 0)
    rgid = ruid;

  /* Ensure we can't execute setuid programs - see prctl(2) and capabilities(7) */
  if (prctl (PR_SET_SECUREBITS,
	     SECBIT_NOROOT | SECBIT_NOROOT_LOCKED) < 0)
    fatal_errno ("prctl");

  if (chroot (chroot_dir) < 0)
    fatal_errno ("chroot");
  if (chdir ("/") < 0)
    fatal_errno ("chdir");

  /* These are irrevocable - see setuid(2) */
  if (setgid (rgid) < 0)
    fatal_errno ("setgid");
  if (setuid (ruid) < 0)
    fatal_errno ("setuid");

  if (execv (program, argv + 2) < 0)
    fatal_errno ("execv");
  
  return 1;
}
