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
#include "ostree-async-progress.h"
#include "ostree-sepolicy.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_REPO ostree_repo_get_type()
#define OSTREE_REPO(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSTREE_TYPE_REPO, OstreeRepo))
#define OSTREE_IS_REPO(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSTREE_TYPE_REPO))

gboolean ostree_repo_mode_from_string (const char      *mode,
                                       OstreeRepoMode  *out_mode,
                                       GError         **error);

GType ostree_repo_get_type (void);

OstreeRepo* ostree_repo_new (GFile *path);

OstreeRepo* ostree_repo_new_default (void);

gboolean      ostree_repo_open   (OstreeRepo     *self,
                                  GCancellable   *cancellable,
                                  GError        **error);

void          ostree_repo_set_disable_fsync (OstreeRepo    *self,
                                             gboolean       disable_fsync);

gboolean      ostree_repo_is_system (OstreeRepo   *repo);

gboolean      ostree_repo_create (OstreeRepo     *self,
                                  OstreeRepoMode  mode,
                                  GCancellable   *cancellable,
                                  GError        **error);

GFile *       ostree_repo_get_path (OstreeRepo  *self);

OstreeRepoMode ostree_repo_get_mode (OstreeRepo  *self);

GKeyFile *    ostree_repo_get_config (OstreeRepo *self);

GKeyFile *    ostree_repo_copy_config (OstreeRepo *self);

gboolean      ostree_repo_remote_add (OstreeRepo     *self,
                                      const char     *name,
                                      const char     *url,
                                      GVariant       *options,
                                      GCancellable   *cancellable,
                                      GError        **error);

gboolean      ostree_repo_remote_delete (OstreeRepo     *self,
                                         const char     *name,
                                         GCancellable   *cancellable,
                                         GError        **error);

OstreeRepo * ostree_repo_get_parent (OstreeRepo  *self);

gboolean      ostree_repo_write_config (OstreeRepo *self,
                                        GKeyFile   *new_config,
                                        GError    **error);

/**
 * OstreeRepoTransactionStats:
 * @metadata_objects_total: The total number of metadata objects
 * in the repository after this transaction has completed.
 * @metadata_objects_written: The number of metadata objects that
 * were written to the repository in this transaction.
 * @content_objects_total: The total number of content objects
 * in the repository after this transaction has completed.
 * @content_objects_written: The number of content objects that
 * were written to the repository in this transaction.
 * @content_bytes_total: The amount of data added to the repository,
 * in bytes, counting only content objects.
 *
 * A list of statistics for each transaction that may be
 * interesting for reporting purposes.
 */
typedef struct _OstreeRepoTransactionStats OstreeRepoTransactionStats;

struct _OstreeRepoTransactionStats {
  guint metadata_objects_total;
  guint metadata_objects_written;
  guint content_objects_total;
  guint content_objects_written;
  guint64 content_bytes_written;

  guint64 padding1;
  guint64 padding2;
  guint64 padding3;
  guint64 padding4;
};

GType ostree_repo_transaction_stats_get_type (void);

gboolean      ostree_repo_scan_hardlinks      (OstreeRepo     *self,
                                               GCancellable   *cancellable,
                                               GError        **error);

gboolean      ostree_repo_prepare_transaction (OstreeRepo     *self,
                                               gboolean       *out_transaction_resume,
                                               GCancellable   *cancellable,
                                               GError        **error);

gboolean      ostree_repo_commit_transaction (OstreeRepo                  *self,
                                              OstreeRepoTransactionStats  *out_stats,
                                              GCancellable                *cancellable,
                                              GError                     **error);

gboolean      ostree_repo_abort_transaction (OstreeRepo     *self,
                                             GCancellable   *cancellable,
                                             GError        **error);

void          ostree_repo_transaction_set_refspec (OstreeRepo *self,
                                                   const char *refspec,
                                                   const char *checksum);

void          ostree_repo_transaction_set_ref     (OstreeRepo *self,
                                                   const char *remote,
                                                   const char *ref,
                                                   const char *checksum);

gboolean      ostree_repo_set_ref_immediate (OstreeRepo *self,
                                             const char *remote,
                                             const char *ref,
                                             const char *checksum,
                                             GCancellable  *cancellable,
                                             GError       **error);

