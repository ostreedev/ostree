/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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

gboolean _ostree_fetcher_mirrored_request_to_membuf (OstreeFetcher *fetcher,
                                                     GPtrArray     *mirrorlist,
                                                     const char    *filename,
                                                     gboolean       add_nul,
                                                     gboolean       allow_noent,
                                                     GBytes         **out_contents,
                                                     guint64        max_size,
                                                     GCancellable   *cancellable,
                                                     GError         **error);

gboolean _ostree_fetcher_request_uri_to_membuf (OstreeFetcher *fetcher,
                                                OstreeFetcherURI *uri,
                                                gboolean       add_nul,
                                                gboolean       allow_noent,
                                                GBytes         **out_contents,
                                                guint64        max_size,
                                                GCancellable   *cancellable,
                                                GError         **error);

void _ostree_fetcher_journal_failure (const char *remote_name,
                                      const char *url,
                                      const char *msg);


G_END_DECLS

#endif
