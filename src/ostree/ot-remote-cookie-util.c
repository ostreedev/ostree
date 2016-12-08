/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
 * Copyright (C) 2016 Sjoerd Simons <sjoerd@luon.net>
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

#include "config.h"

#include "ot-remote-cookie-util.h"

#include "otutil.h"
#include "ot-main.h"
#include "ot-remote-builtins.h"
#include "ostree-repo-private.h"

static void
ot_cookie_parser_clear (OtCookieParser *parser)
{
  g_clear_pointer (&parser->domain, (GDestroyNotify)g_free);
  g_clear_pointer (&parser->flag, (GDestroyNotify)g_free);
  g_clear_pointer (&parser->path, (GDestroyNotify)g_free);
  g_clear_pointer (&parser->secure, (GDestroyNotify)g_free);
  g_clear_pointer (&parser->name, (GDestroyNotify)g_free);
  g_clear_pointer (&parser->value, (GDestroyNotify)g_free);
}

void
ot_cookie_parser_free (OtCookieParser *parser)
{
  ot_cookie_parser_clear (parser);
  g_free (parser->buf);
  g_free (parser);
}

gboolean
ot_parse_cookies_at (int dfd, const char *path,
                     OtCookieParser **out_parser,
                     GCancellable *cancellable,
                     GError **error)
{
  OtCookieParser *parser;
  g_autofree char *cookies_content = NULL;
  glnx_fd_close int infd = -1;

  infd = openat (dfd, path, O_RDONLY | O_CLOEXEC);
  if (infd < 0)
    {
      if (errno != ENOENT)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }
  else
    {
      cookies_content = glnx_fd_readall_utf8 (infd, NULL, cancellable, error);
      if (!cookies_content)
        return FALSE;
    }

  parser = *out_parser = g_new0 (OtCookieParser, 1);
  parser->buf = g_steal_pointer (&cookies_content);
  parser->iter = parser->buf;
  return TRUE;
}

gboolean
ot_parse_cookies_next (OtCookieParser *parser)
{
  while (parser->iter)
    {
      char *iter = parser->iter;
      char *next = strchr (iter, '\n');

      if (next)
        {
          *next = '\0';
          parser->iter = next + 1;
        }
      else
        parser->iter = NULL;

      ot_cookie_parser_clear (parser);
      if (sscanf (iter, "%ms\t%ms\t%ms\t%ms\t%llu\t%ms\t%ms",
                  &parser->domain,
                  &parser->flag,
                  &parser->path,
                  &parser->secure,
                  &parser->expiration,
                  &parser->name,
                  &parser->value) != 7)
        continue;

      parser->line = iter;
      return TRUE;
    }

  return FALSE;
 
}
