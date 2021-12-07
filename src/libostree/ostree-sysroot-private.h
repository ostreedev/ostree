/*
 * Copyright (C) 2012,2013 Colin Walters <walters@verbum.org>
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "libglnx.h"
#include "ostree.h"
#include "ostree-bootloader.h"

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
  OSTREE_SYSROOT_DEBUG_TEST_NO_DTB = 1 << 3, /* https://github.com/ostreedev/ostree/issues/2154 */
} OstreeSysrootDebugFlags;

typedef enum {
      OSTREE_SYSROOT_LOAD_STATE_NONE, /* ostree_sysroot_new() was called */
      OSTREE_SYSROOT_LOAD_STATE_INIT, /* We've loaded basic sysroot state and have an fd */
      OSTREE_SYSROOT_LOAD_STATE_LOADED, /* We've loaded all of the deployments */
} OstreeSysrootLoadState;

/**
 * OstreeSysroot:
 * Internal struct
 */
struct OstreeSysroot {
  GObject parent;

  GFile *path;
  int sysroot_fd;
  int boot_fd;
  GLnxLockFile lock;

  OstreeSysrootLoadState loadstate;
  gboolean mount_namespace_in_use; /* TRUE if caller has told us they used CLONE_NEWNS */
  gboolean root_is_ostree_booted; /* TRUE if sysroot is / and we are booted via ostree */
  /* The device/inode for /, used to detect booted deployment */
  dev_t root_device;
  ino_t root_inode;

  gboolean is_physical; /* TRUE if we're pointed at physical storage root and not a deployment */
  GPtrArray *deployments;
  int bootversion;
  int subbootversion;
  OstreeDeployment *booted_deployment;
  OstreeDeployment *staged_deployment;
  GVariant *staged_deployment_data;
  struct timespec loaded_ts;

  /* Only access through ostree_sysroot_[_get]repo() */
  OstreeRepo *repo;

  OstreeSysrootDebugFlags debug_flags;
};

#define OSTREE_SYSROOT_LOCKFILE "ostree/lock"
/* We keep some transient state in /run */
#define _OSTREE_SYSROOT_RUNSTATE_STAGED "/run/ostree/staged-deployment"
#define _OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED "/run/ostree/staged-deployment-locked"
#define _OSTREE_SYSROOT_RUNSTATE_STAGED_INITRDS_DIR "/run/ostree/staged-initrds/"
#define _OSTREE_SYSROOT_DEPLOYMENT_RUNSTATE_DIR "/run/ostree/deployment-state/"
#define _OSTREE_SYSROOT_DEPLOYMENT_RUNSTATE_FLAG_DEVELOPMENT "unlocked-development"
#define _OSTREE_SYSROOT_DEPLOYMENT_RUNSTATE_FLAG_TRANSIENT "unlocked-transient"

#define _OSTREE_SYSROOT_BOOT_INITRAMFS_OVERLAYS "ostree/initramfs-overlays"
#define _OSTREE_SYSROOT_INITRAMFS_OVERLAYS "boot/" _OSTREE_SYSROOT_BOOT_INITRAMFS_OVERLAYS

gboolean
_ostree_sysroot_ensure_writable (OstreeSysroot      *self,
                                 GError            **error);

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

void
_ostree_deployment_set_bootconfig_from_kargs (OstreeDeployment *deployment,
                                              char            **override_kernel_argv);

gboolean
_ostree_sysroot_reload_staged (OstreeSysroot *self, GError       **error);

gboolean
_ostree_sysroot_finalize_staged (OstreeSysroot *self,
                                 GCancellable  *cancellable,
                                 GError       **error);

OstreeDeployment *
_ostree_sysroot_deserialize_deployment_from_variant (GVariant *v,
                                                     GError  **error);

char *
_ostree_sysroot_get_origin_relpath (GFile         *path,
                                    guint32       *out_device,
                                    guint64       *out_inode,
                                    GCancellable  *cancellable,
                                    GError       **error);

gboolean
_ostree_sysroot_rmrf_deployment (OstreeSysroot *sysroot,
                                 OstreeDeployment *deployment,
                                 GCancellable  *cancellable,
                                 GError       **error);

char * _ostree_sysroot_get_runstate_path (OstreeDeployment *deployment, const char *key);

char *_ostree_sysroot_join_lines (GPtrArray  *lines);

gboolean
_ostree_sysroot_ensure_boot_fd (OstreeSysroot *self, GError **error);

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
