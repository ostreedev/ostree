/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
