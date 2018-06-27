/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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

#include "config.h"

#include "otutil.h"
#include "ostree.h"

/**
 * SECTION:ostree-mutable-tree
 * @title: In-memory modifiable filesystem tree
 * @short_description: Modifiable filesystem tree
 *
 * In order to commit content into an #OstreeRepo, it must first be
 * imported into an #OstreeMutableTree.  There are several high level
 * APIs to create an initiable #OstreeMutableTree from a physical
 * filesystem directory, but they may also be computed
 * programmatically.
 */

/**
 * OstreeMutableTree:
 *
 * Private instance structure.
 */
struct OstreeMutableTree
{
  GObject parent_instance;

  /* The parent directory to this one.  We don't hold a ref because this mtree
   * is owned by the parent.  We can be certain that any mtree only has one
   * parent because external users can't set this, it's only set when we create
   * a child from within this file (see insert_child_mtree). We ensure that the
   * parent pointer is either valid or NULL because when the parent is destroyed
   * it sets parent = NULL on all its children (see remove_child_mtree) */
  OstreeMutableTree *parent;

  /* This is the checksum of the Dirtree object that corresponds to the current
   * contents of this directory.  contents_checksum can be NULL if the SHA was
   * never calculated or contents of this mtree or any subdirectory has been
   * modified.  If a contents_checksum is NULL then all the parent's checksums
   * will be NULL (see `invalidate_contents_checksum`).
   *
   * Note: This invariant is partially maintained externally - we
   * rely on the callers of `ostree_mutable_tree_set_contents_checksum` to have
   * first ensured that the mtree contents really does correspond to this
   * checksum */
  char *contents_checksum;

  /* This is the checksum of the DirMeta object that holds the uid, gid, mode
   * and xattrs of this directory.  This can be NULL. */
  char *metadata_checksum;

  /* const char* filename -> const char* checksum */
  GHashTable *files;

  /* const char* filename -> OstreeMutableTree* subtree */
  GHashTable *subdirs;
};

G_DEFINE_TYPE (OstreeMutableTree, ostree_mutable_tree, G_TYPE_OBJECT)

static void invalidate_contents_checksum (OstreeMutableTree *self);

static void
ostree_mutable_tree_finalize (GObject *object)
{
  OstreeMutableTree *self;

  self = OSTREE_MUTABLE_TREE (object);

  g_free (self->contents_checksum);
  g_free (self->metadata_checksum);

  g_hash_table_destroy (self->files);
  g_hash_table_destroy (self->subdirs);

  G_OBJECT_CLASS (ostree_mutable_tree_parent_class)->finalize (object);
}

static void
ostree_mutable_tree_class_init (OstreeMutableTreeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = ostree_mutable_tree_finalize;
}

/* This must not be made public or we can't maintain the invariant that any
 * OstreeMutableTree has only one parent.
 *
 * Ownership of @child is transferred from the caller to @self */
static void
insert_child_mtree (OstreeMutableTree *self, const gchar* name,
                    OstreeMutableTree *child)
{
  g_assert_null (child->parent);
  g_hash_table_insert (self->subdirs, g_strdup (name), child);
  child->parent = self;
  invalidate_contents_checksum (self);
}

static void
remove_child_mtree (gpointer data)
{
  /* Each mtree has shared ownership of its children and each child has a
   * non-owning reference back to parent.  If the parent goes out of scope the
   * children may still be alive because they're reference counted. This
   * removes the reference to the parent before it goes stale. */
  OstreeMutableTree *child = (OstreeMutableTree*) data;
  child->parent = NULL;
  g_object_unref (child);
}

static void
ostree_mutable_tree_init (OstreeMutableTree *self)
{
  self->files = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       g_free, g_free);
  self->subdirs = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         g_free, remove_child_mtree);
}

static void
invalidate_contents_checksum (OstreeMutableTree *self)
{
  while (self) {
    if (!self->contents_checksum)
      break;

    g_free (self->contents_checksum);
    g_clear_pointer (&self->contents_checksum, g_free);
    self = self->parent;
  }
}

