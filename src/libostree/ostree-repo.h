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

#pragma once

#include "ostree-core.h"
#include "ostree-types.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_REPO ostree_repo_get_type()
#define OSTREE_REPO(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSTREE_TYPE_REPO, OstreeRepo))
#define OSTREE_IS_REPO(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSTREE_TYPE_REPO))

GType ostree_repo_get_type (void);

OstreeRepo* ostree_repo_new (GFile *path);

gboolean      ostree_repo_check (OstreeRepo  *self, GError **error);

GFile *       ostree_repo_get_path (OstreeRepo  *self);

typedef enum {
  OSTREE_REPO_MODE_BARE,
  OSTREE_REPO_MODE_ARCHIVE,
  OSTREE_REPO_MODE_ARCHIVE_Z2
} OstreeRepoMode;

gboolean       ostree_repo_mode_from_string (const char      *mode,
                                             OstreeRepoMode  *out_mode,
                                             GError         **error);

OstreeRepoMode ostree_repo_get_mode (OstreeRepo  *self);

GFile *       ostree_repo_get_tmpdir (OstreeRepo  *self);

GKeyFile *    ostree_repo_get_config (OstreeRepo *self);

GKeyFile *    ostree_repo_copy_config (OstreeRepo *self);

OstreeRepo * ostree_repo_get_parent (OstreeRepo  *self);

gboolean      ostree_repo_write_config (OstreeRepo *self,
                                        GKeyFile   *new_config,
                                        GError    **error);

GFile *       ostree_repo_get_object_path (OstreeRepo   *self,
                                           const char   *object,
                                           OstreeObjectType type);

GFile *       ostree_repo_get_archive_content_path (OstreeRepo    *self,
                                                    const char    *checksum);

GFile *       ostree_repo_get_file_object_path (OstreeRepo   *self,
                                                const char   *object);

gboolean      ostree_repo_prepare_transaction (OstreeRepo     *self,
                                               gboolean        enable_commit_hardlink_scan,
                                               gboolean       *out_transaction_resume,
                                               GCancellable   *cancellable,
                                               GError        **error);

gboolean      ostree_repo_commit_transaction (OstreeRepo     *self,
                                              GCancellable   *cancellable,
                                              GError        **error);

gboolean      ostree_repo_abort_transaction (OstreeRepo     *self,
                                             GCancellable   *cancellable,
                                             GError        **error);

gboolean      ostree_repo_has_object (OstreeRepo           *self,
                                      OstreeObjectType      objtype,
                                      const char           *checksum,
                                      gboolean             *out_have_object,
                                      GCancellable         *cancellable,
                                      GError              **error);

gboolean      ostree_repo_stage_metadata (OstreeRepo        *self,
                                          OstreeObjectType   objtype,
                                          const char        *expected_checksum,
                                          GVariant          *object,
                                          guchar           **out_csum,
                                          GCancellable      *cancellable,
                                          GError           **error);

void          ostree_repo_stage_metadata_async (OstreeRepo              *self,
                                                OstreeObjectType         objtype,
                                                const char              *expected_checksum,
                                                GVariant                *object,
                                                GCancellable            *cancellable,
                                                GAsyncReadyCallback      callback,
                                                gpointer                 user_data);

gboolean      ostree_repo_stage_metadata_finish (OstreeRepo        *self,
                                                 GAsyncResult      *result,
                                                 guchar           **out_csum,
                                                 GError           **error);

gboolean      ostree_repo_stage_content (OstreeRepo       *self,
                                         const char       *expected_checksum,
                                         GInputStream     *content,
                                         guint64           content_length,
                                         guchar          **out_csum,
                                         GCancellable     *cancellable,
                                         GError          **error);

gboolean      ostree_repo_stage_metadata_trusted (OstreeRepo        *self,
                                                  OstreeObjectType   objtype,
                                                  const char        *checksum,
                                                  GVariant          *object,
                                                  GCancellable      *cancellable,
                                                  GError           **error);

