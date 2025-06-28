/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

#include "ostree-deployment.h"
#include "ostree-repo.h"

G_BEGIN_DECLS

/**
 * OSTREE_PATH_BOOTED:
 * Filesystem path that is created on an ostree-booted system.
 *
 * Since: 2022.2
 */
#define OSTREE_PATH_BOOTED "/run/ostree-booted"

#define OSTREE_TYPE_SYSROOT ostree_sysroot_get_type ()
#define OSTREE_SYSROOT(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSTREE_TYPE_SYSROOT, OstreeSysroot))
#define OSTREE_IS_SYSROOT(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSTREE_TYPE_SYSROOT))

_OSTREE_PUBLIC
GType ostree_sysroot_get_type (void);

_OSTREE_PUBLIC
OstreeSysroot *ostree_sysroot_new (GFile *path);

_OSTREE_PUBLIC
OstreeSysroot *ostree_sysroot_new_default (void);

_OSTREE_PUBLIC
void ostree_sysroot_set_mount_namespace_in_use (OstreeSysroot *self);

_OSTREE_PUBLIC
gboolean ostree_sysroot_initialize_with_mount_namespace (OstreeSysroot *self,
                                                         GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
GFile *ostree_sysroot_get_path (OstreeSysroot *self);

_OSTREE_PUBLIC
gboolean ostree_sysroot_is_booted (OstreeSysroot *self);

_OSTREE_PUBLIC
int ostree_sysroot_get_fd (OstreeSysroot *self);

_OSTREE_PUBLIC
gboolean ostree_sysroot_initialize (OstreeSysroot *self, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_load (OstreeSysroot *self, GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_load_if_changed (OstreeSysroot *self, gboolean *out_changed,
                                         GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
void ostree_sysroot_unload (OstreeSysroot *self);

_OSTREE_PUBLIC
gboolean ostree_sysroot_ensure_initialized (OstreeSysroot *self, GCancellable *cancellable,
                                            GError **error);

_OSTREE_PUBLIC
int ostree_sysroot_get_bootversion (OstreeSysroot *self);
_OSTREE_PUBLIC
int ostree_sysroot_get_subbootversion (OstreeSysroot *self);
_OSTREE_PUBLIC
GPtrArray *ostree_sysroot_get_deployments (OstreeSysroot *self);
_OSTREE_PUBLIC
OstreeDeployment *ostree_sysroot_get_booted_deployment (OstreeSysroot *self);
_OSTREE_PUBLIC
OstreeDeployment *ostree_sysroot_require_booted_deployment (OstreeSysroot *self, GError **error);
_OSTREE_PUBLIC
OstreeDeployment *ostree_sysroot_get_staged_deployment (OstreeSysroot *self);

_OSTREE_PUBLIC
GFile *ostree_sysroot_get_deployment_directory (OstreeSysroot *self, OstreeDeployment *deployment);

_OSTREE_PUBLIC
char *ostree_sysroot_get_deployment_dirpath (OstreeSysroot *self, OstreeDeployment *deployment);

_OSTREE_PUBLIC
GFile *ostree_sysroot_get_deployment_origin_path (GFile *deployment_path);

_OSTREE_PUBLIC
gboolean ostree_sysroot_lock (OstreeSysroot *self, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_try_lock (OstreeSysroot *self, gboolean *out_acquired, GError **error);
_OSTREE_PUBLIC
void ostree_sysroot_lock_async (OstreeSysroot *self, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data);
_OSTREE_PUBLIC
gboolean ostree_sysroot_lock_finish (OstreeSysroot *self, GAsyncResult *result, GError **error);
_OSTREE_PUBLIC
void ostree_sysroot_unlock (OstreeSysroot *self);

_OSTREE_PUBLIC
gboolean ostree_sysroot_init_osname (OstreeSysroot *self, const char *osname,
                                     GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_cleanup (OstreeSysroot *self, GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_prepare_cleanup (OstreeSysroot *self, GCancellable *cancellable,
                                         GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_cleanup_prune_repo (OstreeSysroot *sysroot, OstreeRepoPruneOptions *options,
                                            gint *out_objects_total, gint *out_objects_pruned,
                                            guint64 *out_pruned_object_size_total,
                                            GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_write_origin_file (OstreeSysroot *sysroot, OstreeDeployment *deployment,
                                           GKeyFile *new_origin, GCancellable *cancellable,
                                           GError **error);

_OSTREE_PUBLIC
OstreeRepo *ostree_sysroot_repo (OstreeSysroot *self);

_OSTREE_PUBLIC
gboolean ostree_sysroot_get_repo (OstreeSysroot *self, OstreeRepo **out_repo,
                                  GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_deployment_set_kargs (OstreeSysroot *self, OstreeDeployment *deployment,
                                              char **new_kargs, GCancellable *cancellable,
                                              GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_deployment_set_kargs_in_place (OstreeSysroot *self,
                                                       OstreeDeployment *deployment,
                                                       char *kargs_str, GCancellable *cancellable,
                                                       GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_write_deployments (OstreeSysroot *self, GPtrArray *new_deployments,
                                           GCancellable *cancellable, GError **error);

typedef struct
{
  gboolean do_postclean;
  gboolean disable_auto_early_prune;
  gboolean unused_bools[7];
  int unused_ints[7];
  gpointer unused_ptrs[7];
} OstreeSysrootWriteDeploymentsOpts;

_OSTREE_PUBLIC
gboolean ostree_sysroot_write_deployments_with_options (OstreeSysroot *self,
                                                        GPtrArray *new_deployments,
                                                        OstreeSysrootWriteDeploymentsOpts *opts,
                                                        GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_stage_overlay_initrd (OstreeSysroot *self, int fd, char **out_checksum,
                                              GCancellable *cancellable, GError **error);

/**
 * finalization_flags: only used in the staging path
 */
typedef struct
{
  /* If set to true, then this deployment will be staged but "locked" and not automatically applied
   * on reboot. */
  gboolean locked;
  gboolean unused_bools[7];
  int finalization_flags;
  int unused_ints[7];
  char **override_kernel_argv;
  char **overlay_initrds;
  gpointer unused_ptrs[6];
} OstreeSysrootDeployTreeOpts;

_OSTREE_PUBLIC
gboolean ostree_sysroot_deploy_tree (OstreeSysroot *self, const char *osname, const char *revision,
                                     GKeyFile *origin, OstreeDeployment *provided_merge_deployment,
                                     char **override_kernel_argv,
                                     OstreeDeployment **out_new_deployment,
                                     GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_deploy_tree_with_options (OstreeSysroot *self, const char *osname,
                                                  const char *revision, GKeyFile *origin,
                                                  OstreeDeployment *provided_merge_deployment,
                                                  OstreeSysrootDeployTreeOpts *opts,
                                                  OstreeDeployment **out_new_deployment,
                                                  GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_stage_tree (OstreeSysroot *self, const char *osname, const char *revision,
                                    GKeyFile *origin, OstreeDeployment *merge_deployment,
                                    char **override_kernel_argv,
                                    OstreeDeployment **out_new_deployment,
                                    GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_stage_tree_with_options (OstreeSysroot *self, const char *osname,
                                                 const char *revision, GKeyFile *origin,
                                                 OstreeDeployment *merge_deployment,
                                                 OstreeSysrootDeployTreeOpts *opts,
                                                 OstreeDeployment **out_new_deployment,
                                                 GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_change_finalization (OstreeSysroot *self, OstreeDeployment *deployment,
                                             GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_deployment_set_mutable (OstreeSysroot *self, OstreeDeployment *deployment,
                                                gboolean is_mutable, GCancellable *cancellable,
                                                GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_deployment_set_pinned (OstreeSysroot *self, OstreeDeployment *deployment,
                                               gboolean is_pinned, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_deployment_unlock (OstreeSysroot *self, OstreeDeployment *deployment,
                                           OstreeDeploymentUnlockedState unlocked_state,
                                           GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
void ostree_sysroot_query_deployments_for (OstreeSysroot *self, const char *osname,
                                           OstreeDeployment **out_pending,
                                           OstreeDeployment **out_rollback);

_OSTREE_PUBLIC
OstreeDeployment *ostree_sysroot_get_merge_deployment (OstreeSysroot *self, const char *osname);

_OSTREE_PUBLIC
gboolean ostree_sysroot_update_post_copy (OstreeSysroot *self, GCancellable *cancellable,
                                          GError **error);

_OSTREE_PUBLIC
GKeyFile *ostree_sysroot_origin_new_from_refspec (OstreeSysroot *self, const char *refspec);

typedef enum
{
  OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NONE = 0,
  OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN = (1 << 0),
  OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NOT_DEFAULT = (1 << 1),
  OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NO_CLEAN = (1 << 2),
  OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN_PENDING = (1 << 3),
  OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN_ROLLBACK = (1 << 4),
  OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN_PREVIOUS_VERSION = (1 << 5),
} OstreeSysrootSimpleWriteDeploymentFlags;

_OSTREE_PUBLIC
gboolean ostree_sysroot_simple_write_deployment (OstreeSysroot *sysroot, const char *osname,
                                                 OstreeDeployment *new_deployment,
                                                 OstreeDeployment *merge_deployment,
                                                 OstreeSysrootSimpleWriteDeploymentFlags flags,
                                                 GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC gboolean ostree_sysroot_deployment_can_soft_reboot (OstreeSysroot *self,
                                                                   OstreeDeployment *deployment);

_OSTREE_PUBLIC gboolean ostree_sysroot_deployment_prepare_next_root (OstreeSysroot *self,
                                                                     OstreeDeployment *deployment,
                                                                     gboolean allow_kernel_skew,
                                                                     GCancellable *cancellable,
                                                                     GError **error);

_OSTREE_PUBLIC
gboolean ostree_sysroot_deployment_kexec_load (OstreeSysroot *self, OstreeDeployment *deployment,
                                               GCancellable *cancellable, GError **error);

G_END_DECLS
