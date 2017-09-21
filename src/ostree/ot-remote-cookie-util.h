/*
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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
