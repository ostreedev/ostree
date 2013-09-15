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

#pragma once

#include <gio/gio.h>
#include <ostree.h>
#include "ot-ordered-hash.h"

G_BEGIN_DECLS

char *ot_admin_util_split_keyeq (char *str);

gboolean ot_admin_util_get_devino (GFile         *path,
                                   guint32       *out_device,
                                   guint64       *out_inode,
                                   GCancellable  *cancellable,
                                   GError       **error);

gboolean ot_admin_ensure_initialized (GFile         *ostree_dir, 
				      GCancellable  *cancellable,
				      GError       **error);

gboolean ot_admin_check_os (GFile         *sysroot, 
                            const char    *osname,
                            GCancellable  *cancellable,
                            GError       **error);

OtOrderedHash *ot_admin_parse_kernel_args (const char *options);
char * ot_admin_kernel_arg_string_serialize (OtOrderedHash *ohash);

gboolean ot_admin_find_booted_deployment (GFile               *sysroot,
                                          GPtrArray           *deployments,
                                          OstreeDeployment       **out_deployment,
                                          GCancellable        *cancellable,
                                          GError             **error);

gboolean ot_admin_require_booted_deployment (GFile               *sysroot,
                                             OstreeDeployment       **out_deployment,
                                             GCancellable        *cancellable,
                                             GError             **error);

gboolean ot_admin_require_deployment_or_osname (GFile               *sysroot,
                                                GPtrArray           *deployment_list,
                                                const char          *osname,
                                                OstreeDeployment       **out_deployment,
                                                GCancellable        *cancellable,
                                                GError             **error);

OstreeDeployment *ot_admin_get_merge_deployment (GPtrArray         *deployment_list,
                                             const char        *osname,
                                             OstreeDeployment      *booted_deployment);

gboolean ot_admin_get_default_ostree_dir (GFile        **out_ostree_dir,
                                          GCancellable  *cancellable,
                                          GError       **error);

GKeyFile *ot_origin_new_from_refspec (const char *refspec);

G_END_DECLS

