/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright © 2017 Endless Mobile, Inc.
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
 * Authors:
 *  - Philip Withnall <withnall@endlessm.com>
 */

#pragma once

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>

#include "ostree-ref.h"
#include "ostree-remote.h"
#include "ostree-types.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_REPO_FINDER (ostree_repo_finder_get_type ())

/* Manually expanded version of the following, omitting autoptr support (for GLib < 2.44):
_OSTREE_PUBLIC
G_DECLARE_INTERFACE (OstreeRepoFinder, ostree_repo_finder, OSTREE, REPO_FINDER, GObject) */

_OSTREE_PUBLIC
GType ostree_repo_finder_get_type (void);
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
typedef struct _OstreeRepoFinder OstreeRepoFinder;
typedef struct _OstreeRepoFinderInterface OstreeRepoFinderInterface;

static inline OstreeRepoFinder *OSTREE_REPO_FINDER (gpointer ptr) { return G_TYPE_CHECK_INSTANCE_CAST (ptr, ostree_repo_finder_get_type (), OstreeRepoFinder); }
static inline gboolean OSTREE_IS_REPO_FINDER (gpointer ptr) { return G_TYPE_CHECK_INSTANCE_TYPE (ptr, ostree_repo_finder_get_type ()); }
static inline OstreeRepoFinderInterface *OSTREE_REPO_FINDER_GET_IFACE (gpointer ptr) { return G_TYPE_INSTANCE_GET_INTERFACE (ptr, ostree_repo_finder_get_type (), OstreeRepoFinderInterface); }
G_GNUC_END_IGNORE_DEPRECATIONS

struct _OstreeRepoFinderInterface
{
  GTypeInterface g_iface;

  void (*resolve_async) (OstreeRepoFinder                  *self,
                         const OstreeCollectionRef * const *refs,
                         OstreeRepo                        *parent_repo,
                         GCancellable                      *cancellable,
                         GAsyncReadyCallback                callback,
                         gpointer                           user_data);
  GPtrArray *(*resolve_finish) (OstreeRepoFinder  *self,
                                GAsyncResult      *result,
                                GError           **error);
};

_OSTREE_PUBLIC
void ostree_repo_finder_resolve_async (OstreeRepoFinder                  *self,
                                       const OstreeCollectionRef * const *refs,
                                       OstreeRepo                        *parent_repo,
                                       GCancellable                      *cancellable,
                                       GAsyncReadyCallback                callback,
                                       gpointer                           user_data);
_OSTREE_PUBLIC
GPtrArray *ostree_repo_finder_resolve_finish (OstreeRepoFinder  *self,
                                              GAsyncResult      *result,
                                              GError           **error);

_OSTREE_PUBLIC
void ostree_repo_finder_resolve_all_async (OstreeRepoFinder * const          *finders,
                                           const OstreeCollectionRef * const *refs,
                                           OstreeRepo                        *parent_repo,
                                           GCancellable                      *cancellable,
                                           GAsyncReadyCallback                callback,
                                           gpointer                           user_data);
_OSTREE_PUBLIC
GPtrArray *ostree_repo_finder_resolve_all_finish (GAsyncResult  *result,
                                                  GError       **error);

/**
 * OstreeRepoFinderResult:
 * @remote: #OstreeRemote which contains the transport details for the result,
 *    such as its URI and GPG key
 * @finder: the #OstreeRepoFinder instance which produced this result
 * @priority: static priority of the result, where higher numbers indicate lower
 *    priority
 * @ref_to_checksum: (element-type OstreeCollectionRef utf8): map of collection–ref
 *    pairs to checksums provided by this remote; values may be %NULL to
 *    indicate this remote doesn’t provide that ref
 * @summary_last_modified: Unix timestamp (seconds since the epoch, UTC) when
 *    the summary file on the remote was last modified, or `0` if unknown
 *
 * #OstreeRepoFinderResult gives a single result from an
 * ostree_repo_finder_resolve_async() or ostree_repo_finder_resolve_all_async()
 * operation. This represents a single remote which provides none, some or all
 * of the refs being resolved. The structure includes various bits of metadata
 * which allow ostree_repo_pull_from_remotes_async() (for example) to prioritise
 * how to pull the refs.
 *
 * The @priority is used as one input of many to ordering functions like
 * ostree_repo_finder_result_compare().
 *
 * @ref_to_checksum indicates which refs (out of the ones queried for as inputs
 * to ostree_repo_finder_resolve_async()) are provided by this remote. The refs
 * are present as keys (of type #OstreeCollectionRef), and the corresponding values
 * are the checksums of the commits the remote currently has for those refs. (These
 * might not be the latest commits available out of all results.) A
 * checksum may be %NULL if the remote does not advertise the corresponding ref.
 * After ostree_repo_finder_resolve_async() has been called, the commit metadata
 * should be available locally, so the details for each checksum can be looked
 * up using ostree_repo_load_commit().
 *
 * Since: 2017.8
 */
typedef struct
{
  OstreeRemote *remote;
  OstreeRepoFinder *finder;
  gint priority;
  GHashTable *ref_to_checksum;
  guint64 summary_last_modified;

  /*< private >*/
  gpointer padding[4];
} OstreeRepoFinderResult;

_OSTREE_PUBLIC
GType ostree_repo_finder_result_get_type (void);

_OSTREE_PUBLIC
OstreeRepoFinderResult *ostree_repo_finder_result_new (OstreeRemote     *remote,
                                                       OstreeRepoFinder *finder,
                                                       gint              priority,
                                                       GHashTable       *ref_to_checksum,
                                                       guint64           summary_last_modified);
_OSTREE_PUBLIC
OstreeRepoFinderResult *ostree_repo_finder_result_dup (OstreeRepoFinderResult *result);
_OSTREE_PUBLIC
gint ostree_repo_finder_result_compare (const OstreeRepoFinderResult *a,
                                        const OstreeRepoFinderResult *b);
_OSTREE_PUBLIC
void ostree_repo_finder_result_free (OstreeRepoFinderResult *result);

/**
 * OstreeRepoFinderResultv:
 *
 * A %NULL-terminated array of #OstreeRepoFinderResult instances, designed to
 * be used with g_auto():
 *
 * |[<!-- language="C" -->
 * g_auto(OstreeRepoFinderResultv) results = NULL;
 * ]|
 *
 * Since: 2017.8
 */
typedef OstreeRepoFinderResult** OstreeRepoFinderResultv;

_OSTREE_PUBLIC
void ostree_repo_finder_result_freev (OstreeRepoFinderResult **results);

G_END_DECLS
