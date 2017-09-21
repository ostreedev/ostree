/*
 * Copyright (C) 2012,2013 Colin Walters <walters@verbum.org>
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

#include "libglnx.h"
#include "ostree.h"
#include "ostree-kernel-args.h"
#include "ostree-bootloader.h"

G_BEGIN_DECLS

typedef enum {

  /* Don't flag deployments as immutable. */
  OSTREE_SYSROOT_DEBUG_MUTABLE_DEPLOYMENTS = 1 << 0,
  /* See https://github.com/ostreedev/ostree/pull/759 */
  OSTREE_SYSROOT_DEBUG_NO_XATTRS = 1 << 1,
  /* https://github.com/ostreedev/ostree/pull/1049 */
  OSTREE_SYSROOT_DEBUG_TEST_FIFREEZE = 1 << 2,
} OstreeSysrootDebugFlags;

/**
 * OstreeSysroot:
 * Internal struct
 */
struct OstreeSysroot {
  GObject parent;

  GFile *path;
  int sysroot_fd;
  GLnxLockFile lock;

  gboolean loaded;

  gboolean is_physical; /* TRUE if we're pointed at physical storage root and not a deployment */
  GPtrArray *deployments;
  int bootversion;
  int subbootversion;
  OstreeDeployment *booted_deployment;
  struct timespec loaded_ts;

  /* Only access through ostree_sysroot_[_get]repo() */
  OstreeRepo *repo;

  OstreeSysrootDebugFlags debug_flags;
};

#define OSTREE_SYSROOT_LOCKFILE "ostree/lock"
/* We keep some transient state in /run */
#define _OSTREE_SYSROOT_DEPLOYMENT_RUNSTATE_DIR "/run/ostree/deployment-state/"
#define _OSTREE_SYSROOT_DEPLOYMENT_RUNSTATE_FLAG_DEVELOPMENT "unlocked-development"

void
_ostree_sysroot_emit_journal_msg (OstreeSysroot  *self,
                                  const char     *msg);

gboolean
_ostree_sysroot_read_boot_loader_configs (OstreeSysroot *self,
                                          int            bootversion,
                                          GPtrArray    **out_loader_configs,
                                          GCancellable  *cancellable,
                                          GError       **error);

gboolean
_ostree_sysroot_read_current_subbootversion (OstreeSysroot *self,
                                             int            bootversion,
                                             int           *out_subbootversion,
                                             GCancellable  *cancellable,
                                             GError       **error);

gboolean
_ostree_sysroot_parse_deploy_path_name (const char *name,
                                        char      **out_csum,
                                        int        *out_serial,
                                        GError    **error);

gboolean
_ostree_sysroot_list_deployment_dirs_for_os (int                  deploydir_dfd,
                                             const char          *osname,
                                             GPtrArray           *inout_deployments,
                                             GCancellable        *cancellable,
                                             GError             **error);

char *
_ostree_sysroot_get_origin_relpath (GFile         *path,
                                    guint32       *out_device,
                                    guint64       *out_inode,
                                    GCancellable  *cancellable,
                                    GError       **error);

char *_ostree_sysroot_join_lines (GPtrArray  *lines);

gboolean _ostree_sysroot_query_bootloader (OstreeSysroot     *sysroot,
                                           OstreeBootloader **out_bootloader,
                                           GCancellable      *cancellable,
                                           GError           **error);

gboolean _ostree_sysroot_bump_mtime (OstreeSysroot *sysroot,
                                     GError       **error);

gboolean _ostree_sysroot_cleanup_internal (OstreeSysroot *sysroot,
                                           gboolean       prune_repo,
                                           GCancellable  *cancellable,
                                           GError       **error);

G_END_DECLS
