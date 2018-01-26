/*
 * Copyright (C) 2014 Colin Walters <walters@redhat.com>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#include <gio/gio.h>
#include <lzma.h>

G_BEGIN_DECLS

GConverterResult _ostree_lzma_return (lzma_ret value, GError **error);

G_END_DECLS
