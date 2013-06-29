/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

#ifndef __OT_ADMIN_BUILTINS__
#define __OT_ADMIN_BUILTINS__

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
  GFile *sysroot;
} OtAdminBuiltinOpts;

gboolean ot_admin_builtin_os_init (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error);
gboolean ot_admin_builtin_install (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error);
gboolean ot_admin_builtin_init_fs (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error);
gboolean ot_admin_builtin_deploy (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error);
gboolean ot_admin_builtin_prune (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error);
gboolean ot_admin_builtin_status (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error);
gboolean ot_admin_builtin_diff (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error);
gboolean ot_admin_builtin_run_triggers (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error);
gboolean ot_admin_builtin_upgrade (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error);

G_END_DECLS

#endif