gboolean      ostree_repo_has_object (OstreeRepo           *self,
                                      OstreeObjectType      objtype,
                                      const char           *checksum,
                                      gboolean             *out_have_object,
                                      GCancellable         *cancellable,
                                      GError              **error);

gboolean      ostree_repo_write_metadata (OstreeRepo        *self,
                                          OstreeObjectType   objtype,
                                          const char        *expected_checksum,
                                          GVariant          *object,
                                          guchar           **out_csum,
                                          GCancellable      *cancellable,
                                          GError           **error);

void          ostree_repo_write_metadata_async (OstreeRepo              *self,
                                                OstreeObjectType         objtype,
                                                const char              *expected_checksum,
                                                GVariant                *object,
                                                GCancellable            *cancellable,
                                                GAsyncReadyCallback      callback,
                                                gpointer                 user_data);

gboolean      ostree_repo_write_metadata_finish (OstreeRepo        *self,
                                                 GAsyncResult      *result,
                                                 guchar           **out_csum,
                                                 GError           **error);

gboolean      ostree_repo_write_content (OstreeRepo       *self,
                                         const char       *expected_checksum,
                                         GInputStream     *object_input,
                                         guint64           length,
                                         guchar          **out_csum,
                                         GCancellable     *cancellable,
                                         GError          **error);

gboolean      ostree_repo_write_metadata_trusted (OstreeRepo        *self,
                                                  OstreeObjectType   objtype,
                                                  const char        *checksum,
                                                  GVariant          *variant,
                                                  GCancellable      *cancellable,
                                                  GError           **error);

gboolean      ostree_repo_write_metadata_stream_trusted (OstreeRepo        *self,
                                                         OstreeObjectType   objtype,
                                                         const char        *checksum,
                                                         GInputStream      *object_input,
                                                         guint64            length,
                                                         GCancellable      *cancellable,
                                                         GError           **error);

gboolean      ostree_repo_write_content_trusted (OstreeRepo       *self,
                                                 const char       *checksum,
                                                 GInputStream     *object_input,
                                                 guint64           length,
                                                 GCancellable     *cancellable,
                                                 GError          **error);

void          ostree_repo_write_content_async (OstreeRepo              *self,
                                               const char              *expected_checksum,
                                               GInputStream            *object,
                                               guint64                  length,
                                               GCancellable            *cancellable,
                                               GAsyncReadyCallback      callback,
                                               gpointer                 user_data);

gboolean      ostree_repo_write_content_finish (OstreeRepo        *self,
                                                GAsyncResult      *result,
                                                guchar           **out_csum,
                                                GError           **error);

gboolean      ostree_repo_resolve_rev (OstreeRepo  *self,
                                       const char  *refspec,
                                       gboolean     allow_noent,
                                       char       **out_rev,
                                       GError     **error);

gboolean      ostree_repo_list_refs (OstreeRepo       *self,
                                     const char       *refspec_prefix,
                                     GHashTable      **out_all_refs,
                                     GCancellable     *cancellable,
                                     GError          **error);

gboolean      ostree_repo_load_variant (OstreeRepo  *self,
                                        OstreeObjectType objtype,
                                        const char    *sha256, 
                                        GVariant     **out_variant,
                                        GError       **error);

gboolean      ostree_repo_load_variant_if_exists (OstreeRepo  *self,
                                                  OstreeObjectType objtype,
                                                  const char    *sha256, 
                                                  GVariant     **out_variant,
                                                  GError       **error);

gboolean ostree_repo_load_file (OstreeRepo         *self,
                                const char         *checksum,
                                GInputStream      **out_input,
                                GFileInfo         **out_file_info,
                                GVariant          **out_xattrs,
                                GCancellable       *cancellable,
                                GError            **error);

gboolean ostree_repo_load_object_stream (OstreeRepo         *self,
                                         OstreeObjectType    objtype,
                                         const char         *checksum,
                                         GInputStream      **out_input,
                                         guint64            *out_size,
                                         GCancellable       *cancellable,
                                         GError            **error);

