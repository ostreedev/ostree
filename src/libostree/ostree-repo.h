/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */
/* ostree-repo.h */

#ifndef _OSTREE_REPO
#define _OSTREE_REPO

#include <glib-object.h>

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

GKeyFile *    ostree_repo_get_config (OstreeRepo *self);

GKeyFile *    ostree_repo_copy_config (OstreeRepo *self);

gboolean      ostree_repo_write_config (OstreeRepo *self,
                                        GKeyFile   *new_config,
                                        GError    **error);

gboolean      ostree_repo_link_file (OstreeRepo *self,
                                     const char   *path,
                                     gboolean      ignore_exists,
                                     gboolean      force,
                                     GError      **error);

gboolean      ostree_repo_store_packfile (OstreeRepo       *self,
                                           const char       *expected_checksum,
                                           const char       *path,
                                           OstreeObjectType  objtype,
                                           GError          **error);

gboolean      ostree_repo_store_object_trusted (OstreeRepo   *self,
                                                const char   *path,
                                                const char   *checksum,
                                                OstreeObjectType objtype,
                                                gboolean      ignore_exists,
                                                gboolean      force,
                                                gboolean     *did_exist,
                                                GError      **error);

gboolean      ostree_repo_resolve_rev (OstreeRepo  *self,
                                       const char  *rev,
                                       char       **out_resolved,
                                       GError     **error);

gboolean      ostree_repo_write_ref (OstreeRepo  *self,
                                     gboolean     is_local,
                                     const char  *name,
                                     const char  *rev,
                                     GError     **error);

gboolean      ostree_repo_load_variant (OstreeRepo *self,
                                          const char   *sha256,
                                          OstreeSerializedVariantType *out_type,
                                          GVariant    **out_variant,
                                          GError      **error);

gboolean      ostree_repo_commit (OstreeRepo   *self,
                                  const char   *branch,
                                  const char   *parent,
                                  const char   *subject,
                                  const char   *body,
                                  GVariant     *metadata,
                                  const char   *base,
                                  GPtrArray    *modified_files,
                                  GPtrArray    *removed_files,
                                  GChecksum   **out_commit,
                                  GError      **error);

gboolean      ostree_repo_commit_from_filelist_fd (OstreeRepo   *self,
                                                   const char   *branch,
                                                   const char   *parent,
                                                   const char   *subject,
                                                   const char   *body,
                                                   GVariant     *metadata,
                                                   const char   *base,
                                                   int           fd,
                                                   char          separator,
                                                   GChecksum   **out_commit,
                                                   GError      **error);

gboolean      ostree_repo_checkout (OstreeRepo *self,
                                      const char   *ref,
                                      const char   *destination,
                                      GError      **error);

typedef void (*OstreeRepoObjectIter) (OstreeRepo *self, const char *path,
                                        GFileInfo *fileinfo, gpointer user_data);

gboolean     ostree_repo_iter_objects (OstreeRepo  *self,
                                         OstreeRepoObjectIter callback,
                                         gpointer       user_data,
                                         GError        **error);

G_END_DECLS

#endif /* _OSTREE_REPO */
