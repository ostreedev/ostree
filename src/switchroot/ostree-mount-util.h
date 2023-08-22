/*
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef __OSTREE_MOUNT_UTIL_H_
#define __OSTREE_MOUNT_UTIL_H_

#include <err.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define INITRAMFS_MOUNT_VAR "/run/ostree/initramfs-mount-var"
#define _OSTREE_SYSROOT_READONLY_STAMP "/run/ostree-sysroot-ro.stamp"
#define _OSTREE_COMPOSEFS_ROOT_STAMP "/run/ostree-composefs-root.stamp"

#define autofree __attribute__ ((cleanup (cleanup_free)))

static inline int
path_is_on_readonly_fs (const char *path)
{
  struct statvfs stvfsbuf;

  if (statvfs (path, &stvfsbuf) == -1)
    err (EXIT_FAILURE, "statvfs(%s)", path);

  return (stvfsbuf.f_flag & ST_RDONLY) != 0;
}

static inline char *
read_proc_cmdline (void)
{
  FILE *f = fopen ("/proc/cmdline", "r");
  char *cmdline = NULL;
  size_t len;

  if (!f)
    goto out;

  /* Note that /proc/cmdline will not end in a newline, so getline
   * will fail unelss we provide a length.
   */
  if (getline (&cmdline, &len, f) < 0)
    goto out;
  /* ... but the length will be the size of the malloc buffer, not
   * strlen().  Fix that.
   */
  len = strlen (cmdline);

  if (cmdline[len - 1] == '\n')
    cmdline[len - 1] = '\0';
out:
  if (f)
    fclose (f);
  return cmdline;
}

static inline void
cleanup_free (void *p)
{
  void **pp = (void **)p;
  free (*pp);
}

static inline char *
find_proc_cmdline_key (const char *cmdline, const char *key)
{
  const size_t key_len = strlen (key);
  for (const char *iter = cmdline; iter;)
    {
      const char *next = strchr (iter, ' ');
      if (strncmp (iter, key, key_len) == 0 && iter[key_len] == '=')
        {
          const char *start = iter + key_len + 1;
          if (next)
            return strndup (start, next - start);

          return strdup (start);
        }

      if (next)
        next += strspn (next, " ");

      iter = next;
    }

  return NULL;
}

/* This is an API for other projects to determine whether or not the
 * currently running system is ostree-controlled.
 */
static inline void
touch_run_ostree (void)
{
  int fd = open ("/run/ostree-booted", O_CREAT | O_WRONLY | O_NOCTTY | O_CLOEXEC, 0640);
  /* We ignore failures here in case /run isn't mounted...not much we
   * can do about that, but we don't want to fail.
   */
  if (fd == -1)
    return;
  (void)close (fd);
}

#endif /* __OSTREE_MOUNT_UTIL_H_ */
