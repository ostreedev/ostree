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

#ifndef _OSTREE_REPO_FILE
#define _OSTREE_REPO_FILE

#include "ostree-types.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_REPO_FILE         (ostree_repo_file_get_type ())
#define OSTREE_REPO_FILE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), OSTREE_TYPE_REPO_FILE, OstreeRepoFile))
#define OSTREE_REPO_FILE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), OSTREE_TYPE_REPO_FILE, OstreeRepoFileClass))
#define OSTREE_IS_REPO_FILE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), OSTREE_TYPE_REPO_FILE))
#define OSTREE_IS_REPO_FILE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), OSTREE_TYPE_REPO_FILE))
#define OSTREE_REPO_FILE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), OSTREE_TYPE_REPO_FILE, OstreeRepoFileClass))

typedef struct _OstreeRepoFileClass   OstreeRepoFileClass;

struct _OstreeRepoFileClass
{
  GObjectClass parent_class;
};

GType   ostree_repo_file_get_type (void) G_GNUC_CONST;

GFile * ostree_repo_file_new_root (OstreeRepo  *repo,
                                    const char  *commit);

GFile * ostree_repo_file_new_child (OstreeRepoFile *parent,
                                     const char  *name);

gboolean ostree_repo_file_ensure_resolved (OstreeRepoFile  *self,
                                            GError         **error);

gboolean ostree_repo_file_get_xattrs (OstreeRepoFile  *self,
                                      GVariant       **out_xattrs,
                                      GCancellable    *cancellable,
                                      GError         **error);

OstreeRepo * ostree_repo_file_get_repo (OstreeRepoFile  *self);
OstreeRepoFile * ostree_repo_file_get_root (OstreeRepoFile  *self);

void ostree_repo_file_make_empty_tree (OstreeRepoFile  *self);

void ostree_repo_file_tree_set_metadata (OstreeRepoFile *self,
                                          const char     *checksum,
                                          GVariant       *metadata);

void ostree_repo_file_tree_set_content_checksum (OstreeRepoFile  *self,
                                                  const char      *checksum);
const char *ostree_repo_file_tree_get_content_checksum (OstreeRepoFile  *self);

gboolean ostree_repo_file_is_tree (OstreeRepoFile  *self);

const char * ostree_repo_file_get_checksum (OstreeRepoFile  *self);

const char * ostree_repo_file_get_commit (OstreeRepoFile  *self);

GFile *ostree_repo_file_nontree_get_local (OstreeRepoFile  *self);

int     ostree_repo_file_tree_find_child  (OstreeRepoFile  *self,
                                            const char      *name,
                                            gboolean        *is_dir,
                                            GVariant       **out_container);

gboolean ostree_repo_file_tree_query_child (OstreeRepoFile  *self,
                                             int              n,
                                             const char      *attributes,
                                             GFileQueryInfoFlags flags,
                                             GFileInfo      **out_info,
                                             GCancellable    *cancellable,
                                             GError         **error);

GVariant *ostree_repo_file_tree_get_contents (OstreeRepoFile  *self);
GVariant *ostree_repo_file_tree_get_metadata (OstreeRepoFile  *self);

G_END_DECLS

#endif
