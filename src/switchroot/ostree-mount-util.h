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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#ifndef __OSTREE_MOUNT_UTIL_H_
#define __OSTREE_MOUNT_UTIL_H_

#include <err.h>
#include <stdlib.h>
#include <sys/statvfs.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>

#define INITRAMFS_MOUNT_VAR "/run/ostree/initramfs-mount-var"
#define _OSTREE_SYSROOT_READONLY_STAMP "/run/ostree-sysroot-ro.stamp"

// This limit depends on the architecture and is between 256 and 4096 characters.
// It is defined in the file ./include/asm/setup.h as COMMAND_LINE_SIZE.
#define COMMAND_LINE_SIZE 4096

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
  FILE *f = fopen("/proc/cmdline", "r");
  if (!f)
    return NULL;

  char buf[COMMAND_LINE_SIZE] = {'\0'};
  size_t len = fread(&buf, 1, sizeof (buf), f);
  fclose(f);
  return strndup(buf, len);
}

static inline char *
read_proc_cmdline_ostree (void)
{
  char *cmdline = read_proc_cmdline ();
  if (!cmdline)
    err (EXIT_FAILURE, "failed to read /proc/cmdline");

  char *ret = NULL;
  char *sp = NULL;
  const char* next = strtok_r (cmdline, " \n", &sp);
  while (next)
    {
      if (strncmp (next, "ostree=", strlen ("ostree=")) == 0) {
        const char *start = next + strlen ("ostree=");
        ret = strdup (start);
        break;
      }
      next = strtok_r (NULL, " \n", &sp);
    }

  free (cmdline);
  return ret;
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
  (void) close (fd);
}

#endif /* __OSTREE_MOUNT_UTIL_H_ */
