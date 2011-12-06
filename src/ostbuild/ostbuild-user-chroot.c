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
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <linux/securebits.h>
#include <sched.h>

static void fatal (const char *message, ...) __attribute__ ((noreturn)) __attribute__ ((format (printf, 1, 2)));
static void fatal_errno (const char *message) __attribute__ ((noreturn));

static void
fatal (const char *fmt,
       ...)
{
  va_list args;
  
  va_start (args, fmt);

  vfprintf (stderr, fmt, args);
  putc ('\n', stderr);
  
  va_end (args);
  exit (1);
}

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
  const char *argv0;
  const char *chroot_dir;
  const char *program;
  uid_t ruid, euid, suid;
  gid_t rgid, egid, sgid;
  int after_bind_arg_index;
  int i;
  char **program_argv;
  char **argv_iter;

  if (argc <= 0)
    return 1;

  argv0 = argv[0];
  argc--;
  argv++;

  if (argc < 1)
    fatal ("ROOTDIR argument must be specified");

  after_bind_arg_index = 0;
  argv_iter = argv;
  while (after_bind_arg_index < argc
         && strcmp (argv[after_bind_arg_index], "--bind") == 0)
    {
      if ((argc - after_bind_arg_index) < 3)
        fatal ("--bind takes two arguments");
      after_bind_arg_index += 3;
      argv_iter += 3;
    }

  if ((argc - after_bind_arg_index) < 2)
    fatal ("usage: %s [--bind SOURCE DEST] ROOTDIR PROGRAM ARGS...", argv0);
  chroot_dir = argv[after_bind_arg_index];
  program = argv[after_bind_arg_index+1];
  program_argv = argv + after_bind_arg_index + 1;

  if (getresgid (&rgid, &egid, &sgid) < 0)
    fatal_errno ("getresgid");
  if (getresuid (&ruid, &euid, &suid) < 0)
    fatal_errno ("getresuid");

  if (ruid == 0)
    fatal ("error: ruid is 0");
  if (rgid == 0)
    rgid = ruid;

  /* Ensure we can't execute setuid programs.  See prctl(2) and
   * capabilities(7).
   *
   * This closes the main historical reason why only uid 0 can
   * chroot(2) - because unprivileged users can create hard links to
   * setuid binaries, and possibly confuse them into looking at data
   * (or loading libraries) that they don't expect, and thus elevating
   * privileges.
   */
  if (prctl (PR_SET_SECUREBITS,
	     SECBIT_NOROOT | SECBIT_NOROOT_LOCKED) < 0)
    fatal_errno ("prctl (SECBIT_NOROOT)");

  /* This call makes it so that when we create bind mounts, we're only
   * affecting our children, not the entire system.  This way it's
   * harmless to bind mount e.g. /proc over an arbitrary directory.
   */
  if (unshare (CLONE_NEWNS) < 0)
    fatal_errno ("unshare (CLONE_NEWNS)");

  /* This is necessary to undo the damage "sandbox" creates on Fedora
   * by making / a shared mount instead of private.  This isn't
   * totally correct because the targets for our bind mounts may still
   * be shared, but really, Fedora's sandbox is broken.
   */
  if (mount ("/", "/", "none", MS_PRIVATE, NULL) < 0)
    fatal_errno ("mount(/, MS_PRIVATE)");

  /* Now let's set up our bind mounts */
  for (i = 0; i < after_bind_arg_index; i += 3)
    {
      const char *bind_arg = argv[0]; /* --bind */
      const char *bind_source = argv[i+1];
      const char *bind_target = argv[i+2];
      char *bind_abs_target;

      assert (strcmp (bind_arg, "--bind") == 0);

      asprintf (&bind_abs_target, "%s%s", chroot_dir, bind_target);
      if (mount (bind_source, bind_abs_target, NULL, MS_BIND | MS_PRIVATE, NULL) < 0)
        fatal_errno ("mount (MS_BIND)");
      free (bind_abs_target);
    }

  /* Actually perform the chroot. */
  if (chroot (chroot_dir) < 0)
    fatal_errno ("chroot");
  if (chdir ("/") < 0)
    fatal_errno ("chdir");

  /* Switch back to the uid of our invoking process.  These calls are
   * irrevocable - see setuid(2) */
  if (setgid (rgid) < 0)
    fatal_errno ("setgid");
  if (setuid (ruid) < 0)
    fatal_errno ("setuid");

  /* Finally, run the given child program. */
  if (execv (program, program_argv) < 0)
    fatal_errno ("execv");
  
  return 1;
}
