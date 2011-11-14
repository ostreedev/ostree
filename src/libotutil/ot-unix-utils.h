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

#ifndef __OSTREE_UNIX_UTILS_H__
#define __OSTREE_UNIX_UTILS_H__

#include <gio/gio.h>

/* I just put all this shit here. Sue me. */
#include <sys/types.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>

G_BEGIN_DECLS

gboolean ot_util_spawn_pager (GOutputStream  **out_stream, GError         **error);

void ot_util_fatal_literal (const char *msg) G_GNUC_NORETURN;

void ot_util_fatal_gerror (GError *error) G_GNUC_NORETURN;

gboolean ot_util_filename_has_dotdot (const char *path);

GPtrArray *ot_util_sort_filenames_by_component_length (GPtrArray *files);

GPtrArray* ot_util_path_split (const char *path);

char *ot_util_path_join_n (const char *base, GPtrArray *components, int n);

int ot_util_count_filename_components (const char *path);

int ot_util_open_file_read (const char *path, GError **error);

int ot_util_open_file_read_at (int dirfd, const char *name, GError **error);

void ot_util_set_error_from_errno (GError **error, gint saved_errno);

G_END_DECLS

#endif
