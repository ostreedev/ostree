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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ostree.h"
#include "otutil.h"

#include "ostree-core-private.h"

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

typedef enum
{
  MTREE_STATE_WHOLE,

  /* MTREE_STATE_LAZY allows us to not read files and subdirs from the objects
   * on disk until they're actually needed - often they won't be needed at
   * all. */
  MTREE_STATE_LAZY
} OstreeMutableTreeState;

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

  OstreeMutableTreeState state;

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

  /* ======== Valid for state LAZY: =========== */

  /* The repo so we can look up the checksums. */
  OstreeRepo *repo;

  GError *cached_error;

  /* ======== Valid for state WHOLE: ========== */

  /* const char* filename -> const char* checksum. */
  GHashTable *files;

  /* const char* filename -> OstreeMutableTree* subtree */
  GHashTable *subdirs;
};

G_DEFINE_TYPE (OstreeMutableTree, ostree_mutable_tree, G_TYPE_OBJECT)

static void
ostree_mutable_tree_finalize (GObject *object)
{
  OstreeMutableTree *self;

  self = OSTREE_MUTABLE_TREE (object);

  g_free (self->contents_checksum);
  g_free (self->metadata_checksum);

  g_clear_pointer (&self->cached_error, g_error_free);
  g_hash_table_destroy (self->files);
  g_hash_table_destroy (self->subdirs);

  g_clear_object (&self->repo);

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
insert_child_mtree (OstreeMutableTree *self, const gchar *name, OstreeMutableTree *child)
{
  g_assert_null (child->parent);
  g_hash_table_insert (self->subdirs, g_strdup (name), child);
  child->parent = self;
}

static void
remove_child_mtree (gpointer data)
{
  /* Each mtree has shared ownership of its children and each child has a
   * non-owning reference back to parent.  If the parent goes out of scope the
   * children may still be alive because they're reference counted. This
   * removes the reference to the parent before it goes stale. */
  OstreeMutableTree *child = (OstreeMutableTree *)data;
  child->parent = NULL;
  g_object_unref (child);
}

static void
ostree_mutable_tree_init (OstreeMutableTree *self)
{
  self->files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->subdirs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, remove_child_mtree);
  self->state = MTREE_STATE_WHOLE;
}

static void
invalidate_contents_checksum (OstreeMutableTree *self)
{
  while (self)
    {
      if (!self->contents_checksum)
        break;

      g_clear_pointer (&self->contents_checksum, g_free);
      self = self->parent;
    }
}

/* Go from state LAZY to state WHOLE by reading the tree from disk */
static gboolean
_ostree_mutable_tree_make_whole (OstreeMutableTree *self, GCancellable *cancellable, GError **error)
{
  if (self->state == MTREE_STATE_WHOLE)
    return TRUE;

  g_assert_cmpuint (self->state, ==, MTREE_STATE_LAZY);
  g_assert_nonnull (self->repo);
  g_assert_nonnull (self->contents_checksum);
  g_assert_nonnull (self->metadata_checksum);
  g_assert_cmpuint (g_hash_table_size (self->files), ==, 0);
  g_assert_cmpuint (g_hash_table_size (self->subdirs), ==, 0);

  g_autoptr (GVariant) dirtree = NULL;
  if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_DIR_TREE, self->contents_checksum,
                                 &dirtree, error))
    return FALSE;

  {
    g_autoptr (GVariant) dir_file_contents = g_variant_get_child_value (dirtree, 0);
    GVariantIter viter;
    g_variant_iter_init (&viter, dir_file_contents);
    const char *fname;
    GVariant *contents_csum_v = NULL;
    while (g_variant_iter_loop (&viter, "(&s@ay)", &fname, &contents_csum_v))
      {
        char tmp_checksum[OSTREE_SHA256_STRING_LEN + 1];
        _ostree_checksum_inplace_from_bytes_v (contents_csum_v, tmp_checksum);
        g_hash_table_insert (self->files, g_strdup (fname), g_strdup (tmp_checksum));
      }
  }

  /* Process subdirectories */
  {
    g_autoptr (GVariant) dir_subdirs = g_variant_get_child_value (dirtree, 1);
    const char *dname;
    GVariant *subdirtree_csum_v = NULL;
    GVariant *subdirmeta_csum_v = NULL;
    GVariantIter viter;
    g_variant_iter_init (&viter, dir_subdirs);
    while (
        g_variant_iter_loop (&viter, "(&s@ay@ay)", &dname, &subdirtree_csum_v, &subdirmeta_csum_v))
      {
        char subdirtree_checksum[OSTREE_SHA256_STRING_LEN + 1];
        _ostree_checksum_inplace_from_bytes_v (subdirtree_csum_v, subdirtree_checksum);
        char subdirmeta_checksum[OSTREE_SHA256_STRING_LEN + 1];
        _ostree_checksum_inplace_from_bytes_v (subdirmeta_csum_v, subdirmeta_checksum);
        insert_child_mtree (self, dname,
                            ostree_mutable_tree_new_from_checksum (self->repo, subdirtree_checksum,
                                                                   subdirmeta_checksum));
      }
  }

  g_clear_object (&self->repo);
  self->state = MTREE_STATE_WHOLE;
  return TRUE;
}

