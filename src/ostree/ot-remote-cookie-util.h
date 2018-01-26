/*
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#include "libglnx.h"

G_BEGIN_DECLS

gboolean
ot_add_cookie_at (int dfd, const char *jar_path,
                  const char *domain, const char *path,
                  const char *name, const char *value,
                  GError **error);

gboolean
ot_delete_cookie_at (int dfd, const char *jar_path,
                     const char *domain, const char *path,
                     const char *name,
                     GError **error);

gboolean
ot_list_cookies_at (int dfd, const char *jar_path, GError **error);

G_END_DECLS
