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

#ifndef __OSTREE_GIO_UTILS_H__
#define __OSTREE_GIO_UTILS_H__

#include <gio/gio.h>

G_BEGIN_DECLS

/* Basically the stuff that comes from stat() and cheap calls like
 * readlink().  Other things require opening the file, or also
 * stat()ing the parent directory.
 */
#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target,standard::is-hidden," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")

GFileType ot_gfile_type_for_mode (guint32 mode);

GFile *ot_gfile_get_child_strconcat (GFile *parent, const char *first, ...) G_GNUC_NULL_TERMINATED;

GFile *ot_gfile_new_for_path (const char *path);

const char *ot_gfile_get_path_cached (GFile *file);

const char *ot_gfile_get_basename_cached (GFile *file);

gboolean ot_gfile_ensure_directory (GFile *dir, gboolean with_parents, GError **error);

gboolean ot_gfile_rename (GFile          *src,
                          GFile          *dest,
                          GCancellable   *cancellable,
                          GError        **error);

gboolean ot_gfile_unlink (GFile          *path,
                          GCancellable   *cancellable,
                          GError        **error);

gboolean ot_gfile_load_contents_utf8 (GFile         *file,
                                      char         **contents_out,
                                      char         **etag_out,
                                      GCancellable  *cancellable,
                                      GError       **error);

gboolean ot_gio_splice_get_checksum (GOutputStream  *out,
                                     GInputStream   *in,
                                     guchar        **out_csum,
                                     GCancellable   *cancellable,
                                     GError        **error);

gboolean ot_gio_splice_update_checksum (GOutputStream  *out,
                                        GInputStream   *in,
                                        GChecksum      *checksum,
                                        GCancellable   *cancellable,
                                        GError        **error);

gboolean ot_gio_checksum_stream (GInputStream   *in,
                                 guchar        **out_csum,
                                 GCancellable   *cancellable,
                                 GError        **error);

gboolean ot_gfile_merge_dirs (GFile    *destination,
                              GFile    *src,
                              GCancellable *cancellable,
                              GError   **error);

G_END_DECLS

#endif