/* _ostree_mutable_tree_make_whole can fail if state == MTREE_STATE_LAZY, but
 * we have getters that preceed the existence of MTREE_STATE_LAZY which can't
 * return errors.  So instead this function will fail and print a warning. */
static gboolean
_assert_ostree_mutable_tree_make_whole (OstreeMutableTree *self)
{
  if (self->cached_error)
    return FALSE;
  return _ostree_mutable_tree_make_whole (self, NULL, &self->cached_error);
}

void
ostree_mutable_tree_set_metadata_checksum (OstreeMutableTree *self, const char *checksum)
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
ostree_mutable_tree_set_contents_checksum (OstreeMutableTree *self, const char *checksum)
{
  if (g_strcmp0 (checksum, self->contents_checksum) == 0)
    return;

  if (checksum && self->contents_checksum)
    g_warning ("Setting a contents checksum on an OstreeMutableTree that "
               "already has a checksum set.  Old checksum %s, new checksum %s",
               self->contents_checksum, checksum);

  _assert_ostree_mutable_tree_make_whole (self);

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
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No such file or directory: %s", path);
  return FALSE;
}

gboolean
ostree_mutable_tree_replace_file (OstreeMutableTree *self, const char *name, const char *checksum,
                                  GError **error)
{
  if (!ot_util_filename_validate (name, error))
    return FALSE;

  if (!_ostree_mutable_tree_make_whole (self, NULL, error))
    return FALSE;

  if (g_hash_table_lookup (self->subdirs, name))
    return glnx_throw (error, "Can't replace directory with file: %s", name);

  invalidate_contents_checksum (self);
  g_hash_table_replace (self->files, g_strdup (name), g_strdup (checksum));
  return TRUE;
}

/**
 * ostree_mutable_tree_remove:
 * @self: Tree
 * @name: Name of file or subdirectory to remove
 * @allow_noent: If @FALSE, an error will be thrown if @name does not exist in the tree
 * @error: a #GError
 *
 * Remove the file or subdirectory named @name from the mutable tree @self.
 *
 * Since: 2018.9
 */
gboolean
ostree_mutable_tree_remove (OstreeMutableTree *self, const char *name, gboolean allow_noent,
                            GError **error)
{
  if (!ot_util_filename_validate (name, error))
    return FALSE;

  if (!_ostree_mutable_tree_make_whole (self, NULL, error))
    return FALSE;

  if (!g_hash_table_remove (self->files, name) && !g_hash_table_remove (self->subdirs, name))
    {
      if (allow_noent)
        return TRUE; /* NB: early return */
      return set_error_noent (error, name);
    }

  invalidate_contents_checksum (self);
  return TRUE;
}

/**
 * ostree_mutable_tree_ensure_dir:
 * @self: Tree
 * @name: Name of subdirectory of self to retrieve/creates
 * @out_subdir: (out) (transfer full) (optional): the subdirectory
 * @error: a #GError
 *
 * Returns the subdirectory of self with filename @name, creating an empty one
 * it if it doesn't exist.
 */
gboolean
ostree_mutable_tree_ensure_dir (OstreeMutableTree *self, const char *name,
                                OstreeMutableTree **out_subdir, GError **error)
{
  if (!ot_util_filename_validate (name, error))
    return FALSE;

  if (!_ostree_mutable_tree_make_whole (self, NULL, error))
    return FALSE;

  if (g_hash_table_lookup (self->files, name))
    return glnx_throw (error, "Can't replace file with directory: %s", name);

  g_autoptr (OstreeMutableTree) ret_dir
      = ot_gobject_refz (g_hash_table_lookup (self->subdirs, name));
  if (!ret_dir)
    {
      ret_dir = ostree_mutable_tree_new ();
      invalidate_contents_checksum (self);
      insert_child_mtree (self, name, g_object_ref (ret_dir));
    }

  if (out_subdir)
    *out_subdir = g_steal_pointer (&ret_dir);
  return TRUE;
}

/**
 * ostree_mutable_tree_lookup:
 * @self: Tree
 * @name: name
 * @out_file_checksum: (out) (transfer full) (nullable) (optional): checksum
 * @out_subdir: (out) (transfer full) (nullable) (optional): subdirectory
 * @error: a #GError
 *
 * Lookup @name and returns @out_file_checksum or @out_subdir depending on its
 * file type.
 *
 * Returns: %TRUE on success and either @out_file_checksum or @out_subdir are
 * filled, %FALSE otherwise.
 */
