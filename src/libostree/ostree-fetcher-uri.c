/*
 * Copyright (C) 2011,2017 Colin Walters <walters@verbum.org>
 * Copyright (C) 2022 Igalia S.L.
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <glib.h>

#include "ostree-fetcher.h"

#include "libglnx.h"

void
_ostree_fetcher_uri_free (OstreeFetcherURI *uri)
{
  if (uri)
    g_uri_unref ((GUri*)uri);
}

OstreeFetcherURI *
_ostree_fetcher_uri_parse (const char       *str,
                           GError          **error)
{
  GUri *uri = NULL;
#if GLIB_CHECK_VERSION(2, 68, 0)
  uri = g_uri_parse (str, G_URI_FLAGS_HAS_PASSWORD | G_URI_FLAGS_ENCODED | G_URI_FLAGS_SCHEME_NORMALIZE, error);
#else
  /* perform manual scheme normalization for older glib */
  uri = g_uri_parse (str, G_URI_FLAGS_HAS_PASSWORD | G_URI_FLAGS_ENCODED, error);
  if (uri)
    {
      GUri *nuri = NULL;
      switch (g_uri_get_port (uri))
        {
          case 21:
            if (!strcmp (g_uri_get_scheme (uri), "ftp"))
              break;
            return (OstreeFetcherURI*)uri;
          case 80:
            if (!strcmp (g_uri_get_scheme (uri), "http"))
              break;
            return (OstreeFetcherURI*)uri;
          case 443:
            if (!strcmp (g_uri_get_scheme (uri), "https"))
              break;
            return (OstreeFetcherURI*)uri;
          default:
            return (OstreeFetcherURI*)uri;
        }
      nuri = g_uri_build_with_user (g_uri_get_flags (uri), "http",
                                    g_uri_get_user (uri),
                                    g_uri_get_password (uri),
                                    NULL,
                                    g_uri_get_host (uri), -1,
                                    g_uri_get_path (uri),
                                    g_uri_get_query (uri),
                                    g_uri_get_fragment (uri));
      g_uri_unref (uri);
      uri = nuri;
    }
#endif
  return (OstreeFetcherURI*)uri;
}

static OstreeFetcherURI *
_ostree_fetcher_uri_new_path_internal (OstreeFetcherURI *uri,
                                       gboolean          extend,
                                       const char       *path)
{
  GUri *guri = (GUri*)uri;
  const char *opath = g_uri_get_path (guri);
  g_autofree char *newpath = NULL;
  if (path)
    {
      if (extend)
        {
          newpath = g_build_filename (opath, path, NULL);
          opath = newpath;
        }
      else
        {
          opath = path;
        }
    }
  return (OstreeFetcherURI*)g_uri_build_with_user (g_uri_get_flags (guri),
                                                   g_uri_get_scheme (guri),
                                                   g_uri_get_user (guri),
                                                   g_uri_get_password (guri),
                                                   NULL,
                                                   g_uri_get_host (guri),
                                                   g_uri_get_port (guri),
                                                   opath,
                                                   g_uri_get_query (guri),
                                                   g_uri_get_fragment (guri));
}

OstreeFetcherURI *
_ostree_fetcher_uri_new_path (OstreeFetcherURI *uri,
                              const char       *path)
{
  return _ostree_fetcher_uri_new_path_internal (uri, FALSE, path);
}

OstreeFetcherURI *
_ostree_fetcher_uri_new_subpath (OstreeFetcherURI *uri,
                                 const char       *subpath)
{
  return _ostree_fetcher_uri_new_path_internal (uri, TRUE, subpath);
}

OstreeFetcherURI *
_ostree_fetcher_uri_clone (OstreeFetcherURI *uri)
{
  return _ostree_fetcher_uri_new_subpath (uri, NULL);
}

char *
_ostree_fetcher_uri_get_scheme (OstreeFetcherURI *uri)
{
  return g_strdup (g_uri_get_scheme ((GUri*)uri));
}

char *
_ostree_fetcher_uri_get_path (OstreeFetcherURI *uri)
{
  return g_strdup (g_uri_get_path ((GUri*)uri));
}

char *
_ostree_fetcher_uri_to_string (OstreeFetcherURI *uri)
{
  return g_uri_to_string_partial ((GUri*)uri, G_URI_HIDE_PASSWORD);
}


/* Only accept http, https, and file; particularly curl has a ton of other
 * backends like sftp that we don't want, and this also gracefully filters
 * out invalid input.
 */
gboolean
_ostree_fetcher_uri_validate (OstreeFetcherURI *uri, GError **error) 
{
  const char *scheme = g_uri_get_scheme ((GUri*)uri);
  // TODO only allow file if explicitly requested by a higher level
  if (!(g_str_equal (scheme, "http") || g_str_equal (scheme, "https") || g_str_equal (scheme, "file")))
    {
      g_autofree char *s = _ostree_fetcher_uri_to_string (uri);
      return glnx_throw (error, "Invalid URI scheme in %s", s);
    }
  return TRUE;
}
