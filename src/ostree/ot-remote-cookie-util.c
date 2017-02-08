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

#ifndef HAVE_LIBCURL
#include <libsoup/soup.h>
#endif

#include "otutil.h"
#include "ot-main.h"
#include "ot-remote-builtins.h"
#include "ostree-repo-private.h"

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

gboolean
ot_add_cookie_at (int dfd, const char *jar_path,
                  const char *domain, const char *path,
                  const char *name, const char *value,
                  GError **error)
{
#ifdef HAVE_LIBCURL
  glnx_fd_close int fd = openat (AT_FDCWD, jar_path, O_WRONLY | O_APPEND | O_CREAT, 0644);
  g_autofree char *buf = NULL;
  g_autoptr(GDateTime) now = NULL;
  g_autoptr(GDateTime) expires = NULL;

  if (fd < 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  now = g_date_time_new_now_utc ();
  expires = g_date_time_add_years (now, 25);

  /* Adapted from soup-cookie-jar-text.c:write_cookie() */
  buf = g_strdup_printf ("%s\t%s\t%s\t%s\t%llu\t%s\t%s\n",
                         domain,
                         *domain == '.' ? "TRUE" : "FALSE",
                         path,
                         "FALSE",
                         (long long unsigned)g_date_time_to_unix (expires),
                         name,
                         value);
  if (glnx_loop_write (fd, buf, strlen (buf)) < 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }
#else
  glnx_unref_object SoupCookieJar *jar = NULL;
  SoupCookie *cookie;

  jar = soup_cookie_jar_text_new (jar_path, FALSE);

  /* Pick a silly long expire time, we're just storing the cookies in the
   * jar and on pull the jar is read-only so expiry has little actual value */
  cookie = soup_cookie_new (name, value, domain, path,
                            SOUP_COOKIE_MAX_AGE_ONE_YEAR * 25);

  /* jar takes ownership of cookie */
  soup_cookie_jar_add_cookie (jar, cookie);
#endif
  return TRUE;
}

gboolean
ot_delete_cookie_at (int dfd, const char *jar_path,
                     const char *domain, const char *path,
                     const char *name,
                     GError **error)
{
  gboolean found = FALSE;
#ifdef HAVE_LIBCURL
  glnx_fd_close int tempfile_fd = -1;
  g_autofree char *tempfile_path = NULL;
  g_autofree char *dnbuf = NULL;
  const char *dn = NULL;
  g_autoptr(OtCookieParser) parser = NULL;

  if (!ot_parse_cookies_at (dfd, jar_path, &parser, NULL, error))
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
          strcmp (name, parser->name) == 0)
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
#else
  GSList *cookies;
  glnx_unref_object SoupCookieJar *jar = NULL;

  jar = soup_cookie_jar_text_new (jar_path, FALSE);
  cookies = soup_cookie_jar_all_cookies (jar);

  while (cookies != NULL)
    {
      SoupCookie *cookie = cookies->data;

      if (!strcmp (domain, soup_cookie_get_domain (cookie)) &&
          !strcmp (path, soup_cookie_get_path (cookie)) &&
          !strcmp (name, soup_cookie_get_name (cookie)))
        {
          soup_cookie_jar_delete_cookie (jar, cookie);

          found = TRUE;
        }

      soup_cookie_free (cookie);
      cookies = g_slist_delete_link (cookies, cookies);
    }
#endif

  if (!found)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cookie not found in jar");

  return TRUE;
}


gboolean
ot_list_cookies_at (int dfd, const char *jar_path, GError **error)
{
#ifdef HAVE_LIBCURL
  glnx_fd_close int tempfile_fd = -1;
  g_autofree char *tempfile_path = NULL;
  g_autofree char *dnbuf = NULL;
  const char *dn = NULL;
  g_autoptr(OtCookieParser) parser = NULL;

  if (!ot_parse_cookies_at (AT_FDCWD, jar_path, &parser, NULL, error))
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
#else
  glnx_unref_object SoupCookieJar *jar = soup_cookie_jar_text_new (jar_path, TRUE);
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
#endif
  return TRUE;
}