void
ostree_mutable_tree_set_metadata_checksum (OstreeMutableTree *self,
                                           const char        *checksum)
{
  if (g_strcmp0 (checksum, self->metadata_checksum) == 0)
    return;

  invalidate_contents_checksum (self->parent);
  g_free (self->metadata_checksum);
  self->metadata_checksum = g_strdup (checksum);
}

const char *
ostree_mutable_tree_get_metadata_checksum (OstreeMutableTree *self)
{
  return self->metadata_checksum;
}

void
ostree_mutable_tree_set_contents_checksum (OstreeMutableTree *self,
                                           const char        *checksum)
{
  if (g_strcmp0 (checksum, self->contents_checksum) == 0)
    return;

  if (checksum && self->contents_checksum)
    g_warning ("Setting a contents checksum on an OstreeMutableTree that "
        "already has a checksum set.  Old checksum %s, new checksum %s",
        self->contents_checksum, checksum);

  g_free (self->contents_checksum);
  self->contents_checksum = g_strdup (checksum);
}

const char *
ostree_mutable_tree_get_contents_checksum (OstreeMutableTree *self)
{
  return self->contents_checksum;
}

static gboolean
set_error_noent (GError **error, const char *path)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               "No such file or directory: %s",
               path);
  return FALSE;
}

gboolean
ostree_mutable_tree_replace_file (OstreeMutableTree *self,
                                  const char        *name,
                                  const char        *checksum,
                                  GError           **error)
{
  gboolean ret = FALSE;

  g_return_val_if_fail (name != NULL, FALSE);

  if (!ot_util_filename_validate (name, error))
    goto out;

  if (g_hash_table_lookup (self->subdirs, name))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't replace directory with file: %s", name);
      goto out;
    }

  invalidate_contents_checksum (self);
  g_hash_table_replace (self->files,
                        g_strdup (name),
                        g_strdup (checksum));

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_mutable_tree_ensure_dir:
 * @self: Tree
 * @name: Name of subdirectory of self to retrieve/creates
 * @out_subdir: (out) (transfer full): the subdirectory
 * @error: a #GError
 *
 * Returns the subdirectory of self with filename @name, creating an empty one
 * it if it doesn't exist.
 */
gboolean
ostree_mutable_tree_ensure_dir (OstreeMutableTree *self,
                                const char        *name,
                                OstreeMutableTree **out_subdir,
                                GError           **error)
{
  gboolean ret = FALSE;
  g_autoptr(OstreeMutableTree) ret_dir = NULL;

  g_return_val_if_fail (name != NULL, FALSE);

  if (!ot_util_filename_validate (name, error))
    goto out;

  if (g_hash_table_lookup (self->files, name))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't replace file with directory: %s", name);
      goto out;
    }

  ret_dir = ot_gobject_refz (g_hash_table_lookup (self->subdirs, name));
  if (!ret_dir)
    {
      ret_dir = ostree_mutable_tree_new ();
      insert_child_mtree (self, name, g_object_ref (ret_dir));
    }
  
  ret = TRUE;
  ot_transfer_out_value (out_subdir, &ret_dir);
 out:
  return ret;
}

