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
/* hacktree-repo.h */

#ifndef _HACKTREE_REPO
#define _HACKTREE_REPO

#include <glib-object.h>

G_BEGIN_DECLS

#define HACKTREE_TYPE_REPO hacktree_repo_get_type()
#define HACKTREE_REPO(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), HACKTREE_TYPE_REPO, HacktreeRepo))
#define HACKTREE_REPO_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), HACKTREE_TYPE_REPO, HacktreeRepoClass))
#define HACKTREE_IS_REPO(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), HACKTREE_TYPE_REPO))
#define HACKTREE_IS_REPO_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), HACKTREE_TYPE_REPO))
#define HACKTREE_REPO_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), HACKTREE_TYPE_REPO, HacktreeRepoClass))

typedef struct {
  GObject parent;
} HacktreeRepo;

typedef struct {
  GObjectClass parent_class;
} HacktreeRepoClass;

GType hacktree_repo_get_type (void);

HacktreeRepo* hacktree_repo_new (const char *path);

gboolean      hacktree_repo_check (HacktreeRepo  *repo, GError **error);

gboolean      hacktree_repo_link_file (HacktreeRepo *repo,
                                       const char   *path,
                                       gboolean      ignore_exists,
                                       gboolean      force,
                                       GError      **error);

gboolean      hacktree_repo_commit (HacktreeRepo *repo,
                                    const char   *subject,
                                    const char   *body,
                                    GVariant     *metadata,
                                    const char   *base,
                                    GPtrArray    *modified_files,
                                    GPtrArray    *removed_files,
                                    GChecksum   **out_commit,
                                    GError      **error);

typedef void (*HacktreeRepoObjectIter) (HacktreeRepo *repo, const char *path,
                                        GFileInfo *fileinfo, gpointer user_data);

gboolean     hacktree_repo_iter_objects (HacktreeRepo  *repo,
                                         HacktreeRepoObjectIter callback,
                                         gpointer       user_data,
                                         GError        **error);

G_END_DECLS

#endif /* _HACKTREE_REPO */
