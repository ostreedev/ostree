/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean
ot_keyfile_get_boolean_with_default (GKeyFile      *keyfile,
                                     const char    *section,
                                     const char    *value,
                                     gboolean       default_value,
                                     gboolean      *out_bool,
                                     GError       **error);


gboolean
ot_keyfile_get_value_with_default (GKeyFile      *keyfile,
                                   const char    *section,
                                   const char    *value,
                                   const char    *default_value,
                                   char         **out_value,
                                   GError       **error);

gboolean
ot_keyfile_copy_group (GKeyFile   *source_keyfile,
                       GKeyFile   *target_keyfile,
                       const char *group_name);

G_END_DECLS