gboolean      ostree_repo_query_object_storage_size (OstreeRepo           *self,
                                                     OstreeObjectType      objtype,
                                                     const char           *sha256, 
                                                     guint64              *out_size,
                                                     GCancellable         *cancellable,
                                                     GError              **error);

gboolean      ostree_repo_import_object_from (OstreeRepo           *self,
                                              OstreeRepo           *source,
                                              OstreeObjectType      objtype,
                                              const char           *checksum,
                                              GCancellable         *cancellable,
                                              GError              **error);

gboolean      ostree_repo_delete_object (OstreeRepo           *self,
                                         OstreeObjectType      objtype,
                                         const char           *sha256, 
                                         GCancellable         *cancellable,
                                         GError              **error);

/** 
 * OstreeRepoCommitFilterResult:
 * @OSTREE_REPO_COMMIT_FILTER_ALLOW: Do commit this object
 * @OSTREE_REPO_COMMIT_FILTER_SKIP: Ignore this object
 */
typedef enum {
  OSTREE_REPO_COMMIT_FILTER_ALLOW,
  OSTREE_REPO_COMMIT_FILTER_SKIP
} OstreeRepoCommitFilterResult;

/**
 * OstreeRepoCommitFilter:
 * @repo: Repo
 * @path: Path to file
 * @file_info: File information
 * @user_data: User data
 *
 * Returns: #OstreeRepoCommitFilterResult saying whether or not to commit this file
 */
typedef OstreeRepoCommitFilterResult (*OstreeRepoCommitFilter) (OstreeRepo    *repo,
                                                                const char    *path,
                                                                GFileInfo     *file_info,
                                                                gpointer       user_data);

/**
 * OstreeRepoCommitModifierFlags:
 * @OSTREE_REPO_COMMIT_MODIFIER_FLAGS_NONE: No special flags
 * @OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS: Do not process extended attributes
 */
typedef enum {
  OSTREE_REPO_COMMIT_MODIFIER_FLAGS_NONE = 0,
  OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS = (1 << 0),
  OSTREE_REPO_COMMIT_MODIFIER_FLAGS_GENERATE_SIZES = (1 << 1)
} OstreeRepoCommitModifierFlags;

/**
 * OstreeRepoCommitModifier:
 *
 * A structure allowing control over commits.
 */
typedef struct OstreeRepoCommitModifier OstreeRepoCommitModifier;

OstreeRepoCommitModifier *ostree_repo_commit_modifier_new (OstreeRepoCommitModifierFlags  flags,
                                                           OstreeRepoCommitFilter         commit_filter,
                                                           gpointer                       user_data,
                                                           GDestroyNotify                 destroy_notify);

GType ostree_repo_commit_modifier_get_type (void);

typedef GVariant *(*OstreeRepoCommitModifierXattrCallback) (OstreeRepo     *repo,
                                                            const char     *path,
                                                            GFileInfo      *file_info,
                                                            gpointer        user_data);

void ostree_repo_commit_modifier_set_xattr_callback (OstreeRepoCommitModifier              *modifier,
                                                     OstreeRepoCommitModifierXattrCallback  callback,
                                                     GDestroyNotify                         destroy,
                                                     gpointer                               user_data);

void ostree_repo_commit_modifier_set_sepolicy (OstreeRepoCommitModifier              *modifier,
                                               OstreeSePolicy                        *sepolicy);

OstreeRepoCommitModifier *ostree_repo_commit_modifier_ref (OstreeRepoCommitModifier *modifier);
void ostree_repo_commit_modifier_unref (OstreeRepoCommitModifier *modifier);

gboolean      ostree_repo_write_directory_to_mtree (OstreeRepo                 *self,
                                                    GFile                      *dir,
                                                    OstreeMutableTree          *mtree,
                                                    OstreeRepoCommitModifier   *modifier,
                                                    GCancellable               *cancellable,
                                                    GError                    **error);

gboolean      ostree_repo_write_archive_to_mtree (OstreeRepo                   *self,
                                                  GFile                        *archive,
                                                  OstreeMutableTree            *mtree,
                                                  OstreeRepoCommitModifier     *modifier,
                                                  gboolean                      autocreate_parents,
                                                  GCancellable                 *cancellable,
                                                  GError                      **error);

