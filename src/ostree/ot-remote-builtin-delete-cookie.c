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

#ifndef HAVE_LIBCURL
#include <libsoup/soup.h>
#endif

#include "otutil.h"
#include <sys/stat.h>

#include "ot-main.h"
#include "ot-remote-builtins.h"
#include "ostree-repo-private.h"
#include "ot-remote-cookie-util.h"

static GOptionEntry option_entries[] = {
  { NULL }
};

gboolean
ot_remote_builtin_delete_cookie (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  const char *remote_name;
  const char *domain;
  const char *path;
  const char *cookie_name;
  g_autofree char *jar_path = NULL;
  g_autofree char *cookie_file = NULL;
  gboolean found = FALSE;

  context = g_option_context_new ("NAME DOMAIN PATH COOKIE_NAME- Remote one cookie from remote");

  if (!ostree_option_context_parse (context, option_entries, &argc, &argv,
                                    OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    return FALSE;

  if (argc < 5)
    {
      ot_util_usage_error (context, "NAME, DOMAIN, PATH and COOKIE_NAME must be specified", error);
      return FALSE;
    }

  remote_name = argv[1];
  domain = argv[2];
  path = argv[3];
  cookie_name = argv[4];

  cookie_file = g_strdup_printf ("%s.cookies.txt", remote_name);
  jar_path = g_build_filename (gs_file_get_path_cached (repo->repodir), cookie_file, NULL);

#ifdef HAVE_LIBCURL
  { glnx_fd_close int tempfile_fd = -1;
    g_autofree char *tempfile_path = NULL;
    g_autofree char *dnbuf = NULL;
    const char *dn = NULL;
    g_autoptr(OtCookieParser) parser = NULL;

    if (!ot_parse_cookies_at (AT_FDCWD, jar_path, &parser, cancellable, error))
      return FALSE;

    dnbuf = g_strdup (jar_path);
    dn = dirname (dnbuf);
    if (!glnx_open_tmpfile_linkable_at (AT_FDCWD, dn, O_WRONLY | O_CLOEXEC,
                                        &tempfile_fd, &tempfile_path,
                                        error))
      return FALSE;

    while (ot_parse_cookies_next (parser))
      {
        if (strcmp (domain, parser->domain) == 0 &&
            strcmp (path, parser->path) == 0 &&
            strcmp (cookie_name, parser->name) == 0)
          {
            found = TRUE;
            /* Match, skip writing this one */
            continue;
          }

        if (glnx_loop_write (tempfile_fd, parser->line, strlen (parser->line)) < 0 ||
            glnx_loop_write (tempfile_fd, "\n", 1) < 0)
          {
            glnx_set_error_from_errno (error);
            return FALSE;
          }
     }

    if (!glnx_link_tmpfile_at (AT_FDCWD, GLNX_LINK_TMPFILE_REPLACE,
                               tempfile_fd,
                               tempfile_path,
                               AT_FDCWD, jar_path,
                               error))
      return FALSE;
  }
#else
  { GSList *cookies;
    glnx_unref_object SoupCookieJar *jar = NULL;
  jar = soup_cookie_jar_text_new (jar_path, FALSE);
  cookies = soup_cookie_jar_all_cookies (jar);

  while (cookies != NULL)
    {
      SoupCookie *cookie = cookies->data;

      if (!strcmp (domain, soup_cookie_get_domain (cookie)) &&
          !strcmp (path, soup_cookie_get_path (cookie)) &&
          !strcmp (cookie_name, soup_cookie_get_name (cookie)))
        {
          soup_cookie_jar_delete_cookie (jar, cookie);

          found = TRUE;
        }

      soup_cookie_free (cookie);
      cookies = g_slist_delete_link (cookies, cookies);
    }
  }
#endif

  if (!found)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cookie not found in jar");

  return found;
}
