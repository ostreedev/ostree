/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#define _GNU_SOURCE

#include "otutil.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gio/gunixoutputstream.h>

#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>

gboolean
ot_util_spawn_pager (GOutputStream  **out_stream,
                     GError         **error)
{
  gboolean ret = FALSE;
  const char *pager;
  char *argv[2];
  int stdin_fd;
  pid_t pid;
  ot_lfree GOutputStream *ret_stream = NULL;

  if (!isatty (1))
    {
      ret_stream = (GOutputStream*)g_unix_output_stream_new (1, TRUE);
    }
  else
    {
      pager = g_getenv ("GIT_PAGER");
      if (pager == NULL)
        pager = "less";
      
      argv[0] = (char*)pager;
      argv[1] = NULL;
      
      if (!g_spawn_async_with_pipes (NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                     NULL, NULL, &pid, &stdin_fd, NULL, NULL, error))
        {
          g_prefix_error (error, "%s", "Failed to spawn pager: ");
          goto out;
        }
      
      ret_stream = (GOutputStream*)g_unix_output_stream_new (stdin_fd, TRUE);
    }

  ot_transfer_out_value(out_stream, &ret_stream);
  ret = TRUE;
 out:
  return ret;
}

gboolean
ot_util_filename_validate (const char *name,
                           GError    **error)
{
  if (strcmp (name, ".") == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid self-referential filename '.'");
      return FALSE;
    }
  if (strcmp (name, "..") == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid path uplink filename '..'");
      return FALSE;
    }
  if (strchr (name, '/') != NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid / in filename %s", name);
      return FALSE;
    }
  return TRUE;
}

static GPtrArray *
ot_split_string_ptrarray (const char *str,
                          char        c)
{
  GPtrArray *ret = g_ptr_array_new_with_free_func (g_free);
  const char *p;

  do {
    p = strchr (str, '/');
    if (!p)
      {
        g_ptr_array_add (ret, g_strdup (str));
        str = NULL;
      }
    else
      {
        g_ptr_array_add (ret, g_strndup (str, p - str));
        str = p + 1;
      }
  } while (str && *str);

  return ret;
}

gboolean
ot_util_path_split_validate (const char *path,
                             GPtrArray **out_components,
                             GError    **error)
{
  gboolean ret = FALSE;
  int i;
  ot_lptrarray GPtrArray *ret_components = NULL;

  if (strlen (path) > PATH_MAX)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Path '%s' is too long", path);
      goto out;
    }

  ret_components = ot_split_string_ptrarray (path, '/');

  /* Canonicalize by removing '.' and '', throw an error on .. */
  for (i = ret_components->len-1; i >= 0; i--)
    {
      const char *name = ret_components->pdata[i];
      if (strcmp (name, "..") == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid uplink '..' in path %s", path);
          goto out;
        }
      if (strcmp (name, ".") == 0 || name[0] == '\0')
        g_ptr_array_remove_index (ret_components, i);
    }

  ret = TRUE;
  ot_transfer_out_value(out_components, &ret_components);
 out:
  return ret;
}

void
ot_util_set_error_from_errno (GError **error,
                              gint     saved_errno)
{
  g_set_error_literal (error,
                       G_IO_ERROR,
                       g_io_error_from_errno (saved_errno),
                       g_strerror (saved_errno));
  errno = saved_errno;
}

void
ot_util_fatal_literal (const char *msg)
{
  g_printerr ("%s\n", msg);
  exit (1);
}

void
ot_util_fatal_gerror (GError *error)
{
  g_assert (error != NULL);
  ot_util_fatal_literal (error->message);
}

/**
 * ot_unix_close:
 *
 * Like close(), but uses #GError, and handles EINTR.
 */
gboolean
ot_unix_close (int fd, GError **error)
{
  int result;
  do
    result = close (fd);
  while (G_UNLIKELY (result != 0 && errno == EINTR));
  if (result != 0)
    {
      ot_util_set_error_from_errno (error, errno);
      return FALSE;
    }
  return TRUE;
}

/**
 * ot_unix_open_noatime:
 *
 * Open a file for reading, using O_NOATIME if possible.
 */
gboolean
ot_unix_open_noatime (const char    *path,
                      int           *out_fd,
                      GError       **error)
{
  int fd;

#ifdef O_NOATIME
  fd = g_open (path, O_RDONLY | O_NOATIME | O_CLOEXEC, 0);
  if (fd == -1 && errno == EPERM)
#endif
    fd = g_open (path, O_RDONLY | O_CLOEXEC, 0);
  if (fd == -1)
    {
      ot_util_set_error_from_errno (error, errno);
      return FALSE;
    }
  *out_fd = fd;
  return TRUE;
}
