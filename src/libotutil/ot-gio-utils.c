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

static gboolean
cp_internal (GFile         *src,
             GFile         *dest,
             gboolean       use_hardlinks,
             GCancellable  *cancellable,
             GError       **error)
{
  gboolean ret = FALSE;
  ot_lobj GFileEnumerator *enumerator = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  GError *temp_error = NULL;

  enumerator = g_file_enumerate_children (src, OSTREE_GIO_FAST_QUERYINFO,
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, error);
  if (!enumerator)
    goto out;

  if (!gs_file_ensure_directory (dest, FALSE, cancellable, error))
    goto out;

  while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
    {
      const char *name = g_file_info_get_name (file_info);
      ot_lobj GFile *src_child = g_file_get_child (src, name);
      ot_lobj GFile *dest_child = g_file_get_child (dest, name);

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!gs_file_ensure_directory (dest_child, FALSE, cancellable, error))
            goto out;

          /* Can't do this even though we'd like to; it fails with an error about
           * setting standard::type not being supported =/
           *
           if (!g_file_set_attributes_from_info (dest_child, file_info, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
           cancellable, error))
           goto out;
          */
          if (chmod (gs_file_get_path_cached (dest_child),
                     g_file_info_get_attribute_uint32 (file_info, "unix::mode")) == -1)
            {
              ot_util_set_error_from_errno (error, errno);
              goto out;
            }

          if (!cp_internal (src_child, dest_child, use_hardlinks, cancellable, error))
            goto out;
        }
      else
        {
          gboolean did_link = FALSE;
          (void) unlink (gs_file_get_path_cached (dest_child));
          if (use_hardlinks)
            {
              if (link (gs_file_get_path_cached (src_child), gs_file_get_path_cached (dest_child)) == -1)
                {
                  if (!(errno == EMLINK || errno == EXDEV))
                    {
                      ot_util_set_error_from_errno (error, errno);
                      goto out;
                    }
                  use_hardlinks = FALSE;
                }
              else
                did_link = TRUE;
            }
          if (!did_link)
            {
              if (!g_file_copy (src_child, dest_child,
                                G_FILE_COPY_OVERWRITE | G_FILE_COPY_ALL_METADATA | G_FILE_COPY_NOFOLLOW_SYMLINKS,
                                cancellable, NULL, NULL, error))
                goto out;
            }
        }
      g_clear_object (&file_info);
    }
  if (temp_error)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * ot_gio_shutil_cp_al_or_fallback:
 * @src: Source path
 * @dest: Destination path
 * @cancellable:
 * @error:
 *
 * Recursively copy path @src (which must be a directory) to the
 * target @dest.  If possible, hardlinks are used; if a hardlink is
 * not possible, a regular copy is created.  Any existing files are
 * overwritten.
 *
 * Returns: %TRUE on success
 */
gboolean
ot_gio_shutil_cp_al_or_fallback (GFile         *src,
                                 GFile         *dest,
                                 GCancellable  *cancellable,
                                 GError       **error)
{
  return cp_internal (src, dest, TRUE, cancellable, error);
}

/**
 * ot_gio_shutil_cp_a:
 * @src: Source path
 * @dest: Destination path
 * @cancellable:
 * @error:
 *
 * Recursively copy path @src (which must be a directory) to the
 * target @dest.  Any existing files are overwritten.
 *
 * Returns: %TRUE on success
 */
gboolean
ot_gio_shutil_cp_a (GFile         *src,
                    GFile         *dest,
                    GCancellable  *cancellable,
                    GError       **error)
{
  return cp_internal (src, dest, FALSE, cancellable, error);
}

gboolean
ot_gio_shutil_rm_rf (GFile        *path,
                     GCancellable *cancellable,
                     GError      **error)
{
  gboolean ret = FALSE;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  GError *temp_error = NULL;

  dir_enum = g_file_enumerate_children (path, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, &temp_error);
  if (!dir_enum)
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
          ret = TRUE;
        }
      else
        g_propagate_error (error, temp_error);

      goto out;
    }

  while ((file_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      ot_lobj GFile *subpath = NULL;
      GFileType type;
      const char *name;

      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");
      name = g_file_info_get_attribute_byte_string (file_info, "standard::name");
      
      subpath = g_file_get_child (path, name);

      if (type == G_FILE_TYPE_DIRECTORY)
        {
          if (!ot_gio_shutil_rm_rf (subpath, cancellable, error))
            goto out;
        }
      else
        {
          if (!gs_file_unlink (subpath, cancellable, error))
            goto out;
        }
      g_clear_object (&file_info);
    }
  if (temp_error)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  if (!g_file_delete (path, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