gboolean      ostree_repo_stage_content_trusted (OstreeRepo       *self,
                                                 const char       *checksum,
                                                 GInputStream     *content,
                                                 guint64           content_length,
                                                 GCancellable     *cancellable,
                                                 GError          **error);

void          ostree_repo_stage_content_async (OstreeRepo              *self,
                                               const char              *expected_checksum,
                                               GInputStream            *object,
                                               guint64                  file_object_length,
                                               GCancellable            *cancellable,
                                               GAsyncReadyCallback      callback,
                                               gpointer                 user_data);

gboolean      ostree_repo_stage_content_finish (OstreeRepo        *self,
                                                GAsyncResult      *result,
                                                guchar           **out_csum,
                                                GError           **error);

gboolean      ostree_repo_resolve_rev (OstreeRepo  *self,
                                       const char  *refspec,
                                       gboolean     allow_noent,
                                       char       **out_resolved,
                                       GError     **error);

gboolean      ostree_repo_write_ref (OstreeRepo  *self,
                                     const char  *remote,
                                     const char  *name,
                                     const char  *rev,
                                     GError     **error);

gboolean      ostree_repo_write_refspec (OstreeRepo  *self,
                                         const char  *refspec,
                                         const char  *rev,
                                         GError     **error);

gboolean      ostree_repo_list_refs (OstreeRepo       *repo,
                                     const char       *refspec_prefix,
                                     GHashTable      **out_all_refs,
                                     GCancellable     *cancellable,
                                     GError          **error);

gboolean      ostree_repo_load_variant_c (OstreeRepo  *self,
                                          OstreeObjectType expected_type,
                                          const guchar  *csum,       
                                          GVariant     **out_variant,
                                          GError       **error);

gboolean      ostree_repo_load_variant (OstreeRepo  *self,
                                        OstreeObjectType expected_type,
                                        const char    *sha256, 
                                        GVariant     **out_variant,
                                        GError       **error);

gboolean      ostree_repo_load_variant_if_exists (OstreeRepo  *self,
                                                  OstreeObjectType expected_type,
                                                  const char    *sha256, 
                                                  GVariant     **out_variant,
                                                  GError       **error);

gboolean ostree_repo_load_file (OstreeRepo         *self,
                                const char         *entry_sha256,
                                GInputStream      **out_input,
                                GFileInfo         **out_file_info,
                                GVariant          **out_xattrs,
                                GCancellable       *cancellable,
                                GError            **error);

typedef enum {
  OSTREE_REPO_COMMIT_FILTER_ALLOW,
  OSTREE_REPO_COMMIT_FILTER_SKIP
} OstreeRepoCommitFilterResult;

typedef OstreeRepoCommitFilterResult (*OstreeRepoCommitFilter) (OstreeRepo    *repo,
                                                                const char    *path,
                                                                GFileInfo     *file_info,
                                                                gpointer       user_data);

typedef struct {
  volatile gint refcount;

  guint reserved_flags : 31;
  guint skip_xattrs : 1;

  OstreeRepoCommitFilter filter;
  gpointer user_data;

  gpointer reserved[3];
} OstreeRepoCommitModifier;

OstreeRepoCommitModifier *ostree_repo_commit_modifier_new (void);

void ostree_repo_commit_modifier_unref (OstreeRepoCommitModifier *modifier);

gboolean      ostree_repo_stage_directory_to_mtree (OstreeRepo         *self,
                                                    GFile              *dir,
                                                    OstreeMutableTree  *mtree,
                                                    OstreeRepoCommitModifier *modifier,
                                                    GCancellable *cancellable,
                                                    GError      **error);

gboolean      ostree_repo_stage_archive_to_mtree (OstreeRepo         *self,
                                                  GFile              *archive,
                                                  OstreeMutableTree  *tree,
                                                  OstreeRepoCommitModifier *modifier,
                                                  gboolean            autocreate_parents,
                                                  GCancellable *cancellable,
                                                  GError      **error);

gboolean      ostree_repo_stage_mtree (OstreeRepo         *self,
                                       OstreeMutableTree  *tree,
                                       char              **out_contents_checksum,
                                       GCancellable       *cancellable,
                                       GError            **error);

