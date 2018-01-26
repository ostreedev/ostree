/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#include <ostree.h>

G_BEGIN_DECLS

gboolean ot_admin_instutil_builtin_selinux_ensure_labeled (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error);
gboolean ot_admin_instutil_builtin_set_kargs (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error);
gboolean ot_admin_instutil_builtin_grub2_generate (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error);

G_END_DECLS
