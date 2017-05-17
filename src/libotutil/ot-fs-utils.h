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
#include "libglnx.h"

G_BEGIN_DECLS

/* This is a copy of https://github.com/GNOME/libglnx/pull/46 until we
 * can do a full port; see https://github.com/ostreedev/ostree/pull/861 */
typedef struct {
  gboolean initialized;
  int src_dfd;
  int fd;
  char *path;
} OtTmpfile;
void ot_tmpfile_clear (OtTmpfile *tmpf);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(OtTmpfile, ot_tmpfile_clear);

gboolean
ot_open_tmpfile_linkable_at (int dfd,
                             const char *subpath,
                             int flags,
                             OtTmpfile *out_tmpf,
                             GError **error);
gboolean
ot_link_tmpfile_at (OtTmpfile *tmpf,
                    GLnxLinkTmpfileReplaceMode flags,
                    int target_dfd,
                    const char *target,
                    GError **error);

GFile * ot_fdrel_to_gfile (int dfd, const char *path);

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


gboolean ot_query_exists_at (int dfd, const char *path,
                             gboolean *out_exists,
                             GError **error);

gboolean ot_ensure_unlinked_at (int dfd,
                                const char *path,
                                GError **error);

gboolean ot_openat_ignore_enoent (int dfd,
                                  const char *path,
                                  int *out_fd,
                                  GError **error);

gboolean ot_dfd_iter_init_allow_noent (int dfd,
                                       const char *path,
                                       GLnxDirFdIterator *dfd_iter,
                                       gboolean *out_exists,
                                       GError **error);

GBytes *ot_file_mapat_bytes (int dfd,
                             const char *path,
                             GError **error);

G_END_DECLS
