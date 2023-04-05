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

#include <dirent.h>
#include <err.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#define INITRAMFS_MOUNT_VAR "/run/ostree/initramfs-mount-var"
#define _OSTREE_SYSROOT_READONLY_STAMP "/run/ostree-sysroot-ro.stamp"
#define ABOOT_KARG "aboot"

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

static inline void
free_char (char **to_free)
{
  free (*to_free);
}

static inline void
close_dir (DIR **dir)
{
  if (*dir)
    closedir (*dir);
}

static inline void
close_file (FILE **f)
{
  if (*f)
    fclose (*f);
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

static inline void
cpy_and_null (char **dest, char **src)
{
  *dest = *src;
  *src = NULL;
}

/* If the key matches the start of the line, copy line to out
 */
static inline bool
cpy_if_key_match (char **line, const char *key, char **out)
{
  /* There should only be one of each key per BLS file, so check for NULL, we
   * should only parse the first occurance of a key, if there's two, it's a
   * malformed BLS file
   */
  if (!*out && strstr (*line, key) == *line)
    {
      cpy_and_null (out, line);
      return true;
    }

  return false;
}

static inline bool
has_suffix (const char *str, const char *suffix)
{
  if (!str || !suffix)
    return false;

  const size_t str_len = strlen (str);
  const size_t suffix_len = strlen (suffix);
  if (str_len < suffix_len)
    return false;

  return !strcmp (str + str_len - suffix_len, suffix);
}

/* On completion version and options will be set to new values if the version
 * is more recent. Will loop line through line on the passed in open FILE.
 */
static inline void
copy_if_higher_version (FILE *f, char **version, char **options)
{
  char __attribute__ ((cleanup (free_char))) *line = NULL;
  char __attribute__ ((cleanup (free_char))) *version_local = NULL;
  char __attribute__ ((cleanup (free_char))) *options_local = NULL;
  char __attribute__ ((cleanup (free_char))) *linux_local = NULL;
  /* Note getline() will reuse the previous buffer when not zero */
  for (size_t len = 0; getline (&line, &len, f) != -1;)
    {
      /* This is an awful hack to avoid depending on GLib in the
       * initramfs right now.
       */
      if (cpy_if_key_match (&line, "version ", &version_local))
          continue;

      if (cpy_if_key_match (&line, "options ", &options_local))
          continue;

      if (cpy_if_key_match (&line, "linux ", &linux_local))
          continue;
    }

  /* The case where we have no version set yet */
  if (!*version
      || strverscmp (version_local + sizeof ("version"), (*version) + sizeof ("version")) > 0)
    {
      struct utsname buf;
      uname (&buf);
      strtok (linux_local + sizeof ("linux"), " \t\r\n");
      if (!has_suffix (linux_local, buf.release))
        return;

      cpy_and_null (version, &version_local);
      cpy_and_null (options, &options_local);
    }

  return;
}

static inline char *
parse_ostree_from_options (const char *options)
{
  if (options)
    {
      options += sizeof ("options");
      char *start_of_ostree = strstr (options, "ostree=");
      if (start_of_ostree > options)
        {
          start_of_ostree += sizeof ("ostree");
          /* trim everything to the right */
          strtok (start_of_ostree, " \t\r\n");
          return strdup (start_of_ostree);
        }
    }

  return NULL;
}

/* This function is for boot arrangements where it is not possible to use a
 * karg/cmdline, this is the case when the cmdline is part of the signed
 * boot image, alternatively this function takes the karg from the bls entry
 * which will have the correct ostree= karg set. This bls entry is not parsed
 * from the bootloader but from the initramfs instead.
 */
static inline char *
bls_parser_get_ostree_option (const char *sysroot)
{
  char out[PATH_MAX] = "";
  int written = snprintf (out, PATH_MAX, "%s/boot/loader/entries", sysroot);
  DIR __attribute__ ((cleanup (close_dir))) *dir = opendir (out);
  if (!dir)
    {
      fprintf (stderr, "opendir(\"%s\") failed with %d\n", out, errno);
      return NULL;
    }

  char __attribute__ ((cleanup (free_char))) *version = NULL;
  char __attribute__ ((cleanup (free_char))) *options = NULL;
  for (struct dirent *ent = 0; (ent = readdir (dir));)
    {
      if (ent->d_name[0] == '.')
        continue;

      if (!has_suffix (ent->d_name, ".conf"))
        continue;

      snprintf (out + written, PATH_MAX - written, "/%s", ent->d_name);

      FILE __attribute__ ((cleanup (close_file))) *f = fopen (out, "r");
      if (!f)
        {
          fprintf (stderr, "fopen(\"%s\", \"r\") failed with %d\n", out, errno);
          continue;
        }

      copy_if_higher_version (f, &version, &options);
    }

  return parse_ostree_from_options (options);
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