gboolean
ostree_mutable_tree_lookup (OstreeMutableTree   *self,
                            const char          *name,
                            char               **out_file_checksum,
                            OstreeMutableTree  **out_subdir,
                            GError             **error)
{
  gboolean ret = FALSE;
  g_autoptr(OstreeMutableTree) ret_subdir = NULL;
  g_autofree char *ret_file_checksum = NULL;
  
  ret_subdir = ot_gobject_refz (g_hash_table_lookup (self->subdirs, name));
  if (!ret_subdir)
    {
      ret_file_checksum = g_strdup (g_hash_table_lookup (self->files, name));
      if (!ret_file_checksum)
        {
          set_error_noent (error, name);
          goto out;
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_file_checksum, &ret_file_checksum);
  ot_transfer_out_value (out_subdir, &ret_subdir);
 out:
  return ret;
}

/**
 * ostree_mutable_tree_ensure_parent_dirs:
 * @self: Tree
 * @split_path: (element-type utf8): File path components
 * @metadata_checksum: SHA256 checksum for metadata
 * @out_parent: (out) (transfer full): The parent tree
 * @error: a #GError
 *
 * Create all parent trees necessary for the given @split_path to
 * exist.
 */
gboolean
ostree_mutable_tree_ensure_parent_dirs (OstreeMutableTree  *self,
                                        GPtrArray          *split_path,
                                        const char         *metadata_checksum,
                                        OstreeMutableTree **out_parent,
                                        GError            **error)
{
  gboolean ret = FALSE;
  int i;
  OstreeMutableTree *subdir = self; /* nofree */
  g_autoptr(OstreeMutableTree) ret_parent = NULL;

  g_assert (metadata_checksum != NULL);

  if (!self->metadata_checksum)
    ostree_mutable_tree_set_metadata_checksum (self, metadata_checksum);

  for (i = 0; i+1 < split_path->len; i++)
    {
      OstreeMutableTree *next;
      const char *name = split_path->pdata[i];

      if (g_hash_table_lookup (subdir->files, name))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Can't replace file with directory: %s", name);
          goto out;
        }

      next = g_hash_table_lookup (subdir->subdirs, name);
      if (!next) 
        {
          next = ostree_mutable_tree_new ();
          ostree_mutable_tree_set_metadata_checksum (next, metadata_checksum);
          insert_child_mtree (subdir, g_strdup (name), next);
        }
      
      subdir = next;
    }

  ret_parent = g_object_ref (subdir);

  ret = TRUE;
  ot_transfer_out_value (out_parent, &ret_parent);
 out:
  return ret;
}

/**
 * ostree_mutable_tree_walk:
 * @self: Tree
 * @split_path: (element-type utf8): Split pathname
 * @start: Descend from this number of elements in @split_path
 * @out_subdir: (out) (transfer full): Target parent
 * @error: Error
 *
 * Traverse @start number of elements starting from @split_path; the
 * child will be returned in @out_subdir.
 */
gboolean
ostree_mutable_tree_walk (OstreeMutableTree     *self,
                          GPtrArray             *split_path,
                          guint                  start,
                          OstreeMutableTree    **out_subdir,
                          GError               **error)
{
  g_return_val_if_fail (start < split_path->len, FALSE);

  if (start == split_path->len - 1)
    {
      *out_subdir = g_object_ref (self);
      return TRUE;
    }
  else
    {
      OstreeMutableTree *subdir;

      subdir = g_hash_table_lookup (self->subdirs, split_path->pdata[start]);
      if (!subdir)
        return set_error_noent (error, (char*)split_path->pdata[start]);

      return ostree_mutable_tree_walk (subdir, split_path, start + 1, out_subdir, error);
    }
}

/**
 * ostree_mutable_tree_get_subdirs:
 * @self:
 * 
 * Returns: (transfer none) (element-type utf8 OstreeMutableTree): All children directories
 */
GHashTable *
ostree_mutable_tree_get_subdirs (OstreeMutableTree *self)
{
  return self->subdirs;
}

/**
 * ostree_mutable_tree_get_files:
 * @self:
 * 
 * Returns: (transfer none) (element-type utf8 utf8): All children files (the value is a checksum)
 */
GHashTable *
ostree_mutable_tree_get_files (OstreeMutableTree *self)
{
  return self->files;
}

/**
 * ostree_mutable_tree_new:
 *
 * Returns: (transfer full): A new tree
 */
OstreeMutableTree *
ostree_mutable_tree_new (void)
{
  return (OstreeMutableTree*)g_object_new (OSTREE_TYPE_MUTABLE_TREE, NULL);
}
