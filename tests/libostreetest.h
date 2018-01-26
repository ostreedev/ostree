/*
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#pragma once

#include <gio/gio.h>
#include <ostree.h>

G_BEGIN_DECLS

gboolean ot_test_run_libtest (const char *cmd, GError **error);

OstreeRepo *ot_test_setup_repo (GCancellable *cancellable,
                                GError **error);

gboolean ot_check_relabeling (gboolean *can_relabel,
                              GError  **error);

gboolean ot_check_user_xattrs (gboolean *has_user_xattrs,
                               GError  **error);

OstreeSysroot *ot_test_setup_sysroot (GCancellable *cancellable,
                                      GError **error);

G_END_DECLS
