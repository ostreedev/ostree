/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __OT_PULL_H__
#define __OT_PULL_H__

#include "ostree.h"

G_BEGIN_DECLS

typedef enum {
  OSTREE_PULL_FLAGS_NONE,
  OSTREE_PULL_FLAGS_RELATED
} OstreePullFlags;

gboolean ostree_pull (OstreeRepo        *repo,
                      const char        *remote_name,
                      char             **refs_to_fetch,
                      OstreePullFlags    flags,
                      GCancellable      *cancellable,
                      GError           **error);

G_END_DECLS

#endif /* __OT_PRUNE_H__ */
