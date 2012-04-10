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

#ifndef _OSTREE_REPO
#define _OSTREE_REPO

#include "ostree-core.h"
#include "ostree-types.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_REPO ostree_repo_get_type()
#define OSTREE_REPO(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSTREE_TYPE_REPO, OstreeRepo))
#define OSTREE_REPO_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), OSTREE_TYPE_REPO, OstreeRepoClass))
#define OSTREE_IS_REPO(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSTREE_TYPE_REPO))
#define OSTREE_IS_REPO_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), OSTREE_TYPE_REPO))
#define OSTREE_REPO_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), OSTREE_TYPE_REPO, OstreeRepoClass))

struct OstreeRepo {
  GObject parent;
};

typedef struct {
  GObjectClass parent_class;
} OstreeRepoClass;

GType ostree_repo_get_type (void);

OstreeRepo* ostree_repo_new (GFile *path);

gboolean      ostree_repo_check (OstreeRepo  *self, GError **error);

GFile *       ostree_repo_get_path (OstreeRepo  *self);

typedef enum {
  OSTREE_REPO_MODE_BARE,
  OSTREE_REPO_MODE_ARCHIVE
} OstreeRepoMode;

OstreeRepoMode ostree_repo_get_mode (OstreeRepo  *self);

GFile *       ostree_repo_get_tmpdir (OstreeRepo  *self);

GKeyFile *    ostree_repo_get_config (OstreeRepo *self);

GKeyFile *    ostree_repo_copy_config (OstreeRepo *self);

gboolean      ostree_repo_write_config (OstreeRepo *self,
                                        GKeyFile   *new_config,
                                        GError    **error);

GFile *       ostree_repo_get_object_path (OstreeRepo   *self,
                                           const char   *object,
                                           OstreeObjectType type);

GFile *       ostree_repo_get_file_object_path (OstreeRepo   *self,
                                                const char   *object);

gboolean      ostree_repo_prepare_transaction (OstreeRepo     *self,
                                               GCancellable   *cancellable,
                                               GError        **error);

gboolean      ostree_repo_commit_transaction (OstreeRepo     *self,
                                              GCancellable   *cancellable,
                                              GError        **error);

gboolean      ostree_repo_abort_transaction (OstreeRepo     *self,
                                             GCancellable   *cancellable,
                                             GError        **error);

gboolean      ostree_repo_find_object (OstreeRepo           *self,
                                       OstreeObjectType      objtype,
                                       const char           *checksum,
                                       GFile               **out_stored_path,
                                       char                **out_pack_checksum,
                                       guint64              *out_pack_offset,
                                       GCancellable         *cancellable,
                                       GError              **error);

gboolean      ostree_repo_stage_object (OstreeRepo       *self,
                                        OstreeObjectType  objtype,
                                        const char       *expected_checksum,
                                        GFileInfo        *file_info,
                                        GVariant         *xattrs,
                                        GInputStream     *content,
                                        GCancellable     *cancellable,
                                        GError          **error);

gboolean      ostree_repo_stage_object_trusted (OstreeRepo   *self,
                                                OstreeObjectType objtype,
                                                const char   *checksum,
                                                gboolean          store_if_packed,
                                                GFileInfo        *file_info,
                                                GVariant         *xattrs,
                                                GInputStream     *content,
                                                GCancellable *cancellable,
                                                GError      **error);

gboolean      ostree_repo_resolve_rev (OstreeRepo  *self,
                                       const char  *rev,
                                       gboolean     allow_noent,
                                       char       **out_resolved,
                                       GError     **error);

gboolean      ostree_repo_write_ref (OstreeRepo  *self,
                                     const char  *remote,
                                     const char  *name,
                                     const char  *rev,
                                     GError     **error);

gboolean      ostree_repo_list_all_refs (OstreeRepo       *repo,
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

gboolean      ostree_repo_load_pack_index (OstreeRepo    *self,
                                           const char    *pack_checksum, 
                                           gboolean       is_meta,
                                           GVariant     **out_variant,
                                           GCancellable  *cancellable,
                                           GError       **error);

gboolean      ostree_repo_load_pack_data  (OstreeRepo    *self,
                                           const char    *pack_checksum,
                                           guchar       **out_data,
                                           GCancellable  *cancellable,
                                           GError       **error);

gboolean ostree_repo_map_pack_file (OstreeRepo    *self,
                                    const char    *sha256,
                                    gboolean       is_meta,
                                    guchar       **out_data,
                                    guint64       *out_len,
                                    GCancellable  *cancellable,
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
                                        GVariant     *metadata,
                                        const char   *content_checksum,
                                        const char   *metadata_checksum,
                                        char        **out_commit,
                                        GCancellable *cancellable,
                                        GError      **error);

gboolean ostree_repo_regenerate_pack_index (OstreeRepo       *self,
                                            GCancellable     *cancellable,
                                            GError          **error);

gboolean     ostree_repo_add_pack_file (OstreeRepo       *self,
                                        const char       *checksum,
                                        gboolean          is_meta,
                                        GFile            *pack_index_path,
                                        GFile            *pack_data_path,
                                        GCancellable     *cancellable,
                                        GError          **error);

gboolean     ostree_repo_resync_cached_remote_pack_indexes (OstreeRepo       *self,
                                                            const char       *remote_name,
                                                            GFile            *superindex_path,
                                                            GPtrArray       **out_cached_meta_indexes,
                                                            GPtrArray       **out_cached_data_indexes,
                                                            GPtrArray       **out_uncached_meta_indexes,
                                                            GPtrArray       **out_uncached_data_indexes,
                                                            GCancellable     *cancellable,
                                                            GError          **error);

gboolean     ostree_repo_clean_cached_remote_pack_data (OstreeRepo       *self,
                                                        const char       *remote_name,
                                                        GCancellable     *cancellable,
                                                        GError          **error);

gboolean     ostree_repo_map_cached_remote_pack_index (OstreeRepo       *self,
                                                       const char       *remote_name,
                                                       const char       *pack_checksum,
                                                       gboolean          is_meta,
                                                       GVariant        **out_variant,
                                                       GCancellable     *cancellable,
                                                       GError          **error);

gboolean     ostree_repo_add_cached_remote_pack_index (OstreeRepo       *self,
                                                       const char       *remote_name,
                                                       const char       *pack_checksum,
                                                       gboolean          is_meta,
                                                       GFile            *cached_path,
                                                       GCancellable     *cancellable,
                                                       GError          **error);

gboolean     ostree_repo_get_cached_remote_pack_data (OstreeRepo       *self,
                                                      const char       *remote_name,
                                                      const char       *pack_checksum,
                                                      gboolean          is_meta,
                                                      GFile           **out_cached_path,
                                                      GCancellable     *cancellable,
                                                      GError          **error);

gboolean     ostree_repo_take_cached_remote_pack_data (OstreeRepo       *self,
                                                       const char       *remote_name,
                                                       const char       *pack_checksum,
                                                       gboolean          is_meta,
                                                       GFile            *cached_path,
                                                       GCancellable     *cancellable,
                                                       GError          **error);

typedef enum {
  OSTREE_REPO_CHECKOUT_MODE_NONE = 0,
  OSTREE_REPO_CHECKOUT_MODE_USER = 1
} OstreeRepoCheckoutMode;

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

gboolean ostree_repo_list_pack_indexes (OstreeRepo              *self,
                                        GPtrArray              **out_meta_indexes,
                                        GPtrArray              **out_data_indexes,
                                        GCancellable            *cancellable,
                                        GError                 **error);

G_END_DECLS

#endif /* _OSTREE_REPO */
