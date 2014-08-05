/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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

#include "ostree-repo.h"

G_BEGIN_DECLS

#define _OSTREE_OBJECT_SIZES_ENTRY_SIGNATURE "ay"

/**
 * OstreeRepo:
 *
 * Private instance structure.
 */
struct OstreeRepo {
  GObject parent;

  GFile *repodir;
  GFile *tmp_dir;
  int    tmp_dir_fd;
  GFile *local_heads_dir;
  GFile *remote_heads_dir;
  GFile *objects_dir;
  GFile *state_dir;
  int objects_dir_fd;
  GFile *deltas_dir;
  GFile *uncompressed_objects_dir;
  int uncompressed_objects_dir_fd;
  GFile *remote_cache_dir;
  GFile *config_file;

  GFile *transaction_lock_path;
  GHashTable *txn_refs;
  GMutex txn_stats_lock;
  OstreeRepoTransactionStats txn_stats;

  GMutex cache_lock;
  GPtrArray *cached_meta_indexes;
  GPtrArray *cached_content_indexes;

  gboolean inited;
  gboolean in_transaction;
  gboolean disable_fsync;
  GHashTable *loose_object_devino_hash;
  GHashTable *updated_uncompressed_dirs;
  GHashTable *object_sizes;

  GKeyFile *config;
  OstreeRepoMode mode;
  gboolean enable_uncompressed_cache;
  gboolean generate_sizes;

  OstreeRepo *parent_repo;
};

gboolean
_ostree_repo_ensure_loose_objdir_at (int             dfd,
                                     const char     *loose_path,
                                     GCancellable   *cancellable,
                                     GError        **error);

gboolean
_ostree_repo_find_object (OstreeRepo           *self,
                          OstreeObjectType      objtype,
                          const char           *checksum,
                          GFile               **out_stored_path,
                          GCancellable         *cancellable,
                          GError             **error);

GFile *
_ostree_repo_get_commit_metadata_loose_path (OstreeRepo        *self,
                                             const char        *checksum);

gboolean
_ostree_repo_has_loose_object (OstreeRepo           *self,
                               const char           *checksum,
                               OstreeObjectType      objtype,
                               gboolean             *out_is_stored,
                               char                 *loose_path_buf,
                               GCancellable         *cancellable,
                               GError             **error);

gboolean
_ostree_repo_get_loose_object_dirs (OstreeRepo       *self,
                                    GPtrArray       **out_object_dirs,
                                    GCancellable     *cancellable,
                                    GError          **error);

GFile *
_ostree_repo_get_object_path (OstreeRepo   *self,
                              const char   *checksum,
                              OstreeObjectType type);

gboolean
_ostree_repo_write_directory_meta (OstreeRepo   *self,
                                   GFileInfo    *file_info,
                                   GVariant     *xattrs,
                                   guchar      **out_csum,
                                   GCancellable *cancellable,
                                   GError      **error);
gboolean
_ostree_repo_update_refs (OstreeRepo        *self,
                          GHashTable        *refs,
                          GCancellable      *cancellable,
                          GError           **error);

gboolean      
_ostree_repo_write_ref (OstreeRepo    *self,
                        const char    *remote,
                        const char    *ref,
                        const char    *rev,
                        GCancellable  *cancellable,
                        GError       **error);

OstreeRepoFile *
_ostree_repo_file_new_for_commit (OstreeRepo  *repo,
                                  const char  *commit,
                                  GError     **error);

OstreeRepoFile *
_ostree_repo_file_new_root (OstreeRepo  *repo,
                            const char  *contents_checksum,
                            const char  *metadata_checksum);

OstreeRepoCommitFilterResult
_ostree_repo_commit_modifier_apply (OstreeRepo               *self,
                                    OstreeRepoCommitModifier *modifier,
                                    const char               *path,
                                    GFileInfo                *file_info,
                                    GFileInfo               **out_modified_info);

G_END_DECLS

