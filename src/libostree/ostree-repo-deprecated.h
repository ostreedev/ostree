/*
 * Copyright (C) 2016 Red Hat, Inc.
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

#include "ostree-core.h"
#include "ostree-types.h"

#ifndef G_GNUC_DEPRECATED_FOR
# define G_GNUC_DEPRECATED_FOR(x)
#endif

G_BEGIN_DECLS

/**
 * OstreeRepoCheckoutOptions: (skip)
 *
 * An extensible options structure controlling checkout.  Ensure that
 * you have entirely zeroed the structure, then set just the desired
 * options.  This is used by ostree_repo_checkout_tree_at() which
 * supercedes previous separate enumeration usage in
 * ostree_repo_checkout_tree().
 */
typedef struct {
  OstreeRepoCheckoutMode mode;
  OstreeRepoCheckoutOverwriteMode overwrite_mode;

  guint enable_uncompressed_cache : 1;
  guint disable_fsync : 1;
  guint process_whiteouts : 1;
  guint no_copy_fallback : 1;
  guint reserved : 28;

  const char *subpath;

  OstreeRepoDevInoCache *devino_to_csum_cache;

  guint unused_uints[6];
  gpointer unused_ptrs[7];
} OstreeRepoCheckoutOptions;

_OSTREE_PUBLIC
gboolean ostree_repo_checkout_tree_at (OstreeRepo                         *self,
                                       OstreeRepoCheckoutOptions          *options,
                                       int                                 destination_dfd,
                                       const char                         *destination_path,
                                       const char                         *commit,
                                       GCancellable                       *cancellable,
                                       GError                            **error)
G_GNUC_DEPRECATED_FOR(ostree_repo_checkout_at);

G_END_DECLS
