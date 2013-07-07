/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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

#define _GNU_SOURCE
#include "config.h"

#include "ot-admin-functions.h"
#include "otutil.h"
#include "ostree-core.h"
#include "libgsystem.h"

/*
 * Modify @arg which should be of the form key=value to make @arg just
 * contain key.  Return a pointer to the start of value.
 */
char *
ot_admin_util_split_keyeq (char *arg)
{
  char *eq;
      
  eq = strchr (arg, '=');
  if (eq)
    {
      /* Note key/val are in one malloc block,
       * so we don't free val...
       */
      *eq = '\0';
      return eq+1;
    }
  else
    {
      /* ...and this allows us to insert a constant
       * string.
       */
      return "";
    }
}

gboolean
ot_admin_util_get_devino (GFile         *path,
                          guint32       *out_device,
                          guint64       *out_inode,
                          GCancellable  *cancellable,
                          GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileInfo *finfo = g_file_query_info (path, "unix::device,unix::inode",
                                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                        cancellable, error);

  if (!finfo)
    goto out;

  ret = TRUE;
  *out_device = g_file_info_get_attribute_uint32 (finfo, "unix::device");
  *out_inode = g_file_info_get_attribute_uint64 (finfo, "unix::inode");
 out:
  return ret;
}
