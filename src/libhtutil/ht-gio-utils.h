/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>.
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

#ifndef __HACKTREE_GIO_UTILS_H__
#define __HACKTREE_GIO_UTILS_H__

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean ht_util_ensure_directory (const char *path, gboolean with_parents, GError **error);

char * ht_util_get_file_contents_utf8 (const char *path, GError    **error);

GInputStream *ht_util_read_file_noatime (GFile *file, GCancellable *cancellable, GError **error);

G_END_DECLS

#endif
