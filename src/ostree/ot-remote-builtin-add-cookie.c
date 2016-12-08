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

#include <libsoup/soup.h>

#include "otutil.h"

#include "ot-main.h"
#include "ot-remote-builtins.h"
#include "ostree-repo-private.h"


static GOptionEntry option_entries[] = {
  { NULL }
};

gboolean
ot_remote_builtin_add_cookie (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  const char *remote_name;
  const char *domain;
  const char *path;
  const char *cookie_name;
  const char *value;
  g_autofree char *jar_path = NULL;
  g_autofree char *cookie_file = NULL;
  glnx_unref_object SoupCookieJar *jar = NULL;
  SoupCookie *cookie;

  context = g_option_context_new ("NAME DOMAIN PATH COOKIE_NAME VALUE - Add a cookie to remote");

  if (!ostree_option_context_parse (context, option_entries, &argc, &argv,
                                    OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    return FALSE;

  if (argc < 6)
    {
      ot_util_usage_error (context, "NAME, DOMAIN, PATH, COOKIE_NAME and VALUE must be specified", error);
      return FALSE;
    }

  remote_name = argv[1];
  domain = argv[2];
  path = argv[3];
  cookie_name = argv[4];
  value = argv[5];

  cookie_file = g_strdup_printf ("%s.cookies.txt", remote_name);
  jar_path = g_build_filename (gs_file_get_path_cached (repo->repodir), cookie_file, NULL);

  jar = soup_cookie_jar_text_new (jar_path, FALSE);

  /* Pick a silly long expire time, we're just storing the cookies in the
   * jar and on pull the jar is read-only so expiry has little actual value */
  cookie = soup_cookie_new (cookie_name, value, domain, path,
                            SOUP_COOKIE_MAX_AGE_ONE_YEAR * 25);

  /* jar takes ownership of cookie */
  soup_cookie_jar_add_cookie (jar, cookie);

  return TRUE;
}
