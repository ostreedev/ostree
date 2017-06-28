/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>.
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

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* Basically the stuff that comes from stat() and cheap calls like
 * readlink().  Other things require opening the file, or also
 * stat()ing the parent directory.
 */
#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")

GFile * ot_gfile_resolve_path_printf (GFile       *path,
                                      const char  *format,
                                      ...) G_GNUC_PRINTF(2, 3);

gboolean ot_gfile_replace_contents_fsync (GFile          *path,
                                          GBytes         *contents,
                                          GCancellable   *cancellable,
                                          GError        **error);

gboolean ot_gfile_ensure_unlinked (GFile         *path,
                                   GCancellable  *cancellable,
                                   GError       **error);

#if !GLIB_CHECK_VERSION(2, 44, 0)
gboolean
ot_file_enumerator_iterate (GFileEnumerator  *direnum,
                            GFileInfo       **out_info,
                            GFile           **out_child,
                            GCancellable     *cancellable,
                            GError          **error);
#else
static inline gboolean
ot_file_enumerator_iterate (GFileEnumerator  *direnum,
                            GFileInfo       **out_info,
                            GFile           **out_child,
                            GCancellable     *cancellable,
                            GError          **error)
{
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS;
  return (g_file_enumerator_iterate) (direnum, out_info, out_child, cancellable, error);
  G_GNUC_END_IGNORE_DEPRECATIONS;
}
#endif
#define g_file_enumerator_iterate ot_file_enumerator_iterate

const char *
ot_file_get_path_cached (GFile *file);

static inline
const char *
gs_file_get_path_cached (GFile *file)
{
  return ot_file_get_path_cached (file);
}

G_END_DECLS
