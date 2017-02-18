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
#include <gio/gfiledescriptorbased.h>

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
  g_autofree char *path = NULL;
  g_autoptr(GPtrArray) components = NULL;  

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

  va_end (args);

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
  g_autofree char *path = NULL;
  g_autoptr(GPtrArray) components = NULL;  

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
  g_autofree char *relpath = NULL;

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
  g_autoptr(GFile) path_parent = NULL;
  g_autoptr(GFile) ret_target = NULL;

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
  g_autoptr(GFileInfo) ret_file_info = NULL;
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
  g_autoptr(GFileInfo) file_info = NULL;
  g_autoptr(GFile) ret_target = NULL;

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
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  g_autofree char *ret_contents = NULL;

  ret_contents = glnx_file_get_contents_utf8_at (AT_FDCWD, gs_file_get_path_cached (path), NULL,
                                                 cancellable, &temp_error);
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
 * ot_gfile_replace_contents_fsync:
 * 
 * Like g_file_replace_contents(), except always uses fdatasync().
 */
gboolean
ot_gfile_replace_contents_fsync (GFile          *path,
                                 GBytes         *contents,
                                 GCancellable   *cancellable,
                                 GError        **error)
{
  gsize len;
  const guint8*buf = g_bytes_get_data (contents, &len);

  return glnx_file_replace_contents_at (AT_FDCWD, gs_file_get_path_cached (path),
                                        buf, len,
                                        GLNX_FILE_REPLACE_DATASYNC_NEW,
                                        cancellable, error);
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
  if (unlink (gs_file_get_path_cached (path)) != 0)
    {
      if (errno != ENOENT)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }
  return TRUE;
}

#if !GLIB_CHECK_VERSION(2, 44, 0)

gboolean
ot_file_enumerator_iterate (GFileEnumerator  *direnum,
                            GFileInfo       **out_info,
                            GFile           **out_child,
                            GCancellable     *cancellable,
                            GError          **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;

  static GQuark cached_info_quark;
  static GQuark cached_child_quark;
  static gsize quarks_initialized;

  g_return_val_if_fail (direnum != NULL, FALSE);
  g_return_val_if_fail (out_info != NULL, FALSE);

  if (g_once_init_enter (&quarks_initialized))
    {
      cached_info_quark = g_quark_from_static_string ("ot-cached-info");
      cached_child_quark = g_quark_from_static_string ("ot-cached-child");
      g_once_init_leave (&quarks_initialized, 1);
    }

  *out_info = g_file_enumerator_next_file (direnum, cancellable, &temp_error);
  if (out_child)
    *out_child = NULL;
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  else if (*out_info != NULL)
    {
      g_object_set_qdata_full ((GObject*)direnum, cached_info_quark, *out_info, (GDestroyNotify)g_object_unref);
      if (out_child != NULL)
        {
          const char *name = g_file_info_get_name (*out_info);
          *out_child = g_file_get_child (g_file_enumerator_get_container (direnum), name);
          g_object_set_qdata_full ((GObject*)direnum, cached_child_quark, *out_child, (GDestroyNotify)g_object_unref);
        }
    }

  ret = TRUE;
 out:
  return ret;
}

#endif

G_LOCK_DEFINE_STATIC (pathname_cache);

/**
 * ot_file_get_path_cached:
 *
 * Like g_file_get_path(), but returns a constant copy so callers
 * don't need to free the result.
 */
const char *
ot_file_get_path_cached (GFile *file)
{
  const char *path;
  static GQuark _file_path_quark = 0;

  if (G_UNLIKELY (_file_path_quark) == 0)
    _file_path_quark = g_quark_from_static_string ("gsystem-file-path");

  G_LOCK (pathname_cache);

  path = g_object_get_qdata ((GObject*)file, _file_path_quark);
  if (!path)
    {
      path = g_file_get_path (file);
      if (path == NULL)
        {
          G_UNLOCK (pathname_cache);
          return NULL;
        }
      g_object_set_qdata_full ((GObject*)file, _file_path_quark, (char*)path, (GDestroyNotify)g_free);
    }

  G_UNLOCK (pathname_cache);

  return path;
}
