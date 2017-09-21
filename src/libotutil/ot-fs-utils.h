/*
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

/* A little helper to call unlinkat() as a cleanup
 * function.  Mostly only necessary to handle
 * deletion of temporary symlinks.
 */
typedef struct {
  int dfd;
  char *path;
} OtCleanupUnlinkat;

static inline void
ot_cleanup_unlinkat_clear (OtCleanupUnlinkat *cleanup)
{
  g_clear_pointer (&cleanup->path, g_free);
}

static inline void
ot_cleanup_unlinkat (OtCleanupUnlinkat *cleanup)
{
  if (cleanup->path)
    {
      (void) unlinkat (cleanup->dfd, cleanup->path, 0);
      ot_cleanup_unlinkat_clear (cleanup);
    }
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(OtCleanupUnlinkat, ot_cleanup_unlinkat)

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

GBytes *
ot_map_anonymous_tmpfile_from_content (GInputStream *instream,
                                       GCancellable *cancellable,
                                       GError      **error);

GBytes *ot_file_mapat_bytes (int dfd,
                             const char *path,
                             GError **error);

G_END_DECLS
