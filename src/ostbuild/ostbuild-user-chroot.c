/* -*- mode: c; tab-width: 2; indent-tabs-mode: nil -*-
 *
 * user-chroot: A setuid program that allows non-root users to safely chroot(2)
 *
 * "safely": I believe that this program, when deployed as setuid on a
 * typical "distribution" such as RHEL or Debian, does not, even when
 * used in combination with typical software installed on that
 * distribution, allow privilege escalation.
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

typedef struct _BindMount BindMount;
struct _BindMount {
  const char *source;
  const char *dest;

  unsigned int readonly;

  BindMount *next;
};

static BindMount *
reverse_bind_mount_list (BindMount *mount)
{
  BindMount *prev = NULL;

  while (mount)
    {
      BindMount *next = mount->next;
      mount->next = prev;
      prev = mount;
      mount = next;
    }

  return prev;
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
  int after_mount_arg_index;
  unsigned int n_mounts = 0;
  const unsigned int max_mounts = 50; /* Totally arbitrary... */
  char **program_argv;
  BindMount *bind_mounts = NULL;
  BindMount *bind_mount_iter;

  if (argc <= 0)
    return 1;

  argv0 = argv[0];
  argc--;
  argv++;

  if (argc < 1)
    fatal ("ROOTDIR argument must be specified");

  after_mount_arg_index = 0;
  while (after_mount_arg_index < argc)
    {
      const char *arg = argv[after_mount_arg_index];
      BindMount *mount = NULL;

      if (n_mounts >= max_mounts)
        fatal ("Too many mounts (maximum of %u)", n_mounts);
      n_mounts++;

      if (strcmp (arg, "--mount-bind") == 0)
        {
          if ((argc - after_mount_arg_index) < 3)
            fatal ("--mount-bind takes two arguments");

          mount = malloc (sizeof (BindMount));
          mount->source = argv[after_mount_arg_index+1];
          mount->dest = argv[after_mount_arg_index+2];
          mount->readonly = 0;
          mount->next = bind_mounts;
          
          bind_mounts = mount;
          after_mount_arg_index += 3;
        }
      else if (strcmp (arg, "--mount-readonly") == 0)
        {
          BindMount *mount;

          if ((argc - after_mount_arg_index) < 2)
            fatal ("--mount-readonly takes one argument");

          mount = malloc (sizeof (BindMount));
          mount->source = NULL;
          mount->dest = argv[after_mount_arg_index+1];
          mount->readonly = 1;
          mount->next = bind_mounts;
          
          bind_mounts = mount;
          after_mount_arg_index += 2;
        }
      else
        break;
    }
        
  bind_mounts = reverse_bind_mount_list (bind_mounts);

  if ((argc - after_mount_arg_index) < 2)
    fatal ("usage: %s [--mount-readonly DIR] [--mount-bind SOURCE DEST] ROOTDIR PROGRAM ARGS...", argv0);
  chroot_dir = argv[after_mount_arg_index];
  program = argv[after_mount_arg_index+1];
  program_argv = argv + after_mount_arg_index + 1;

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
  if (mount ("/", "/", "none", MS_PRIVATE | MS_REC, NULL) < 0)
    fatal_errno ("mount(/, MS_PRIVATE | MS_REC)");

  /* Now let's set up our bind mounts */
  for (bind_mount_iter = bind_mounts; bind_mount_iter; bind_mount_iter = bind_mount_iter->next)
    {
      char *dest;

      asprintf (&dest, "%s%s", chroot_dir, bind_mount_iter->dest);

      if (bind_mount_iter->readonly)
        {
          if (mount (dest, dest,
                     NULL, MS_BIND | MS_PRIVATE, NULL) < 0)
            fatal_errno ("mount (MS_BIND)");
          if (mount (dest, dest,
                     NULL, MS_BIND | MS_PRIVATE | MS_REMOUNT | MS_RDONLY, NULL) < 0)
            fatal_errno ("mount (MS_BIND | MS_RDONLY)");
        }
      else
        {

          if (mount (bind_mount_iter->source, dest,
                     NULL, MS_BIND | MS_PRIVATE, NULL) < 0)
            fatal_errno ("mount (MS_BIND)");
        }
      free (dest);
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
