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

typedef struct {
  GObject parent;
} OstreeRepo;

typedef struct {
  GObjectClass parent_class;
} OstreeRepoClass;

GType ostree_repo_get_type (void);

OstreeRepo* ostree_repo_new (const char *path);

gboolean      ostree_repo_check (OstreeRepo  *self, GError **error);

const char *  ostree_repo_get_path (OstreeRepo  *self);

gboolean      ostree_repo_is_archive (OstreeRepo  *self);

GFile *       ostree_repo_get_tmpdir (OstreeRepo  *self);

GKeyFile *    ostree_repo_get_config (OstreeRepo *self);

GKeyFile *    ostree_repo_copy_config (OstreeRepo *self);

gboolean      ostree_repo_write_config (OstreeRepo *self,
                                        GKeyFile   *new_config,
                                        GError    **error);

GFile *       ostree_repo_get_object_path (OstreeRepo   *self,
                                           const char   *object,
                                           OstreeObjectType type);

gboolean      ostree_repo_store_packfile (OstreeRepo       *self,
                                           const char       *expected_checksum,
                                           const char       *path,
                                           OstreeObjectType  objtype,
                                           gboolean         *did_exist,
                                           GError          **error);

gboolean      ostree_repo_store_object_trusted (OstreeRepo   *self,
                                                GFile        *file,
                                                const char   *checksum,
                                                OstreeObjectType objtype,
                                                gboolean      overwrite,
                                                gboolean     *did_exist,
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

gboolean      ostree_repo_load_variant (OstreeRepo *self,
                                          const char   *sha256,
                                          OstreeSerializedVariantType *out_type,
                                          GVariant    **out_variant,
                                          GError      **error);

gboolean      ostree_repo_load_variant_checked (OstreeRepo  *self,
                                                OstreeSerializedVariantType expected_type,
                                                const char    *sha256, 
                                                GVariant     **out_variant,
                                                GError       **error);

gboolean      ostree_repo_commit_directory (OstreeRepo   *self,
                                            const char   *branch,
                                            const char   *parent,
                                            const char   *subject,
                                            const char   *body,
                                            GVariant     *metadata,
                                            GFile        *base,
                                            GChecksum   **out_commit,
                                            GCancellable *cancellable,
                                            GError      **error);

gboolean      ostree_repo_commit_tarfile (OstreeRepo   *self,
                                          const char   *branch,
                                          const char   *parent,
                                          const char   *subject,
                                          const char   *body,
                                          GVariant     *metadata,
                                          GFile        *base,
                                          GChecksum   **out_commit,
                                          GCancellable *cancellable,
                                          GError      **error);

gboolean      ostree_repo_checkout (OstreeRepo *self,
                                    const char   *ref,
                                    const char   *destination,
                                    GCancellable   *cancellable,
                                    GError      **error);

gboolean       ostree_repo_read_commit (OstreeRepo *self,
                                        const char *rev,
                                        GFile       **out_root,
                                        GCancellable *cancellable,
                                        GError  **error);

typedef struct {
  volatile gint refcount;

  GFile *src;
  GFile *target;

  GFileInfo *src_info;
  GFileInfo *target_info;

  char *src_checksum;
  char *target_checksum;
} OstreeRepoDiffItem;

OstreeRepoDiffItem *ostree_repo_diff_item_ref (OstreeRepoDiffItem *diffitem);
void ostree_repo_diff_item_unref (OstreeRepoDiffItem *diffitem);

gboolean      ostree_repo_diff (OstreeRepo     *self,
                                GFile          *src,
                                GFile          *target,
                                GPtrArray     **out_modified, /* OstreeRepoDiffItem */
                                GPtrArray     **out_removed, /* OstreeRepoDiffItem */
                                GPtrArray     **out_added, /* OstreeRepoDiffItem */
                                GCancellable   *cancellable,
                                GError        **error);

typedef void (*OstreeRepoObjectIter) (OstreeRepo *self, 
                                      const char *checksum,
                                      OstreeObjectType type,
                                      GFile      *path,
                                      GFileInfo  *fileinfo,
                                      gpointer user_data);

gboolean     ostree_repo_iter_objects (OstreeRepo  *self,
                                       OstreeRepoObjectIter callback,
                                       gpointer       user_data,
                                       GError        **error);

G_END_DECLS

#endif /* _OSTREE_REPO */
