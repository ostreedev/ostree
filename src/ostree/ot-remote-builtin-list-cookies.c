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
ot_remote_builtin_list_cookies (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  glnx_unref_object OstreeRepo *repo = NULL;
  const char *remote_name;
  g_autofree char *jar_path = NULL;
  g_autofree char *cookie_file = NULL;
  glnx_unref_object SoupCookieJar *jar = NULL;
  GSList *cookies;

  context = g_option_context_new ("NAME - Show remote repository cookies");

  if (!ostree_option_context_parse (context, option_entries, &argc, &argv,
                                    OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    return FALSE;

  if (argc < 2)
    {
      ot_util_usage_error (context, "NAME must be specified", error);
      return FALSE;
    }

  remote_name = argv[1];

  cookie_file = g_strdup_printf ("%s.cookies.txt", remote_name);
  jar_path = g_build_filename (g_file_get_path (repo->repodir), cookie_file, NULL);

  jar = soup_cookie_jar_text_new (jar_path, TRUE);
  cookies = soup_cookie_jar_all_cookies (jar);

  while (cookies != NULL)
    {
      SoupCookie *cookie = cookies->data;
      SoupDate *expiry = soup_cookie_get_expires (cookie);

      g_print ("--\n");
      g_print ("Domain: %s\n", soup_cookie_get_domain (cookie));
      g_print ("Path: %s\n", soup_cookie_get_path (cookie));
      g_print ("Name: %s\n", soup_cookie_get_name (cookie));
      g_print ("Secure: %s\n", soup_cookie_get_secure (cookie) ? "yes" : "no");
      g_print ("Expires: %s\n", soup_date_to_string (expiry, SOUP_DATE_COOKIE));
      g_print ("Value: %s\n", soup_cookie_get_value (cookie));

      soup_cookie_free (cookie);
      cookies = g_slist_delete_link (cookies, cookies);
    }

  return TRUE;
}
