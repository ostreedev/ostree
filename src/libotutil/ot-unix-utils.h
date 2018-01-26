/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#pragma once

#include <gio/gio.h>

/* I just put all this shit here. Sue me. */
#include <sys/types.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>

G_BEGIN_DECLS

gboolean ot_util_filename_validate (const char *name, GError **error);

gboolean ot_util_path_split_validate (const char *path, GPtrArray **out_components, GError **error);

G_END_DECLS
