/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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

#ifndef __OT_ADMIN_FUNCTIONS__
#define __OT_ADMIN_FUNCTIONS__

#include <gio/gio.h>
#include "ostree.h"
#include "ot-deployment.h"
#include "ot-bootloader.h"
#include "ot-ordered-hash.h"

G_BEGIN_DECLS

gboolean ot_admin_ensure_initialized (GFile         *ostree_dir, 
				      GCancellable  *cancellable,
				      GError       **error);

gboolean ot_admin_check_os (GFile         *sysroot, 
                            const char    *osname,
                            GCancellable  *cancellable,
                            GError       **error);

char *ot_admin_split_keyeq (char *str);
OtOrderedHash *ot_admin_parse_kernel_args (const char *options);
char * ot_admin_kernel_arg_string_serialize (OtOrderedHash *ohash);

OtBootloader *ot_admin_query_bootloader (GFile         *sysroot);

gboolean ot_admin_read_current_subbootversion (GFile         *sysroot,
                                               int            bootversion,
                                               int           *out_subbootversion,
                                               GCancellable  *cancellable,
                                               GError       **error);

gboolean ot_admin_read_boot_loader_configs (GFile         *boot_dir,
                                            int            bootversion,
                                            GPtrArray    **out_loader_configs,
                                            GCancellable  *cancellable,
                                            GError       **error);

gboolean ot_admin_list_deployments (GFile               *sysroot,
                                    int                 *out_bootversion,
                                    GPtrArray          **out_deployments,
                                    GCancellable        *cancellable,
                                    GError             **error);

gboolean ot_admin_find_booted_deployment (GFile               *sysroot,
                                          GPtrArray           *deployments,
                                          OtDeployment       **out_deployment,
                                          GCancellable        *cancellable,
                                          GError             **error);

gboolean ot_admin_require_booted_deployment (GFile               *sysroot,
                                             OtDeployment       **out_deployment,
                                             GCancellable        *cancellable,
                                             GError             **error);

gboolean ot_admin_require_deployment_or_osname (GFile               *sysroot,
                                                GPtrArray           *deployment_list,
                                                const char          *osname,
                                                OtDeployment       **out_deployment,
                                                GCancellable        *cancellable,
                                                GError             **error);

OtDeployment *ot_admin_get_merge_deployment (GPtrArray         *deployment_list,
                                             const char        *osname,
                                             OtDeployment      *booted_deployment,
                                             OtDeployment      *new_deployment);

GFile *ot_admin_get_deployment_origin_path (GFile   *deployment_path);

GFile *ot_admin_get_deployment_directory (GFile        *sysroot,
                                          OtDeployment *deployment);

gboolean ot_admin_get_repo (GFile         *sysroot,
                            OstreeRepo   **out_repo,
                            GCancellable  *cancellable,
                            GError       **error);

gboolean ot_admin_cleanup (GFile               *sysroot,
                           GCancellable        *cancellable,
                           GError             **error);

gboolean ot_admin_get_default_ostree_dir (GFile        **out_ostree_dir,
                                          GCancellable  *cancellable,
                                          GError       **error);

GKeyFile *ot_origin_new_from_refspec (const char *refspec);

gboolean ot_admin_pull (GFile         *ostree_dir,
                        const char    *remote,
                        const char    *ref,
                        GCancellable  *cancellable,
                        GError       **error);

G_END_DECLS

#endif
