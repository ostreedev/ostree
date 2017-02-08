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

#include "otutil.h"

#include "ot-main.h"
#include "ot-remote-builtins.h"
#include "ostree-repo-private.h"
#include "ot-remote-cookie-util.h"

static GOptionEntry option_entries[] = {
  { NULL }
};

gboolean
ot_remote_builtin_list_cookies (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  const char *remote_name;
  g_autofree char *jar_path = NULL;
  g_autofree char *cookie_file = NULL;

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

#ifdef HAVE_LIBCURL
  { glnx_fd_close int tempfile_fd = -1;
    g_autofree char *tempfile_path = NULL;
    g_autofree char *dnbuf = NULL;
    const char *dn = NULL;
    g_autoptr(OtCookieParser) parser = NULL;

    if (!ot_parse_cookies_at (AT_FDCWD, jar_path, &parser, cancellable, error))
      return FALSE;

    dnbuf = dirname (g_strdup (jar_path));
    dn = dnbuf;
    if (!glnx_open_tmpfile_linkable_at (AT_FDCWD, dn, O_WRONLY | O_CLOEXEC,
                                        &tempfile_fd, &tempfile_path,
                                        error))
      return FALSE;

    while (ot_parse_cookies_next (parser))
      {
        g_autoptr(GDateTime) expires = g_date_time_new_from_unix_utc (parser->expiration);
        g_autofree char *expires_str = g_date_time_format (expires, "%Y-%m-%d %H:%M:%S +0000");

        g_print ("--\n");
        g_print ("Domain: %s\n", parser->domain);
        g_print ("Path: %s\n", parser->path);
        g_print ("Name: %s\n", parser->name);
        g_print ("Secure: %s\n", parser->secure);
        g_print ("Expires: %s\n", expires_str);
        g_print ("Value: %s\n", parser->value);
     }
  }
#else
  { glnx_unref_object SoupCookieJar *jar = soup_cookie_jar_text_new (jar_path, TRUE);
    GSList *cookies = soup_cookie_jar_all_cookies (jar);

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
  }
#endif

  return TRUE;
}