gboolean      ostree_repo_stage_commit (OstreeRepo   *self,
                                        const char   *branch,
                                        const char   *parent,
                                        const char   *subject,
                                        const char   *body,
                                        const char   *content_checksum,
                                        const char   *metadata_checksum,
                                        char        **out_commit,
                                        GCancellable *cancellable,
                                        GError      **error);

typedef enum {
  OSTREE_REPO_CHECKOUT_MODE_NONE = 0,
  OSTREE_REPO_CHECKOUT_MODE_USER = 1
} OstreeRepoCheckoutMode;

typedef enum {
  OSTREE_REPO_CHECKOUT_OVERWRITE_NONE = 0,
  OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES = 1
} OstreeRepoCheckoutOverwriteMode;

void
ostree_repo_checkout_tree_async (OstreeRepo               *self,
                                 OstreeRepoCheckoutMode    mode,
                                 OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                                 GFile                    *destination,
                                 OstreeRepoFile           *source,
                                 GFileInfo                *source_info,
                                 GCancellable             *cancellable,
                                 GAsyncReadyCallback       callback,
                                 gpointer                  user_data);

gboolean
ostree_repo_checkout_tree_finish (OstreeRepo               *self,
                                  GAsyncResult             *result,
                                  GError                  **error);

gboolean       ostree_repo_checkout_gc (OstreeRepo        *self,
                                        GCancellable      *cancellable,
                                        GError           **error);

gboolean       ostree_repo_read_commit (OstreeRepo *self,
                                        const char *rev,
                                        GFile       **out_root,
                                        GCancellable *cancellable,
                                        GError  **error);

typedef enum {
  OSTREE_REPO_LIST_OBJECTS_LOOSE = (1 << 0),
  OSTREE_REPO_LIST_OBJECTS_PACKED = (1 << 1),
  OSTREE_REPO_LIST_OBJECTS_ALL = (1 << 2)
} OstreeRepoListObjectsFlags;

/**
 * OSTREE_REPO_LIST_OBJECTS_VARIANT_TYPE:
 *
 * b - %TRUE if object is available "loose"
 * as - List of pack file checksums in which this object appears
 */
#define OSTREE_REPO_LIST_OBJECTS_VARIANT_TYPE (G_VARIANT_TYPE ("(bas)")

gboolean ostree_repo_list_objects (OstreeRepo                  *self,
                                   OstreeRepoListObjectsFlags   flags,
                                   GHashTable                 **out_objects,
                                   GCancellable                *cancellable,
                                   GError                     **error);

GHashTable *ostree_repo_traverse_new_reachable (void);

gboolean ostree_repo_traverse_dirtree (OstreeRepo         *repo,
                                       const char         *commit_checksum,
                                       GHashTable         *inout_reachable,
                                       GCancellable       *cancellable,
                                       GError            **error);

gboolean ostree_repo_traverse_commit (OstreeRepo         *repo,
                                      const char         *commit_checksum,
                                      int                 maxdepth,
                                      GHashTable         *inout_reachable,
                                      GCancellable       *cancellable,
                                      GError            **error);

typedef enum {
  OSTREE_REPO_PRUNE_FLAGS_NONE,
  OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE,
  OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY
} OstreeRepoPruneFlags;

gboolean ostree_repo_prune (OstreeRepo        *repo,
                            OstreeRepoPruneFlags   flags,
                            gint               depth,
                            gint              *out_objects_total,
                            gint              *out_objects_pruned,
                            guint64           *out_pruned_object_size_total,
                            GCancellable      *cancellable,
                            GError           **error);

typedef enum {
  OSTREE_REPO_PULL_FLAGS_NONE,
  OSTREE_REPO_PULL_FLAGS_RELATED
} OstreeRepoPullFlags;

gboolean ostree_repo_pull (OstreeRepo             *repo,
                           const char             *remote_name,
                           char                  **refs_to_fetch,
                           OstreeRepoPullFlags     flags,
                           GCancellable           *cancellable,
                           GError                **error);

G_END_DECLS

