/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ht-unix-utils.h"

#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixoutputstream.h>

#include <string.h>
#include <sys/types.h>
#include <dirent.h>

gboolean
ht_util_spawn_pager (GOutputStream  **out_stream,
                     GError         **error)
{
  const char *pager;
  char *argv[2];
  int stdin_fd;
  pid_t pid;
  gboolean ret = FALSE;
  GOutputStream *ret_stream = NULL;

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
      
      if (!g_spawn_async_with_pipes (NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                     NULL, NULL, &pid, &stdin_fd, NULL, NULL, error))
        {
          g_prefix_error (error, "%s", "Failed to spawn pager: ");
          goto out;
        }
      
      ret_stream = (GOutputStream*)g_unix_output_stream_new (stdin_fd, TRUE);
    }

  *out_stream = ret_stream;
  ret_stream = NULL;
  ret = TRUE;
 out:
  g_clear_object (&ret_stream);
  return ret;
}

static int
compare_filenames_by_component_length (const char *a,
                                       const char *b)
{
  char *a_slash, *b_slash;

  a_slash = strchr (a, '/');
  b_slash = strchr (b, '/');
  while (a_slash && b_slash)
    {
      a = a_slash + 1;
      b = b_slash + 1;
      a_slash = strchr (a, '/');
      b_slash = strchr (b, '/');
    }
  if (a_slash)
    return -1;
  else if (b_slash)
    return 1;
  else
    return 0;
}

GPtrArray *
ht_util_sort_filenames_by_component_length (GPtrArray *files)
{
  GPtrArray *array = g_ptr_array_sized_new (files->len);
  memcpy (array->pdata, files->pdata, sizeof (gpointer) * files->len);
  g_ptr_array_sort (array, (GCompareFunc) compare_filenames_by_component_length);
  return array;
}

int
ht_util_count_filename_components (const char *path)
{
  int i = 0;

  while (path)
    {
      i++;
      path = strchr (path, '/');
      if (path)
        path++;
    }
  return i;
}

gboolean
ht_util_filename_has_dotdot (const char *path)
{
  char *p;
  char last;

  if (strcmp (path, "..") == 0)
    return TRUE;
  if (g_str_has_prefix (path, "../"))
    return TRUE;
  p = strstr (path, "/..");
  if (!p)
    return FALSE;
  last = *(p + 1);
  return last == '\0' || last == '/';
}

GPtrArray *
ht_util_path_split (const char *path)
{
  GPtrArray *ret = NULL;
  const char *p;
  const char *slash;
  int i;

  g_return_val_if_fail (path[0] != '/', NULL);

  ret = g_ptr_array_new ();
  g_ptr_array_set_free_func (ret, g_free);

  p = path;
  do {
    slash = strchr (p, '/');
    if (!slash)
      {
        g_ptr_array_add (ret, g_strdup (p));
        p = NULL;
      }
    else
      {
        g_ptr_array_add (ret, g_strndup (p, slash - p));
        p = slash + 1;
      }
  } while (p && *p);

  /* Canonicalize by removing duplicate '.' */
  for (i = ret->len-1; i >= 0; i--)
    {
      if (strcmp (ret->pdata[i], ".") == 0)
        g_ptr_array_remove_index (ret, i);
    }

  return ret;
}

char *
ht_util_path_join_n (const char *base, GPtrArray *components, int n)
{
  int max = MIN(n+1, components->len);
  GPtrArray *subcomponents;
  char *path;
  int i;

  subcomponents = g_ptr_array_new ();

  if (base != NULL)
    g_ptr_array_add (subcomponents, (char*)base);

  for (i = 0; i < max; i++)
    {
      g_ptr_array_add (subcomponents, components->pdata[i]);
    }
  g_ptr_array_add (subcomponents, NULL);
  
  path = g_build_filenamev ((char**)subcomponents->pdata);
  g_ptr_array_free (subcomponents, TRUE);
  
  return path;
}

void
ht_util_set_error_from_errno (GError **error,
                              gint     saved_errno)
{
  g_set_error_literal (error,
                       G_UNIX_ERROR,
                       0,
                       g_strerror (saved_errno));
  errno = saved_errno;
}

int
ht_util_open_file_read (const char *path, GError **error)
{
  char *dirname = NULL;
  char *basename = NULL;
  DIR *dir = NULL;
  int fd = -1;

  dirname = g_path_get_dirname (path);
  basename = g_path_get_basename (path);
  dir = opendir (dirname);
  if (dir == NULL)
    {
      ht_util_set_error_from_errno (error, errno);
      goto out;
    }

  fd = ht_util_open_file_read_at (dirfd (dir), basename, error);

 out:
  g_free (basename);
  g_free (dirname);
  if (dir != NULL)
    closedir (dir);
  return fd;
}

int
ht_util_open_file_read_at (int dirfd, const char *name, GError **error)
{
  int fd;
  int flags = O_RDONLY;
  
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOATIME
  flags |= O_NOATIME;
#endif
  fd = openat (dirfd, name, flags);
  if (fd < 0)
    ht_util_set_error_from_errno (error, errno);
  return fd;
}
