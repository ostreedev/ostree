/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#pragma once

#include "ot-main.h"

G_BEGIN_DECLS

#define BUILTINPROTO(name) \
  gboolean ot_admin_builtin_##name (int argc, char **argv, OstreeCommandInvocation *invocation, \
                                    GCancellable *cancellable, GError **error)

BUILTINPROTO (selinux_ensure_labeled);
BUILTINPROTO (os_init);
BUILTINPROTO (install);
BUILTINPROTO (instutil);
BUILTINPROTO (init_fs);
BUILTINPROTO (undeploy);
BUILTINPROTO (set_default);
BUILTINPROTO (deploy);
BUILTINPROTO (cleanup);
BUILTINPROTO (pin);
BUILTINPROTO (finalize_staged);
BUILTINPROTO (boot_complete);
BUILTINPROTO (prepare_soft_reboot);
BUILTINPROTO (impl_prepare_soft_reboot);
BUILTINPROTO (unlock);
BUILTINPROTO (status);
BUILTINPROTO (set_origin);
BUILTINPROTO (diff);
BUILTINPROTO (upgrade);
BUILTINPROTO (kargs);
BUILTINPROTO (post_copy);
BUILTINPROTO (lock_finalization);
BUILTINPROTO (state_overlay);
// Defined manually since "switch" is a keyword and that totally confuses clang-format
gboolean ot_admin_builtin_switch (int argc, char **argv, OstreeCommandInvocation *invocation,
                                  GCancellable *cancellable, GError **error);

#undef BUILTINPROTO

G_END_DECLS
