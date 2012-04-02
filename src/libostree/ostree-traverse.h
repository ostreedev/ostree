/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

#ifndef _OSTREE_TRAVERSE
#define _OSTREE_TRAVERSE

#include "ostree-core.h"
#include "ostree-types.h"

G_BEGIN_DECLS

GHashTable *ostree_traverse_new_reachable (void);

gboolean ostree_traverse_dirtree (OstreeRepo         *repo,
                                  const char         *commit_checksum,
                                  GHashTable         *inout_reachable,
                                  GCancellable       *cancellable,
                                  GError            **error);

gboolean ostree_traverse_commit (OstreeRepo         *repo,
                                 const char         *commit_checksum,
                                 int                 maxdepth,
                                 GHashTable         *inout_reachable,
                                 GCancellable       *cancellable,
                                 GError            **error);

G_END_DECLS

#endif /* _OSTREE_REPO */
