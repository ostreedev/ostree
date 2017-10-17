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

#define BUILTINPROTO(name) gboolean ot_admin_builtin_ ## name (int argc, char **argv, \
                                                               OstreeCommandInvocation *invocation, \
                                                               GCancellable *cancellable, GError **error)

BUILTINPROTO(selinux_ensure_labeled);
BUILTINPROTO(os_init);
BUILTINPROTO(install);
BUILTINPROTO(instutil);
BUILTINPROTO(init_fs);
BUILTINPROTO(undeploy);
BUILTINPROTO(deploy);
BUILTINPROTO(cleanup);
BUILTINPROTO(unlock);
BUILTINPROTO(status);
BUILTINPROTO(set_origin);
BUILTINPROTO(diff);
BUILTINPROTO(switch);
BUILTINPROTO(upgrade);

#undef BUILTINPROTO

G_END_DECLS
