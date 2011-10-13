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

#ifndef __HACKTREE_UNIX_UTILS_H__
#define __HACKTREE_UNIX_UTILS_H__

#include <gio/gio.h>
#include <glib-unix.h>

/* I just put all this shit here. Sue me. */
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>

G_BEGIN_DECLS

gboolean ht_util_filename_has_dotdot (const char *path);

GPtrArray *ht_util_sort_filenames_by_component_length (GPtrArray *files);

GPtrArray* ht_util_path_split (const char *path);

char *ht_util_path_join_n (const char *base, GPtrArray *components, int n);

int ht_util_count_filename_components (const char *path);

int ht_util_open_file_read (const char *path, GError **error);

int ht_util_open_file_read_at (int dirfd, const char *name, GError **error);

void ht_util_set_error_from_errno (GError **error, gint saved_errno);

G_END_DECLS

#endif
