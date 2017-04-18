/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Endless Mobile, Inc.
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
 * Author: Krzesimir Nowak <krzesimir@kinvolk.io>
 */

#pragma once

#include <ostree.h>

G_BEGIN_DECLS

/* ostree can use g_autoptr backports from libglnx when glib is too
 * old, but still avoid exposing them to users that also have an old
 * glib */
#if defined(OSTREE_COMPILATION) || GLIB_CHECK_VERSION(2, 44, 0)

/*
 * The following types have no specific clear/free/unref functions, so
 * they can be used as the stack-allocated variables or as the
 * g_autofree heap-allocated variables.
 *
 * OstreeRepoTransactionStats
 * OstreeRepoImportArchiveOptions
 * OstreeRepoExportArchiveOptions
 * OstreeRepoCheckoutOptions
 */

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeDiffItem, ostree_diff_item_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeRepoCommitModifier, ostree_repo_commit_modifier_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeRepoDevInoCache, ostree_repo_devino_cache_unref)

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeAsyncProgress, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeBootconfigParser, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeDeployment, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeGpgVerifyResult, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeMutableTree, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeRepo, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeRepoFile, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeSePolicy, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeSysroot, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeSysrootUpgrader, g_object_unref)

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (OstreeRepoCommitTraverseIter, ostree_repo_commit_traverse_iter_clear)

#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeCollectionRef, ostree_collection_ref_free)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC (OstreeCollectionRefv, ostree_collection_ref_freev, NULL)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeRemote, ostree_remote_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeRepoFinder, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeRepoFinderAvahi, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeRepoFinderConfig, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeRepoFinderMount, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeRepoFinderResult, ostree_repo_finder_result_free)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC (OstreeRepoFinderResultv, ostree_repo_finder_result_freev, NULL)
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */

#endif

G_END_DECLS
