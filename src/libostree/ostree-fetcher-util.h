/*
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#ifndef __GI_SCANNER__

#include "ostree-fetcher.h"

G_BEGIN_DECLS

/* We used to only send "ostree/" but now include the version
 * https://github.com/ostreedev/ostree/issues/1405
 * This came up in allowing Fedora infrastructure to work around a libcurl bug with HTTP2.
 */
#define OSTREE_FETCHER_USERAGENT_STRING (PACKAGE_NAME "/" PACKAGE_VERSION)

static inline gboolean
_ostree_fetcher_tmpf_from_flags (OstreeFetcherRequestFlags flags,
                                 int                       dfd,
                                 GLnxTmpfile              *tmpf,
                                 GError                  **error)
{
  if ((flags & OSTREE_FETCHER_REQUEST_LINKABLE) > 0)
    {
      if (!glnx_open_tmpfile_linkable_at (dfd, ".", O_RDWR | O_CLOEXEC, tmpf, error))
        return FALSE;
    }
  else if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, tmpf, error))
    return FALSE;

  if (!glnx_fchmod (tmpf->fd, 0644, error))
    return FALSE;
  return TRUE;
}

gboolean _ostree_fetcher_mirrored_request_to_membuf (OstreeFetcher *fetcher,
                                                     GPtrArray     *mirrorlist,
                                                     const char    *filename,
                                                     OstreeFetcherRequestFlags flags,
                                                     GBytes         **out_contents,
                                                     guint64        max_size,
                                                     GCancellable   *cancellable,
                                                     GError         **error);

gboolean _ostree_fetcher_request_uri_to_membuf (OstreeFetcher *fetcher,
                                                OstreeFetcherURI *uri,
                                                OstreeFetcherRequestFlags flags,
                                                GBytes         **out_contents,
                                                guint64        max_size,
                                                GCancellable   *cancellable,
                                                GError         **error);

void _ostree_fetcher_journal_failure (const char *remote_name,
                                      const char *url,
                                      const char *msg);


G_END_DECLS

#endif
