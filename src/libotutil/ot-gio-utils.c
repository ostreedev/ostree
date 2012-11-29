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
