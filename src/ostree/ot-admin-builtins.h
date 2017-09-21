/*
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

#pragma once

#include <ostree.h>

G_BEGIN_DECLS

gboolean ot_admin_builtin_selinux_ensure_labeled (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_admin_builtin_os_init (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_admin_builtin_install (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_admin_builtin_instutil (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_admin_builtin_init_fs (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_admin_builtin_undeploy (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_admin_builtin_deploy (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_admin_builtin_cleanup (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_admin_builtin_unlock (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_admin_builtin_status (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_admin_builtin_set_origin (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_admin_builtin_diff (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_admin_builtin_switch (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_admin_builtin_upgrade (int argc, char **argv, GCancellable *cancellable, GError **error);

G_END_DECLS
