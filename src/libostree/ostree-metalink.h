/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#ifndef __GI_SCANNER__

#include "ostree-fetcher.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_METALINK         (_ostree_metalink_get_type ())
#define OSTREE_METALINK(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), OSTREE_TYPE_METALINK, OstreeMetalink))
#define OSTREE_METALINK_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), OSTREE_TYPE_METALINK, OstreeMetalinkClass))
#define OSTREE_IS_METALINK(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), OSTREE_TYPE_METALINK))
#define OSTREE_IS_METALINK_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), OSTREE_TYPE_METALINK))
#define OSTREE_METALINK_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), OSTREE_TYPE_METALINK, OstreeMetalinkClass))

typedef struct OstreeMetalinkClass   OstreeMetalinkClass;
typedef struct OstreeMetalink   OstreeMetalink;

struct OstreeMetalinkClass
{
  GObjectClass parent_class;
};
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeMetalink, g_object_unref)

GType   _ostree_metalink_get_type (void) G_GNUC_CONST;

OstreeMetalink *_ostree_metalink_new (OstreeFetcher  *fetcher,
                                      const char     *requested_file,
                                      guint64         max_size,
                                      OstreeFetcherURI *uri);

gboolean _ostree_metalink_request_sync (OstreeMetalink        *self,
                                        OstreeFetcherURI      **out_target_uri,
                                        GBytes                **out_data,
                                        GCancellable          *cancellable,
                                        GError                **error);
G_END_DECLS

#endif
