/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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

#include <gio/gio.h>

G_BEGIN_DECLS

#define OSTREE_TYPE_CURL_FETCHER         (ostree_curl_fetcher_get_type ())
#define OSTREE_CURL_FETCHER(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), OSTREE_TYPE_CURL_FETCHER, OstreeCurlFetcher))
#define OSTREE_CURL_FETCHER_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), OSTREE_TYPE_CURL_FETCHER, OstreeCurlFetcherClass))
#define OSTREE_IS_CURL_FETCHER(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), OSTREE_TYPE_CURL_FETCHER))
#define OSTREE_IS_CURL_FETCHER_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), OSTREE_TYPE_CURL_FETCHER))
#define OSTREE_CURL_FETCHER_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), OSTREE_TYPE_CURL_FETCHER, OstreeCurlFetcherClass))

typedef struct OstreeCurlFetcherClass   OstreeCurlFetcherClass;
typedef struct OstreeCurlFetcher        OstreeCurlFetcher;

struct OstreeCurlFetcherClass
{
  GObjectClass parent_class;
};

GType   ostree_curl_fetcher_get_type (void) G_GNUC_CONST;

OstreeCurlFetcher *ostree_curl_fetcher_new (GFile *tmpdir);

void ostree_curl_fetcher_request_uri_async (OstreeCurlFetcher         *self,
                                            const char                *uri,
                                            GCancellable              *cancellable,
                                            GAsyncReadyCallback        callback,
                                            gpointer                   user_data);

GFile *ostree_curl_fetcher_request_uri_finish (OstreeCurlFetcher         *self,
                                               GAsyncResult          *result,
                                               GError               **error);

G_END_DECLS

