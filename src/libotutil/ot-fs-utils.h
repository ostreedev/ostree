/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>.
 *
 * SPDX-License-Identifier: LGPL-2.0+
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

typedef enum {

  /* Don't flag deployments as immutable. */
  OSTREE_SYSROOT_DEBUG_MUTABLE_DEPLOYMENTS = 1 << 0,
  /* See https://github.com/ostreedev/ostree/pull/759 */
  OSTREE_SYSROOT_DEBUG_NO_XATTRS = 1 << 1,
  /* https://github.com/ostreedev/ostree/pull/1049 */
  OSTREE_SYSROOT_DEBUG_TEST_FIFREEZE = 1 << 2,
  /* This is a temporary flag until we fully drop the explicit `systemctl start
   * ostree-finalize-staged.service` so that tests can exercise the new path unit. */
  OSTREE_SYSROOT_DEBUG_TEST_STAGED_PATH = 1 << 3,
} OstreeSysrootDebugFlags;

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

static inline GLnxFileCopyFlags
ot_sysroot_flags_to_copy_flags (GLnxFileCopyFlags defaults,
                                OstreeSysrootDebugFlags sysrootflags)
{
  if (sysrootflags & OSTREE_SYSROOT_DEBUG_NO_XATTRS)
    defaults |= GLNX_FILE_COPY_NOXATTRS;
  return defaults;
}

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

GBytes *ot_fd_readall_or_mmap (int fd, goffset offset,
                               GError **error);

gboolean
ot_parse_file_by_line (const char    *path,
                       gboolean     (*cb)(const char*, void*, GError**),
                       void          *cbdata,
                       GCancellable  *cancellable,
                       GError       **error);

gboolean
ot_dirfd_copy_attributes_and_xattrs (int            src_parent_dfd,
                                     const char    *src_name,
                                     int            src_dfd,
                                     int            dest_dfd,
                                     OstreeSysrootDebugFlags flags,
                                     GCancellable  *cancellable,
                                     GError       **error);

gboolean
ot_copy_dir_recurse (int              src_parent_dfd,
                     int              dest_parent_dfd,
                     const char      *name,
                     OstreeSysrootDebugFlags flags,
                     GCancellable    *cancellable,
                     GError         **error);

gboolean
ot_is_ro_mount (const char *path);

gboolean
ot_is_rw_mount (const char *path);

G_END_DECLS