gboolean
ostree_mutable_tree_lookup (OstreeMutableTree *self, const char *name, char **out_file_checksum,
                            OstreeMutableTree **out_subdir, GError **error)
{
  if (!_ostree_mutable_tree_make_whole (self, NULL, error))
    return FALSE;

  g_autofree char *ret_file_checksum = NULL;
  g_autoptr (OstreeMutableTree) ret_subdir
      = ot_gobject_refz (g_hash_table_lookup (self->subdirs, name));
  if (!ret_subdir)
    {
      ret_file_checksum = g_strdup (g_hash_table_lookup (self->files, name));
      if (!ret_file_checksum)
        return set_error_noent (error, name);
    }

  if (out_file_checksum)
    *out_file_checksum = g_steal_pointer (&ret_file_checksum);
  if (out_subdir)
    *out_subdir = g_steal_pointer (&ret_subdir);
  return TRUE;
}

/**
 * ostree_mutable_tree_ensure_parent_dirs:
 * @self: Tree
 * @split_path: (element-type utf8): File path components
 * @metadata_checksum: SHA256 checksum for metadata
 * @out_parent: (out) (transfer full) (optional): The parent tree
 * @error: a #GError
 *
 * Create all parent trees necessary for the given @split_path to
 * exist.
 */
gboolean
ostree_mutable_tree_ensure_parent_dirs (OstreeMutableTree *self, GPtrArray *split_path,
                                        const char *metadata_checksum,
                                        OstreeMutableTree **out_parent, GError **error)
{
  g_assert (metadata_checksum != NULL);

  if (!_ostree_mutable_tree_make_whole (self, NULL, error))
    return FALSE;

  if (!self->metadata_checksum)
    ostree_mutable_tree_set_metadata_checksum (self, metadata_checksum);

  OstreeMutableTree *subdir = self; /* nofree */
  for (guint i = 0; i + 1 < split_path->len; i++)
    {
      const char *name = split_path->pdata[i];
      if (g_hash_table_lookup (subdir->files, name))
        return glnx_throw (error, "Can't replace file with directory: %s", name);

      OstreeMutableTree *next = g_hash_table_lookup (subdir->subdirs, name);
      if (!next)
        {
          invalidate_contents_checksum (subdir);
          next = ostree_mutable_tree_new ();
          ostree_mutable_tree_set_metadata_checksum (next, metadata_checksum);
          insert_child_mtree (subdir, name, next);
        }

      subdir = next;
      g_assert (subdir);
      if (!_ostree_mutable_tree_make_whole (subdir, NULL, error))
        return FALSE;
    }

  if (out_parent)
    *out_parent = g_object_ref (subdir);
  return TRUE;
}

const char empty_tree_csum[] = "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d";

/**
 * ostree_mutable_tree_fill_empty_from_dirtree:
 *
 * Merges @self with the tree given by @contents_checksum and
 * @metadata_checksum, but only if it's possible without writing new objects to
 * the @repo.  We can do this if either @self is empty, the tree given by
 * @contents_checksum is empty or if both trees already have the same
 * @contents_checksum.
 *
 * Returns: @TRUE if merge was successful, @FALSE if it was not possible.
 *
 * This function enables optimisations when composing trees.  The provided
 * checksums are not loaded or checked when this function is called.  Instead
 * the contents will be loaded only when needed.
 *
 * Since: 2018.7
 */
