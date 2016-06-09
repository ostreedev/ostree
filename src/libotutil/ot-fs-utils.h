/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>.
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
 */

#pragma once

#include "ot-unix-utils.h"

G_BEGIN_DECLS

int ot_opendirat (int dfd, const char *path, gboolean follow);
gboolean ot_gopendirat (int             dfd,
                        const char     *path,
                        gboolean        follow,
                        int            *out_fd,
                        GError        **error);

GBytes * ot_lgetxattrat (int            dfd,
                         const char    *path,
                         const char    *attribute,
                         GError       **error);

gboolean ot_lsetxattrat (int            dfd,
                         const char    *path,
                         const char    *attribute,
                         const void    *value,
                         gsize          value_size,
                         int            flags,
                         GError       **error);

gboolean ot_readlinkat_gfile_info (int             dfd,
                                   const char     *path,
                                   GFileInfo      *target_info,
                                   GCancellable   *cancellable,
                                   GError        **error);

gboolean ot_openat_read_stream (int             dfd,
                                const char     *path,
                                gboolean        follow,
                                GInputStream  **out_istream,
                                GCancellable   *cancellable,
                                GError        **error);

gboolean ot_ensure_unlinked_at (int dfd,
                                const char *path,
                                GError **error);

gboolean ot_openat_ignore_enoent (int dfd,
                                  const char *path,
                                  int *out_fd,
                                  GError **error);

GBytes *ot_file_mapat_bytes (int dfd,
                             const char *path,
                             GError **error);

G_END_DECLS
