/*
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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

static inline int
path_is_on_readonly_fs (char *path)
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

  if (cmdline[len-1] == '\n')
    cmdline[len-1] = '\0';
out:
  if (f)
    fclose (f);
  return cmdline;
}

static inline char *
read_proc_cmdline_ostree (void)
{
  char *cmdline = NULL;
  const char *iter;
  char *ret = NULL;

  cmdline = read_proc_cmdline ();
  if (!cmdline)
    err (EXIT_FAILURE, "failed to read /proc/cmdline");

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

#endif /* __OSTREE_MOUNT_UTIL_H_ */
