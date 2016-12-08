/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
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

typedef struct OtCookieParser OtCookieParser;
struct OtCookieParser {
  char *buf;
  char *iter;

  char *line;
  char *domain;
  char *flag;
  char *path;
  char *secure;
  long long unsigned int expiration;
  char *name;
  char *value;
};
void ot_cookie_parser_free (OtCookieParser *parser);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OtCookieParser, ot_cookie_parser_free)

gboolean
ot_parse_cookies_at (int dfd, const char *path,
                     OtCookieParser **out_parser,
                     GCancellable *cancellable,
                     GError **error);
gboolean
ot_parse_cookies_next (OtCookieParser *parser);

G_END_DECLS