gboolean
ostree_mutable_tree_fill_empty_from_dirtree (OstreeMutableTree *self, OstreeRepo *repo,
                                             const char *contents_checksum,
                                             const char *metadata_checksum)
{
  g_assert (repo);
  g_assert (contents_checksum);
  g_assert (metadata_checksum);

  switch (self->state)
    {
    case MTREE_STATE_LAZY:
      {
        if (g_strcmp0 (contents_checksum, self->contents_checksum) == 0
            || g_strcmp0 (empty_tree_csum, self->contents_checksum) == 0)
          break;

        if (g_strcmp0 (empty_tree_csum, contents_checksum) == 0)
          {
            /* Adding an empty tree to a full one - stick with the old contents */
            g_set_object (&self->repo, repo);
            ostree_mutable_tree_set_metadata_checksum (self, metadata_checksum);
            return TRUE;
          }
        else
          return FALSE;
      }
    case MTREE_STATE_WHOLE:
      if (g_hash_table_size (self->files) == 0 && g_hash_table_size (self->subdirs) == 0)
        break;
      /* We're not empty - can't convert to a LAZY tree */
      return FALSE;
    default:
      g_assert_not_reached ();
    }

  self->state = MTREE_STATE_LAZY;
  g_set_object (&self->repo, repo);
  ostree_mutable_tree_set_metadata_checksum (self, metadata_checksum);
  if (g_strcmp0 (self->contents_checksum, contents_checksum) != 0)
    {
      invalidate_contents_checksum (self);
      self->contents_checksum = g_strdup (contents_checksum);
    }
  return TRUE;
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
ostree_mutable_tree_walk (OstreeMutableTree *self, GPtrArray *split_path, guint start,
                          OstreeMutableTree **out_subdir, GError **error)
{
  g_assert_cmpuint (start, <, split_path->len);

  if (start == split_path->len - 1)
    {
      *out_subdir = g_object_ref (self);
      return TRUE;
    }
  else
    {
      if (!_ostree_mutable_tree_make_whole (self, NULL, error))
        return FALSE;
      OstreeMutableTree *subdir = g_hash_table_lookup (self->subdirs, split_path->pdata[start]);
      if (!subdir)
        return set_error_noent (error, (char *)split_path->pdata[start]);

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
  _assert_ostree_mutable_tree_make_whole (self);
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
  _assert_ostree_mutable_tree_make_whole (self);
  return self->files;
}

/**
 * ostree_mutable_tree_check_error:
 * @self: Tree
 *
 * In some cases, a tree may be in a "lazy" state that loads
 * data in the background; if an error occurred during a non-throwing
 * API call, it will have been cached.  This function checks for a
 * cached error.  The tree remains in error state.
 *
 * Since: 2018.7
 * Returns: `TRUE` on success
 */
gboolean
ostree_mutable_tree_check_error (OstreeMutableTree *self, GError **error)
{
  if (self->cached_error)
    {
      if (error)
        *error = g_error_copy (self->cached_error);
      return FALSE;
    }
  return TRUE;
}

/**
 * ostree_mutable_tree_new:
 *
 * Returns: (transfer full): A new tree
 */
OstreeMutableTree *
ostree_mutable_tree_new (void)
{
  return (OstreeMutableTree *)g_object_new (OSTREE_TYPE_MUTABLE_TREE, NULL);
}

/**
 * ostree_mutable_tree_new_from_checksum:
 * @repo: The repo which contains the objects refered by the checksums.
 * @contents_checksum: dirtree checksum
 * @metadata_checksum: dirmeta checksum
 *
 * Creates a new OstreeMutableTree with the contents taken from the given repo
 * and checksums.  The data will be loaded from the repo lazily as needed.
 *
 * Returns: (transfer full): A new tree
 *
 * Since: 2018.7
 */
OstreeMutableTree *
ostree_mutable_tree_new_from_checksum (OstreeRepo *repo, const char *contents_checksum,
                                       const char *metadata_checksum)
{
  OstreeMutableTree *out = (OstreeMutableTree *)g_object_new (OSTREE_TYPE_MUTABLE_TREE, NULL);
  out->state = MTREE_STATE_LAZY;
  out->repo = g_object_ref (repo);
  out->contents_checksum = g_strdup (contents_checksum);
  out->metadata_checksum = g_strdup (metadata_checksum);
  return out;
}

/**
 * ostree_mutable_tree_new_from_commit:
 * @repo: The repo which contains the objects refered by the checksums.
 * @rev: ref or SHA-256 checksum
 *
 * Creates a new OstreeMutableTree with the contents taken from the given commit.
 * The data will be loaded from the repo lazily as needed.
 *
 * Returns: (transfer full): A new tree
 * Since: 2021.5
 */
OstreeMutableTree *
ostree_mutable_tree_new_from_commit (OstreeRepo *repo, const char *rev, GError **error)
{
  g_autofree char *commit = NULL;
  if (!ostree_repo_resolve_rev (repo, rev, FALSE, &commit, error))
    return NULL;
  g_autoptr (GVariant) commit_v = NULL;
  if (!ostree_repo_load_commit (repo, commit, &commit_v, NULL, error))
    return NULL;

  g_autoptr (GVariant) contents_checksum_v = NULL;
  g_autoptr (GVariant) metadata_checksum_v = NULL;
  char contents_checksum[OSTREE_SHA256_STRING_LEN + 1];
  char metadata_checksum[OSTREE_SHA256_STRING_LEN + 1];
  g_variant_get_child (commit_v, 6, "@ay", &contents_checksum_v);
  ostree_checksum_inplace_from_bytes (g_variant_get_data (contents_checksum_v), contents_checksum);
  g_variant_get_child (commit_v, 7, "@ay", &metadata_checksum_v);
  ostree_checksum_inplace_from_bytes (g_variant_get_data (metadata_checksum_v), metadata_checksum);
  return ostree_mutable_tree_new_from_checksum (repo, contents_checksum, metadata_checksum);
}
