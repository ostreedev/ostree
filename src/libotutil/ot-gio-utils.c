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

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

#include <string.h>

#include "otutil.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

GFileType
ot_gfile_type_for_mode (guint32 mode)
{
  if (S_ISDIR (mode))
    return G_FILE_TYPE_DIRECTORY;
  else if (S_ISREG (mode))
    return G_FILE_TYPE_REGULAR;
  else if (S_ISLNK (mode))
    return G_FILE_TYPE_SYMBOLIC_LINK;
  else if (S_ISBLK (mode) || S_ISCHR(mode) || S_ISFIFO(mode))
    return G_FILE_TYPE_SPECIAL;
  else
    return G_FILE_TYPE_UNKNOWN;
}


GFile *
ot_gfile_from_build_path (const char *first, ...)
{
  va_list args;
  const char *arg;
  ot_lfree char *path = NULL;
  ot_lptrarray GPtrArray *components = NULL;  

  va_start (args, first);

  components = g_ptr_array_new ();
  
  arg = first;
  while (arg != NULL)
    {
      g_ptr_array_add (components, (char*)arg);
      arg = va_arg (args, const char *);
    }

  va_end (args);

  g_ptr_array_add (components, NULL);

  path = g_build_filenamev ((char**)components->pdata);

  return g_file_new_for_path (path);
}

GFile *
ot_gfile_get_child_strconcat (GFile *parent,
                              const char *first,
                              ...) 
{
  va_list args;
  GFile *ret;
  GString *buf;
  const char *arg;

  g_return_val_if_fail (first != NULL, NULL);

  va_start (args, first);
  
  buf = g_string_new (first);
  
  while ((arg = va_arg (args, const char *)) != NULL)
    g_string_append (buf, arg);

  ret = g_file_get_child (parent, buf->str);
  
  g_string_free (buf, TRUE);

  return ret;
}

GFile *
ot_gfile_get_child_build_path (GFile      *parent,
                               const char *first, ...)
{
  va_list args;
  const char *arg;
  ot_lfree char *path = NULL;
  ot_lptrarray GPtrArray *components = NULL;  

  va_start (args, first);

  components = g_ptr_array_new ();
  
  arg = first;
  while (arg != NULL)
    {
      g_ptr_array_add (components, (char*)arg);
      arg = va_arg (args, const char *);
    }

  va_end (args);

  g_ptr_array_add (components, NULL);

  path = g_build_filenamev ((char**)components->pdata);

  return g_file_resolve_relative_path (parent, path);
}

GFile *
ot_gfile_resolve_path_printf (GFile       *path,
                              const char  *format,
                              ...)
{
  va_list args;
  gs_free char *relpath = NULL;

  va_start (args, format);
  relpath = g_strdup_vprintf (format, args);
  va_end (args);

  return g_file_resolve_relative_path (path, relpath);
}


gboolean
ot_gfile_get_symlink_target_from_info (GFile             *path,
                                       GFileInfo         *file_info,
                                       GFile            **out_target,
                                       GCancellable      *cancellable,
                                       GError           **error)
{
  gboolean ret = FALSE;
  const char *target;
  gs_unref_object GFile *path_parent = NULL;
  gs_unref_object GFile *ret_target = NULL;

  if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_SYMBOLIC_LINK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Not a symbolic link");
      goto out;
    }

  path_parent = g_file_get_parent (path);
  target = g_file_info_get_symlink_target (file_info);
  g_assert (target);
  ret_target = g_file_resolve_relative_path (path_parent, target);

  ret = TRUE;
 out:
  ot_transfer_out_value (out_target, &ret_target);
  return ret;
}

gboolean
ot_gfile_query_info_allow_noent (GFile                *path,
                                 const char           *queryopts,
                                 GFileQueryInfoFlags   flags,
                                 GFileInfo           **out_info,
                                 GCancellable         *cancellable,
                                 GError              **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileInfo *ret_file_info = NULL;
  GError *temp_error = NULL;

  ret_file_info = g_file_query_info (path, queryopts, flags,
                                     cancellable, &temp_error);
  if (!ret_file_info)
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_info, &ret_file_info);
 out:
  return ret;
}

gboolean
ot_gfile_query_symlink_target_allow_noent (GFile          *path,
                                           GFile         **out_target,
                                           GCancellable   *cancellable,
                                           GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_object GFile *ret_target = NULL;

  if (!ot_gfile_query_info_allow_noent (path, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        &file_info,
                                        cancellable, error))
    goto out;

  if (file_info != NULL)
    {
      if (!ot_gfile_get_symlink_target_from_info (path, file_info, &ret_target,
                                                  cancellable, error))
        goto out;
    }
  
  ret = TRUE;
  ot_transfer_out_value (out_target, &ret_target);
 out:
  return ret;
}

gboolean
ot_gfile_load_contents_utf8_allow_noent (GFile          *path,
                                         char          **out_contents,
                                         GCancellable   *cancellable,
                                         GError        **error)
{
  gboolean ret = TRUE;
  GError *temp_error = NULL;
  gs_free char *ret_contents = NULL;

  ret_contents = gs_file_load_contents_utf8 (path, cancellable, &temp_error);
  if (!ret_contents)
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_contents, &ret_contents);
 out:
  return ret;
}

/**
 * ot_gfile_ensure_unlinked:
 *
 * Like gs_file_unlink(), but return successfully if the file doesn't
 * exist.
 */
gboolean
ot_gfile_ensure_unlinked (GFile         *path,
                          GCancellable  *cancellable,
                          GError       **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;

  if (!gs_file_unlink (path, cancellable, &temp_error))
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }
  
  ret = TRUE;
 out:
  return ret;
}

/**
 * ot_gfile_atomic_symlink_swap:
 * @path: Replace the contents of this symbolic link
 * @target: New symbolic link target
 * @cancellable:
 * @error
 *
 * Create a new temporary symbolic link, then use the Unix rename()
 * function to atomically replace @path with the new symbolic link.
 * Do not use this function inside directories such as /tmp as it uses
 * a predicatable file name.
 */
gboolean
ot_gfile_atomic_symlink_swap (GFile          *path,
                              const char     *target,
                              GCancellable   *cancellable,
                              GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *parent = g_file_get_parent (path);
  gs_free char *tmpname = g_strconcat (gs_file_get_basename_cached (path), ".tmp", NULL);
  gs_unref_object GFile *tmppath = g_file_get_child (parent, tmpname);

  if (!ot_gfile_ensure_unlinked (tmppath, cancellable, error))
    goto out;
  
  if (!g_file_make_symbolic_link (tmppath, target, cancellable, error))
    goto out;

  if (!gs_file_rename (tmppath, path, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