gboolean      ostree_repo_write_mtree (OstreeRepo         *self,
                                       OstreeMutableTree  *mtree,
                                       GFile             **out_file,
                                       GCancellable       *cancellable,
                                       GError            **error);

gboolean      ostree_repo_write_commit (OstreeRepo      *self,
                                        const char      *parent,
                                        const char      *subject,
                                        const char      *body,
                                        GVariant        *metadata,
                                        OstreeRepoFile  *root,
                                        char           **out_commit,
                                        GCancellable    *cancellable,
                                        GError         **error);

gboolean      ostree_repo_read_commit_detached_metadata (OstreeRepo      *self,
                                                         const char      *checksum,
                                                         GVariant       **out_metadata,
                                                         GCancellable    *cancellable,
                                                         GError         **error);

gboolean      ostree_repo_write_commit_detached_metadata (OstreeRepo      *self,
                                                          const char      *checksum,
                                                          GVariant        *metadata,
                                                          GCancellable    *cancellable,
                                                          GError         **error);

/**
 * OstreeRepoCheckoutMode:
 * @OSTREE_REPO_CHECKOUT_MODE_NONE: No special options
 * @OSTREE_REPO_CHECKOUT_MODE_USER: Ignore uid/gid of files
 */
typedef enum {
  OSTREE_REPO_CHECKOUT_MODE_NONE = 0,
  OSTREE_REPO_CHECKOUT_MODE_USER = 1
} OstreeRepoCheckoutMode;

/**
 * OstreeRepoCheckoutOverwriteMode:
 * @OSTREE_REPO_CHECKOUT_OVERWRITE_NONE: No special options
 * @OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES: When layering checkouts, overwrite earlier files, but keep earlier directories
 */
typedef enum {
  OSTREE_REPO_CHECKOUT_OVERWRITE_NONE = 0,
  OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES = 1
} OstreeRepoCheckoutOverwriteMode;

gboolean
ostree_repo_checkout_tree (OstreeRepo               *self,
                           OstreeRepoCheckoutMode    mode,
                           OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                           GFile                    *destination,
                           OstreeRepoFile           *source,
                           GFileInfo                *source_info,
                           GCancellable             *cancellable,
                           GError                  **error);

gboolean       ostree_repo_checkout_gc (OstreeRepo        *self,
                                        GCancellable      *cancellable,
                                        GError           **error);

gboolean       ostree_repo_read_commit (OstreeRepo    *self,
                                        const char    *ref,
                                        GFile        **out_root,
                                        char         **out_commit,
                                        GCancellable  *cancellable,
                                        GError        **error);

/**
 * OstreeRepoListObjectsFlags:
 * @OSTREE_REPO_LIST_OBJECTS_LOOSE: List only loose (plain file) objects
 * @OSTREE_REPO_LIST_OBJECTS_PACKED: List only packed (compacted into blobs) objects
 * @OSTREE_REPO_LIST_OBJECTS_ALL: List all objects
 */
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

gboolean ostree_repo_list_commit_objects_starting_with ( OstreeRepo                  *self,
                                                         const char                  *start,
                                                         GHashTable                 **out_commits,
                                                         GCancellable                *cancellable,
                                                         GError                     **error);

gboolean ostree_repo_list_static_delta_names (OstreeRepo                  *self,
                                              GPtrArray                  **out_deltas,
                                              GCancellable                *cancellable,
                                              GError                     **error);

/**
 * OstreeStaticDeltaGenerateOpt:
 * @OSTREE_STATIC_DELTA_GENERATE_OPT_LOWLATENCY: Optimize for speed of delta creation over space
 * @OSTREE_STATIC_DELTA_GENERATE_OPT_MAJOR: Optimize for delta size (may be very slow)
 *
 * Parameters controlling optimization of static deltas.
 */
typedef enum {
  OSTREE_STATIC_DELTA_GENERATE_OPT_LOWLATENCY,
  OSTREE_STATIC_DELTA_GENERATE_OPT_MAJOR
} OstreeStaticDeltaGenerateOpt;

