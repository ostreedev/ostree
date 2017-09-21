/*
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

#include "ostree-types.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_MUTABLE_TREE         (ostree_mutable_tree_get_type ())
#define OSTREE_MUTABLE_TREE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), OSTREE_TYPE_MUTABLE_TREE, OstreeMutableTree))
#define OSTREE_MUTABLE_TREE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), OSTREE_TYPE_MUTABLE_TREE, OstreeMutableTreeClass))
#define OSTREE_IS_MUTABLE_TREE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), OSTREE_TYPE_MUTABLE_TREE))
#define OSTREE_IS_MUTABLE_TREE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), OSTREE_TYPE_MUTABLE_TREE))
#define OSTREE_MUTABLE_TREE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), OSTREE_TYPE_MUTABLE_TREE, OstreeMutableTreeClass))

typedef struct OstreeMutableTreeClass   OstreeMutableTreeClass;

typedef struct {
  gboolean        in_files;
  GHashTableIter  iter;
} OstreeMutableTreeIter;

struct OstreeMutableTreeClass
{
  GObjectClass parent_class;
};

_OSTREE_PUBLIC
GType   ostree_mutable_tree_get_type (void) G_GNUC_CONST;

_OSTREE_PUBLIC
OstreeMutableTree *ostree_mutable_tree_new (void);

_OSTREE_PUBLIC
void ostree_mutable_tree_set_metadata_checksum (OstreeMutableTree *self,
                                                const char        *checksum);

_OSTREE_PUBLIC
const char *ostree_mutable_tree_get_metadata_checksum (OstreeMutableTree *self);

_OSTREE_PUBLIC
void ostree_mutable_tree_set_contents_checksum (OstreeMutableTree *self,
                                                const char        *checksum);

_OSTREE_PUBLIC
const char *ostree_mutable_tree_get_contents_checksum (OstreeMutableTree *self);

_OSTREE_PUBLIC
gboolean ostree_mutable_tree_replace_file (OstreeMutableTree *self,
                                           const char        *name,
                                           const char        *checksum,
                                           GError           **error);

_OSTREE_PUBLIC
gboolean ostree_mutable_tree_ensure_dir (OstreeMutableTree *self,
                                         const char        *name,
                                         OstreeMutableTree **out_subdir,
                                         GError           **error);

_OSTREE_PUBLIC
gboolean ostree_mutable_tree_lookup (OstreeMutableTree   *self,
                                     const char          *name,
                                     char               **out_file_checksum,
                                     OstreeMutableTree  **out_subdir,
                                     GError             **error);

_OSTREE_PUBLIC
gboolean
ostree_mutable_tree_ensure_parent_dirs (OstreeMutableTree  *self,
                                        GPtrArray          *split_path,
                                        const char         *metadata_checksum,
                                        OstreeMutableTree **out_parent,
                                        GError            **error);

_OSTREE_PUBLIC
gboolean ostree_mutable_tree_walk (OstreeMutableTree   *self,
                                   GPtrArray           *split_path,
                                   guint                start,
                                   OstreeMutableTree  **out_subdir,
                                   GError             **error);

_OSTREE_PUBLIC
GHashTable * ostree_mutable_tree_get_subdirs (OstreeMutableTree *self);
_OSTREE_PUBLIC
GHashTable * ostree_mutable_tree_get_files (OstreeMutableTree *self);

G_END_DECLS
