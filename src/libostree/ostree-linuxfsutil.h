/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#include "ostree-types.h"

G_BEGIN_DECLS

gboolean
_ostree_linuxfs_fd_alter_immutable_flag (int            fd,
                                         gboolean       new_immutable_state,
                                         GCancellable  *cancellable,
                                         GError       **error);

G_END_DECLS
