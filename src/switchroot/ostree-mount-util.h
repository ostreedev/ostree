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

#ifdef HAVE_LINUX_FSVERITY_H
#include <linux/fsverity.h>
#endif

#define INITRAMFS_MOUNT_VAR "/run/ostree/initramfs-mount-var"
#define _OSTREE_SYSROOT_READONLY_STAMP "/run/ostree-sysroot-ro.stamp"
#define _OSTREE_COMPOSEFS_ROOT_STAMP "/run/ostree-composefs-root.stamp"

#define autofree __attribute__ ((cleanup (cleanup_free)))
#define steal_pointer(pp) steal_pointer_impl ((void **)pp)

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

static inline void *
steal_pointer_impl (void **to_steal)
{
  void *ret = *to_steal;
  *to_steal = NULL;

  return ret;
}

static inline char *
read_proc_cmdline_key (const char *key)
{
  char *cmdline = NULL;
  const char *iter;
  char *ret = NULL;
  size_t key_len = strlen (key);

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
      if (strncmp (iter, key, key_len) == 0 && iter[key_len] == '=')
        {
          const char *start = iter + key_len + 1;
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

static inline char *
get_aboot_root_slot (void)
{
  autofree char *slot_suffix = read_proc_cmdline_key ("androidboot.slot_suffix");
  if (strcmp (slot_suffix, "_a") == 0)
    return strdup ("/ostree/root.a");
  else if (strcmp (slot_suffix, "_b") == 0)
    return strdup ("/ostree/root.b");

  errx (EXIT_FAILURE, "androidboot.slot_suffix invalid: %s", slot_suffix);

  return NULL;
}

static inline char *
get_ostree_target (void)
{
  autofree char *ostree_cmdline = read_proc_cmdline_key ("ostree");

  if (!ostree_cmdline)
    return NULL;

  if (strcmp (ostree_cmdline, "aboot") == 0)
    return get_aboot_root_slot ();

  return steal_pointer (&ostree_cmdline);
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

static inline unsigned char *
read_file (const char *path, size_t *out_len)
{
  int fd;

  fd = open (path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      if (errno == ENOENT)
        return NULL;
      err (EXIT_FAILURE, "failed to open %s", path);
    }

  struct stat stbuf;
  if (fstat (fd, &stbuf))
    err (EXIT_FAILURE, "fstat(%s) failed", path);

  size_t file_size = stbuf.st_size;
  unsigned char *buf = malloc (file_size);
  if (buf == NULL)
    err (EXIT_FAILURE, "Out of memory");

  size_t file_read = 0;
  while (file_read < file_size)
    {
      ssize_t bytes_read;
      do
        bytes_read = read (fd, buf + file_read, file_size - file_read);
      while (bytes_read == -1 && errno == EINTR);
      if (bytes_read == -1)
        err (EXIT_FAILURE, "read_file(%s) failed", path);
      if (bytes_read == 0)
        break;

      file_read += bytes_read;
    }

  close (fd);

  *out_len = file_read;
  return buf;
}

static inline void
fsverity_sign (int fd, unsigned char *signature, size_t signature_len)
{
#ifdef HAVE_LINUX_FSVERITY_H
  struct fsverity_enable_arg arg = {
    0,
  };
  arg.version = 1;
  arg.hash_algorithm = FS_VERITY_HASH_ALG_SHA256;
  arg.block_size = 4096;
  arg.sig_size = signature_len;
  arg.sig_ptr = (uint64_t)signature;

  if (ioctl (fd, FS_IOC_ENABLE_VERITY, &arg) < 0)
    err (EXIT_FAILURE, "failed to fs-verity sign file");
#endif
}

static inline void
bin2hex (char *out_buf, const unsigned char *inbuf, size_t len)
{
  static const char hexchars[] = "0123456789abcdef";
  unsigned int i, j;

  for (i = 0, j = 0; i < len; i++, j += 2)
    {
      unsigned char byte = inbuf[i];
      out_buf[j] = hexchars[byte >> 4];
      out_buf[j + 1] = hexchars[byte & 0xF];
    }
  out_buf[j] = '\0';
}

#endif /* __OSTREE_MOUNT_UTIL_H_ */
