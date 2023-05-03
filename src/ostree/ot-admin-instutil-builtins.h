/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include "ot-main.h"

G_BEGIN_DECLS

gboolean ot_admin_instutil_builtin_selinux_ensure_labeled (int argc, char **argv,
                                                           OstreeCommandInvocation *invocation,
                                                           GCancellable *cancellable,
                                                           GError **error);
gboolean ot_admin_instutil_builtin_set_kargs (int argc, char **argv,
                                              OstreeCommandInvocation *invocation,
                                              GCancellable *cancellable, GError **error);
gboolean ot_admin_instutil_builtin_grub2_generate (int argc, char **argv,
                                                   OstreeCommandInvocation *invocation,
                                                   GCancellable *cancellable, GError **error);

G_END_DECLS