gboolean ostree_repo_static_delta_generate (OstreeRepo                   *self,
                                            OstreeStaticDeltaGenerateOpt  opt,
                                            const char                   *from,
                                            const char                   *to,
                                            GVariant                     *metadata,
                                            GCancellable                 *cancellable,
                                            GError                      **error);

gboolean ostree_repo_static_delta_execute_offline (OstreeRepo                    *self,
                                                   GFile                         *dir,
                                                   gboolean                       skip_validation,
                                                   GCancellable                  *cancellable,
                                                   GError                      **error);

GHashTable *ostree_repo_traverse_new_reachable (void);

gboolean ostree_repo_traverse_commit (OstreeRepo         *repo,
                                      const char         *commit_checksum,
                                      int                 maxdepth,
                                      GHashTable        **out_reachable,
                                      GCancellable       *cancellable,
                                      GError            **error);

gboolean ostree_repo_traverse_commit_union (OstreeRepo         *repo,
                                            const char         *commit_checksum,
                                            int                 maxdepth,
                                            GHashTable         *inout_reachable,
                                            GCancellable       *cancellable,
                                            GError            **error);

/**
 * OstreeRepoPruneFlags:
 * @OSTREE_REPO_PRUNE_FLAGS_NONE: No special options for pruning
 * @OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE: Don't actually delete objects
 * @OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY: Do not traverse individual commit objects, only follow refs
 */
typedef enum {
  OSTREE_REPO_PRUNE_FLAGS_NONE,
  OSTREE_REPO_PRUNE_FLAGS_NO_PRUNE,
  OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY
} OstreeRepoPruneFlags;

gboolean ostree_repo_prune (OstreeRepo        *self,
                            OstreeRepoPruneFlags   flags,
                            gint               depth,
                            gint              *out_objects_total,
                            gint              *out_objects_pruned,
                            guint64           *out_pruned_object_size_total,
                            GCancellable      *cancellable,
                            GError           **error);

/**
 * OstreeRepoPullFlags:
 * @OSTREE_REPO_PULL_FLAGS_NONE: No special options for pull
 * @OSTREE_REPO_PULL_FLAGS_MIRROR: Write out refs suitable for mirrors
 */
typedef enum {
  OSTREE_REPO_PULL_FLAGS_NONE,
  OSTREE_REPO_PULL_FLAGS_MIRROR
} OstreeRepoPullFlags;

gboolean ostree_repo_pull (OstreeRepo             *self,
                           const char             *remote_name,
                           char                  **refs_to_fetch,
                           OstreeRepoPullFlags     flags,
                           OstreeAsyncProgress    *progress,
                           GCancellable           *cancellable,
                           GError                **error);

gboolean
ostree_repo_pull_one_dir (OstreeRepo               *self,
                          const char               *remote_name,
                          const char               *dir_to_pull,
                          char                    **refs_to_fetch,
                          OstreeRepoPullFlags       flags,
                          OstreeAsyncProgress      *progress,
                          GCancellable             *cancellable,
                          GError                  **error);

gboolean ostree_repo_pull_with_options (OstreeRepo             *self,
                                        const char             *remote_name,
                                        GVariant               *options,
                                        OstreeAsyncProgress    *progress,
                                        GCancellable           *cancellable,
                                        GError                **error);

gboolean ostree_repo_sign_commit (OstreeRepo     *self,
                                  const gchar    *commit_checksum,
                                  const gchar    *key_id,
                                  const gchar    *homedir,
                                  GCancellable   *cancellable,
                                  GError        **error);

gboolean ostree_repo_append_gpg_signature (OstreeRepo     *self,
                                           const gchar    *commit_checksum,
                                           GBytes         *signature_bytes,
                                           GCancellable   *cancellable,
                                           GError        **error);

gboolean ostree_repo_verify_commit (OstreeRepo   *self,
                                    const gchar  *commit_checksum,
                                    GFile        *keyringdir,
                                    GFile        *extra_keyring,
                                    GCancellable *cancellable,
                                    GError      **error);

gboolean ostree_repo_regenerate_summary (OstreeRepo     *self,
                                         GVariant       *additional_metadata,
                                         GCancellable   *cancellable,
                                         GError        **error);


G_END_DECLS

