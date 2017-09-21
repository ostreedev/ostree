/*
 * Copyright (C) 2011,2017 Colin Walters <walters@verbum.org>
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"


#ifdef HAVE_LIBCURL
#include "ostree-soup-uri.h"
#else
#define LIBSOUP_USE_UNSTABLE_REQUEST_API
#include <libsoup/soup.h>
#include <libsoup/soup-requester.h>
#include <libsoup/soup-request-http.h>
#endif

#include "ostree-fetcher.h"

#include "libglnx.h"

void
_ostree_fetcher_uri_free (OstreeFetcherURI *uri)
{
  if (uri)
    soup_uri_free ((SoupURI*)uri);
}

OstreeFetcherURI *
_ostree_fetcher_uri_parse (const char       *str,
                           GError          **error)
{
  SoupURI *soupuri = soup_uri_new (str);
  if (soupuri == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to parse uri: %s", str);
      return NULL;
    }
  return (OstreeFetcherURI*)soupuri;
}

static OstreeFetcherURI *
_ostree_fetcher_uri_new_path_internal (OstreeFetcherURI *uri,
                                       gboolean          extend,
                                       const char       *path)
{
  SoupURI *newuri = soup_uri_copy ((SoupURI*)uri);
  if (path)
    {
      if (extend)
        {
          const char *origpath = soup_uri_get_path ((SoupURI*)uri);
          g_autofree char *newpath = g_build_filename (origpath, path, NULL);
          soup_uri_set_path (newuri, newpath);
        }
      else
        {
          soup_uri_set_path (newuri, path);
        }
    }
  return (OstreeFetcherURI*)newuri;
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
  return g_strdup (soup_uri_get_scheme ((SoupURI*)uri));
}

char *
_ostree_fetcher_uri_get_path (OstreeFetcherURI *uri)
{
  return g_strdup (soup_uri_get_path ((SoupURI*)uri));
}

char *
_ostree_fetcher_uri_to_string (OstreeFetcherURI *uri)
{
  return soup_uri_to_string ((SoupURI*)uri, FALSE);
}
