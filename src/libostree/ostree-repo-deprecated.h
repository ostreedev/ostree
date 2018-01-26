/*
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
