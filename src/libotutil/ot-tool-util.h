/*
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean
ot_parse_boolean (const char  *value,
                  gboolean    *out_parsed,
                  GError     **error);
gboolean
ot_parse_keyvalue (const char  *keyvalue,
                   char       **out_key,
                   char       **out_value,
                   GError     **error);

G_END_DECLS
