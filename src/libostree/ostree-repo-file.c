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

#include "config.h"

#include "otutil.h"
#include "ostree-repo-file-enumerator.h"
#include "ostree-repo-private.h"

static void ostree_repo_file_file_iface_init (GFileIface *iface);

struct OstreeRepoFile
{
  GObject parent_instance;

  OstreeRepo *repo;
  OstreeRepoFile *parent;
  int index;
  char *name;

  char *cached_file_checksum;

  char *tree_contents_checksum;
  GVariant *tree_contents;
  char *tree_metadata_checksum;
  GVariant *tree_metadata;
};

G_DEFINE_TYPE_WITH_CODE (OstreeRepoFile, ostree_repo_file, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						ostree_repo_file_file_iface_init))

static void
ostree_repo_file_finalize (GObject *object)
{
  OstreeRepoFile *self;

  self = OSTREE_REPO_FILE (object);

  g_clear_pointer (&self->tree_contents, (GDestroyNotify) g_variant_unref);
  g_clear_pointer (&self->tree_metadata, (GDestroyNotify) g_variant_unref);
  g_free (self->cached_file_checksum);
  g_free (self->tree_contents_checksum);
  g_free (self->tree_metadata_checksum);
  g_free (self->name);

  G_OBJECT_CLASS (ostree_repo_file_parent_class)->finalize (object);
}

static void
ostree_repo_file_dispose (GObject *object)
{
  OstreeRepoFile *self;

  self = OSTREE_REPO_FILE (object);

  g_clear_object (&self->repo);
  g_clear_object (&self->parent);

  if (G_OBJECT_CLASS (ostree_repo_file_parent_class)->dispose)
    G_OBJECT_CLASS (ostree_repo_file_parent_class)->dispose (object);
}

static void
ostree_repo_file_class_init (OstreeRepoFileClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = ostree_repo_file_finalize;
  gobject_class->dispose = ostree_repo_file_dispose;
}

static void
ostree_repo_file_init (OstreeRepoFile *self)
{
  self->index = -1;
}

static gboolean
set_error_noent (GFile *self, GError **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               "No such file or directory: %s",
               gs_file_get_path_cached (self));
  return FALSE;
}

OstreeRepoFile *
_ostree_repo_file_new_root (OstreeRepo *repo,
                            const char *contents_checksum,
                            const char *metadata_checksum)
{
  OstreeRepoFile *self;

  g_return_val_if_fail (repo != NULL, NULL);
  g_return_val_if_fail (contents_checksum != NULL, NULL);
  g_return_val_if_fail (strlen (contents_checksum) == OSTREE_SHA256_STRING_LEN, NULL);
  g_return_val_if_fail (metadata_checksum != NULL, NULL);
  g_return_val_if_fail (strlen (metadata_checksum) == OSTREE_SHA256_STRING_LEN, NULL);

  self = g_object_new (OSTREE_TYPE_REPO_FILE, NULL);
  self->repo = g_object_ref (repo);
  self->tree_contents_checksum = g_strdup (contents_checksum);
  self->tree_metadata_checksum = g_strdup (metadata_checksum);

  return self;
}

static OstreeRepoFile *
ostree_repo_file_new_child (OstreeRepoFile *parent,
                            const char     *name)
{
  OstreeRepoFile *self;
  size_t len;
  
  self = g_object_new (OSTREE_TYPE_REPO_FILE, NULL);
  self->repo = g_object_ref (parent->repo);
  self->parent = g_object_ref (parent);
  self->name = g_strdup (name);
  len = strlen(self->name);
  if (self->name[len-1] == '/')
    self->name[len-1] = '\0';

  return self;
}

OstreeRepoFile *
_ostree_repo_file_new_for_commit (OstreeRepo  *repo,
                                  const char  *commit,
                                  GError     **error)
{
  g_autoptr(GVariant) commit_v = NULL;
  g_autoptr(GVariant) tree_contents_csum_v = NULL;
  g_autoptr(GVariant) tree_metadata_csum_v = NULL;
  char tree_contents_csum[OSTREE_SHA256_STRING_LEN + 1];
  char tree_metadata_csum[OSTREE_SHA256_STRING_LEN + 1];

  g_return_val_if_fail (repo != NULL, NULL);
  g_return_val_if_fail (commit != NULL, NULL);
  g_return_val_if_fail (strlen (commit) == OSTREE_SHA256_STRING_LEN, NULL);

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 commit, &commit_v, error))
    return NULL;

  /* PARSE OSTREE_OBJECT_TYPE_COMMIT */
  g_variant_get_child (commit_v, 6, "@ay", &tree_contents_csum_v);
  ostree_checksum_inplace_from_bytes (g_variant_get_data (tree_contents_csum_v),
                                      tree_contents_csum);

  g_variant_get_child (commit_v, 7, "@ay", &tree_metadata_csum_v);
  ostree_checksum_inplace_from_bytes (g_variant_get_data (tree_metadata_csum_v),
                                      tree_metadata_csum);

  return _ostree_repo_file_new_root (repo, tree_contents_csum, tree_metadata_csum);
}

static gboolean
do_resolve (OstreeRepoFile  *self,
            GError         **error)
{
  g_autoptr(GVariant) root_contents = NULL;
  g_autoptr(GVariant) root_metadata = NULL;

  g_assert (self->parent == NULL);

  if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_DIR_TREE,
                                 self->tree_contents_checksum, &root_contents, error))
    return FALSE;

  if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_DIR_META,
                                 self->tree_metadata_checksum, &root_metadata, error))
    return FALSE;

  self->tree_metadata = root_metadata;
  root_metadata = NULL;
  self->tree_contents = root_contents;
  root_contents = NULL;

  return TRUE;
}

static gboolean
do_resolve_nonroot (OstreeRepoFile     *self,
                    GError            **error)
{
  gboolean is_dir;
  int i;
  g_autoptr(GVariant) container = NULL;
  g_autoptr(GVariant) tree_contents = NULL;
  g_autoptr(GVariant) tree_metadata = NULL;
  g_autoptr(GVariant) contents_csum_v = NULL;
  g_autoptr(GVariant) metadata_csum_v = NULL;
  g_autofree char *tmp_checksum = NULL;

  if (!ostree_repo_file_ensure_resolved (self->parent, error))
    return FALSE;

  if (!self->parent->tree_contents)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                   "Not a directory");
      return FALSE;
    }

  i = ostree_repo_file_tree_find_child (self->parent, self->name, &is_dir, &container);

  if (i < 0)
    {
      set_error_noent ((GFile*)self, error);
      return FALSE;
    }

  if (is_dir)
    {
      const char *name;
      GVariant *files_variant;

      files_variant = g_variant_get_child_value (self->parent->tree_contents, 0);
      self->index = g_variant_n_children (files_variant) + i;
      g_clear_pointer (&files_variant, (GDestroyNotify) g_variant_unref);

      g_variant_get_child (container, i, "(&s@ay@ay)",
                           &name, &contents_csum_v, &metadata_csum_v);

      g_free (tmp_checksum);
      tmp_checksum = ostree_checksum_from_bytes_v (contents_csum_v);
      if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_DIR_TREE,
                                     tmp_checksum, &tree_contents,
                                     error))
        return FALSE;

      g_free (tmp_checksum);
      tmp_checksum = ostree_checksum_from_bytes_v (metadata_csum_v);
      if (!ostree_repo_load_variant (self->repo, OSTREE_OBJECT_TYPE_DIR_META,
                                     tmp_checksum, &tree_metadata,
                                     error))
        return FALSE;

      self->tree_contents = tree_contents;
      tree_contents = NULL;
      self->tree_metadata = tree_metadata;
      tree_metadata = NULL;
      self->tree_contents_checksum = ostree_checksum_from_bytes_v (contents_csum_v);
      self->tree_metadata_checksum = ostree_checksum_from_bytes_v (metadata_csum_v);
    }
  else
    self->index = i;

  return TRUE;
}

gboolean
ostree_repo_file_ensure_resolved (OstreeRepoFile  *self,
                                   GError         **error)
{
  if (self->parent == NULL)
    {
      if (self->tree_contents == NULL)
        if (!do_resolve (self, error))
          return FALSE;
    }
  else
    {
      if (self->index == -1)
        {
          if (!do_resolve_nonroot (self, error))
            return FALSE;
        }
    }

  return TRUE;
}

gboolean
ostree_repo_file_get_xattrs (OstreeRepoFile  *self,
                             GVariant       **out_xattrs,
                             GCancellable    *cancellable,
                             GError         **error)
{
  if (!ostree_repo_file_ensure_resolved (self, error))
    return FALSE;

  g_autoptr(GVariant) ret_xattrs = NULL;
  if (self->tree_metadata)
    ret_xattrs = g_variant_get_child_value (self->tree_metadata, 3);
  else
    {
      if (!ostree_repo_load_file (self->repo, ostree_repo_file_get_checksum (self),
                                  NULL, NULL, &ret_xattrs, cancellable, error))
        return FALSE;
    }

  ot_transfer_out_value(out_xattrs, &ret_xattrs);
  return TRUE;
}

GVariant *
ostree_repo_file_tree_get_contents (OstreeRepoFile  *self)
{
  return self->tree_contents;
}

GVariant *
ostree_repo_file_tree_get_metadata (OstreeRepoFile  *self)
{
  return self->tree_metadata;
}

void
ostree_repo_file_tree_set_metadata (OstreeRepoFile *self,
                                     const char     *checksum,
                                     GVariant       *metadata)
{
  g_clear_pointer (&self->tree_metadata, (GDestroyNotify) g_variant_unref);
  self->tree_metadata = g_variant_ref (metadata);
  g_free (self->tree_metadata_checksum);
  self->tree_metadata_checksum = g_strdup (checksum);
}

const char *
ostree_repo_file_tree_get_contents_checksum (OstreeRepoFile  *self)
{
  return self->tree_contents_checksum;
}

const char *
ostree_repo_file_tree_get_metadata_checksum (OstreeRepoFile  *self)
{
  return self->tree_metadata_checksum;
}

/**
 * ostree_repo_file_get_repo:
 * @self:
 *
 * Returns: (transfer none): Repository
 */
OstreeRepo *
ostree_repo_file_get_repo (OstreeRepoFile  *self)
{
  return self->repo;
}

/**
 * ostree_repo_file_get_root:
 * @self:
 *
 * Returns: (transfer none): The root directory for the commit referenced by this file
 */
OstreeRepoFile *
ostree_repo_file_get_root (OstreeRepoFile  *self)
{
  OstreeRepoFile *parent = self;

  while (parent->parent)
    parent = parent->parent;
  return parent;
}

const char *
ostree_repo_file_get_checksum (OstreeRepoFile  *self)
{
  int n;
  gboolean is_dir;
  GVariant *files_variant;
  GVariant *dirs_variant;
  GVariant *csum_bytes;

  if (!self->parent)
    return self->tree_metadata_checksum;

  if (self->cached_file_checksum)
    return self->cached_file_checksum;

  n = ostree_repo_file_tree_find_child (self->parent, self->name, &is_dir, NULL);
  g_assert (n >= 0);

  files_variant = g_variant_get_child_value (self->parent->tree_contents, 0);
  dirs_variant = g_variant_get_child_value (self->parent->tree_contents, 1);

  if (is_dir)
    {
      g_variant_get_child (dirs_variant, n,
                           "(@s@ay@ay)", NULL, NULL, &csum_bytes);
    }
  else
    {
      g_variant_get_child (files_variant, n,
                           "(@s@ay)", NULL, &csum_bytes);
    }
  g_clear_pointer (&files_variant, (GDestroyNotify) g_variant_unref);
  g_clear_pointer (&dirs_variant, (GDestroyNotify) g_variant_unref);

  self->cached_file_checksum = ostree_checksum_from_bytes_v (csum_bytes);

  g_variant_unref (csum_bytes);

  return self->cached_file_checksum;
}

static gboolean
ostree_repo_file_is_native (GFile *file)
{
  return FALSE;
}

static gboolean
ostree_repo_file_has_uri_scheme (GFile      *file,
				 const char *uri_scheme)
{
  return g_ascii_strcasecmp (uri_scheme, "ostree") == 0;
}

static char *
ostree_repo_file_get_uri_scheme (GFile *file)
{
  return g_strdup ("ostree");
}

static char *
ostree_repo_file_get_basename (GFile *file)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  return g_strdup (self->name);
}

static char *
ostree_repo_file_get_path (GFile *file)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  OstreeRepoFile *parent;
  GString *buf;
  GSList *parents;
  GSList *iter;

  buf = g_string_new ("");
  parents = NULL;

  for (parent = self->parent; parent; parent = parent->parent)
    parents = g_slist_prepend (parents, parent);

  if (parents && parents->next)
    {
      for (iter = parents->next; iter; iter = iter->next)
        {
          parent = iter->data;
          g_string_append_c (buf, '/');
          g_string_append (buf, parent->name);
        }
    }
  g_string_append_c (buf, '/');
  if (self->name)
    g_string_append (buf, self->name);

  g_slist_free (parents);

  return g_string_free (buf, FALSE);
}

static char *
ostree_repo_file_get_uri (GFile *file)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  const char *path;
  char *uri_path;
  char *ret;
  OstreeRepoFile *root = ostree_repo_file_get_root (self);

  path = gs_file_get_path_cached (file);
  uri_path = g_filename_to_uri (path, NULL, NULL);
  g_assert (g_str_has_prefix (uri_path, "file://"));
  ret = g_strconcat ("ostree://",
                     root->tree_contents_checksum, "/", root->tree_metadata_checksum,
                     uri_path+strlen("file://"),
                     NULL);
  g_free (uri_path);

  return ret;
}

static char *
ostree_repo_file_get_parse_name (GFile *file)
{
  return ostree_repo_file_get_uri (file);
}

static GFile *
ostree_repo_file_get_parent (GFile *file)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);

  return g_object_ref (self->parent);
}

static GFile *
ostree_repo_file_dup (GFile *file)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);

  if (self->parent)
    return G_FILE (ostree_repo_file_new_child (self->parent, self->name));
  else
    return G_FILE (_ostree_repo_file_new_root (self->repo, self->tree_contents_checksum, self->tree_metadata_checksum));
}

static guint
ostree_repo_file_hash (GFile *file)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  
  if (self->parent)
    return g_file_hash (self->parent) + g_str_hash (self->name);
  else
    return g_str_hash (self->tree_contents_checksum) + g_str_hash (self->tree_metadata_checksum);
}

static gboolean
ostree_repo_file_equal (GFile *file1,
                        GFile *file2)
{
  OstreeRepoFile *self1 = OSTREE_REPO_FILE (file1);
  OstreeRepoFile *self2 = OSTREE_REPO_FILE (file2);

  if (self1->parent && self2->parent)
    {
      return (g_str_equal (self1->name, self2->name) &&
              g_file_equal ((GFile*)self1->parent, (GFile*)self2->parent));
    }
  else if (!self1->parent && !self2->parent)
    {
      return (g_str_equal (self1->tree_contents_checksum, self2->tree_contents_checksum) &&
              g_str_equal (self1->tree_metadata_checksum, self2->tree_metadata_checksum));
    }
  else
    return FALSE;
}

static const char *
match_prefix (const char *path, 
              const char *prefix)
{
  int prefix_len;

  prefix_len = strlen (prefix);
  if (strncmp (path, prefix, prefix_len) != 0)
    return NULL;
  
  /* Handle the case where prefix is the root, so that
   * the IS_DIR_SEPRARATOR check below works */
  if (prefix_len > 0 &&
      G_IS_DIR_SEPARATOR (prefix[prefix_len-1]))
    prefix_len--;
  
  return path + prefix_len;
}

static gboolean
ostree_repo_file_prefix_matches (GFile *parent,
				 GFile *descendant)
{
  const char *remainder;
  const char *parent_path;
  const char *descendant_path;

  parent_path = gs_file_get_path_cached (parent);
  descendant_path = gs_file_get_path_cached (descendant);
  remainder = match_prefix (descendant_path, parent_path);
  if (remainder != NULL && G_IS_DIR_SEPARATOR (*remainder))
    return TRUE;
  return FALSE;
}

static char *
ostree_repo_file_get_relative_path (GFile *parent,
				    GFile *descendant)
{
  const char *remainder;
  const char *parent_path;
  const char *descendant_path;

  parent_path = gs_file_get_path_cached (parent);
  descendant_path = gs_file_get_path_cached (descendant);
  remainder = match_prefix (descendant_path, parent_path);
  
  if (remainder != NULL && G_IS_DIR_SEPARATOR (*remainder))
    return g_strdup (remainder + 1);
  return NULL;
}

static GFile *
ostree_repo_file_resolve_relative_path (GFile      *file,
					const char *relative_path)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  OstreeRepoFile *parent;
  char *filename;
  const char *rest;
  GFile *ret;

  if (g_path_is_absolute (relative_path))
    {
      g_assert (*relative_path == '/');

      if (strcmp (relative_path, "/") == 0)
        return g_object_ref (ostree_repo_file_get_root (self)); 

      if (self->parent)
        return ostree_repo_file_resolve_relative_path ((GFile*)ostree_repo_file_get_root (self),
                                                       relative_path+1);
      else
        relative_path = relative_path+1;
    }

  rest = strchr (relative_path, '/');
  if (rest)
    {
      rest += 1;
      filename = g_strndup (relative_path, rest - relative_path);
    }
  else
    filename = g_strdup (relative_path);

  parent = ostree_repo_file_new_child (self, filename);
  g_free (filename);
    
  if (!rest)
    ret = (GFile*)parent;
  else
    {
      ret = ostree_repo_file_resolve_relative_path ((GFile*)parent, rest);
      g_clear_object (&parent);
    }
  return ret;
}

static GFileEnumerator *
ostree_repo_file_enumerate_children (GFile                *file,
				     const char           *attributes,
				     GFileQueryInfoFlags   flags,
				     GCancellable         *cancellable,
				     GError              **error)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  return _ostree_repo_file_enumerator_new (self,
					   attributes, flags,
					   cancellable, error);
}

static GFile *
ostree_repo_file_get_child_for_display_name (GFile        *file,
					 const char   *display_name,
					 GError      **error)
{
  return g_file_get_child (file, display_name);
}

static void
set_info_from_dirmeta (GFileInfo  *info,
                       GVariant   *metadata)
{
  guint32 uid, gid, mode;

  g_file_info_set_attribute_uint32 (info, "standard::type", G_FILE_TYPE_DIRECTORY);

  /* PARSE OSTREE_OBJECT_TYPE_DIR_META */
  g_variant_get (metadata, "(uuu@a(ayay))",
                 &uid, &gid, &mode, NULL);
  uid = GUINT32_FROM_BE (uid);
  gid = GUINT32_FROM_BE (gid);
  mode = GUINT32_FROM_BE (mode);

  g_file_info_set_attribute_uint32 (info, "unix::uid", uid);
  g_file_info_set_attribute_uint32 (info, "unix::gid", gid);
  g_file_info_set_attribute_uint32 (info, "unix::mode", mode);
}

static gboolean
query_child_info_dir (OstreeRepo               *repo,
                      const char               *metadata_checksum,
                      GFileAttributeMatcher    *matcher,
                      GFileQueryInfoFlags       flags,
                      GFileInfo               **out_info,
                      GCancellable             *cancellable,
                      GError                  **error)
{

  g_autoptr(GFileInfo) ret_info = g_file_info_new ();

  g_file_info_set_attribute_uint32 (ret_info, "standard::type",
                                    G_FILE_TYPE_DIRECTORY);

  if (g_file_attribute_matcher_matches (matcher, "unix::mode"))
    {
      g_autoptr(GVariant) metadata = NULL;
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_DIR_META,
                                     metadata_checksum, &metadata, error))
        return FALSE;

      set_info_from_dirmeta (ret_info, metadata);
    }

  ot_transfer_out_value(out_info, &ret_info);
  return TRUE;
}

int
ostree_repo_file_tree_find_child  (OstreeRepoFile  *self,
                                    const char      *name,
                                    gboolean        *is_dir,
                                    GVariant       **out_container)
{
  int i;
  GVariant *files_variant = NULL;
  GVariant *dirs_variant = NULL;
  GVariant *ret_container = NULL;

  files_variant = g_variant_get_child_value (self->tree_contents, 0);
  dirs_variant = g_variant_get_child_value (self->tree_contents, 1);

  i = -1;
  if (ot_variant_bsearch_str (files_variant, name, &i))
    {
      *is_dir = FALSE;
      ret_container = files_variant;
      files_variant = NULL;
    }
  else
    {
      if (ot_variant_bsearch_str (dirs_variant, name, &i))
        {
          *is_dir = TRUE;
          ret_container = dirs_variant;
          dirs_variant = NULL;
        }
      else
        {
          i = -1;
        }
    }
  if (ret_container && out_container)
    {
      *out_container = ret_container;
      ret_container = NULL;
    }
  g_clear_pointer (&ret_container, (GDestroyNotify) g_variant_unref);
  g_clear_pointer (&files_variant, (GDestroyNotify) g_variant_unref);
  g_clear_pointer (&dirs_variant, (GDestroyNotify) g_variant_unref);
  return i;
}

gboolean
ostree_repo_file_tree_query_child (OstreeRepoFile  *self,
                                    int              n,
                                    const char      *attributes,
                                    GFileQueryInfoFlags flags,
                                    GFileInfo      **out_info,
                                    GCancellable    *cancellable,
                                    GError         **error)
{
  gboolean ret = FALSE;
  const char *name = NULL;
  int c;
  g_autoptr(GFileInfo) ret_info = NULL;
  g_autoptr(GVariant) files_variant = NULL;
  g_autoptr(GVariant) dirs_variant = NULL;
  g_autoptr(GVariant) content_csum_v = NULL;
  g_autoptr(GVariant) meta_csum_v = NULL;
  char tmp_checksum[OSTREE_SHA256_STRING_LEN+1];
  GFileAttributeMatcher *matcher = NULL;

  if (!ostree_repo_file_ensure_resolved (self, error))
    goto out;

  matcher = g_file_attribute_matcher_new (attributes);

  g_assert (self->tree_contents);

  files_variant = g_variant_get_child_value (self->tree_contents, 0);
  dirs_variant = g_variant_get_child_value (self->tree_contents, 1);

  c = g_variant_n_children (files_variant);
  if (n < c)
    {
      const guchar *csum_bytes;

      g_variant_get_child (files_variant, n, "(&s@ay)", &name, &content_csum_v);
      csum_bytes = ostree_checksum_bytes_peek_validate (content_csum_v, error);
      if (csum_bytes == NULL)
        goto out;

      ostree_checksum_inplace_from_bytes (csum_bytes, tmp_checksum);

      if (!ostree_repo_load_file (self->repo, tmp_checksum, NULL, &ret_info, NULL,
                                  cancellable, error))
        goto out;
    }
  else
    {
      n -= c;

      c = g_variant_n_children (dirs_variant);

      if (n < c)
        {
          const guchar *csum_bytes;

          g_variant_get_child (dirs_variant, n, "(&s@ay@ay)",
                               &name, NULL, &meta_csum_v);
          csum_bytes = ostree_checksum_bytes_peek_validate (meta_csum_v, error);
          if (csum_bytes == NULL)
            goto out;

          ostree_checksum_inplace_from_bytes (csum_bytes, tmp_checksum);

          if (!query_child_info_dir (self->repo, tmp_checksum,
                                     matcher, flags, &ret_info,
                                     cancellable, error))
            goto out;
        }
    }

  if (name)
    {
      g_file_info_set_attribute_byte_string (ret_info, "standard::name",
                                             name);
      g_file_info_set_attribute_string (ret_info, "standard::display-name",
                                        name);
      if (*name == '.')
        g_file_info_set_is_hidden (ret_info, TRUE);
    }
  else
    {
      g_clear_object (&ret_info);
    }

  ret = TRUE;
  ot_transfer_out_value(out_info, &ret_info);
 out:
  if (matcher)
    g_file_attribute_matcher_unref (matcher);
  return ret;
}

static GFileInfo *
ostree_repo_file_query_info (GFile                *file,
			     const char           *attributes,
			     GFileQueryInfoFlags   flags,
			     GCancellable         *cancellable,
			     GError              **error)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  g_autoptr(GFileInfo) info = NULL;

  if (!ostree_repo_file_ensure_resolved (self, error))
    return NULL;

  if (!self->parent)
    {
      info = g_file_info_new ();
      set_info_from_dirmeta (info, self->tree_metadata);
    }
  else
    {
      if (!ostree_repo_file_tree_query_child (self->parent, self->index,
                                               attributes, flags,
                                               &info, cancellable, error))
        return NULL;
      g_assert (info != NULL);
    }

  return g_steal_pointer (&info);
}

static GFileAttributeInfoList *
ostree_repo_file_query_settable_attributes (GFile         *file,
					GCancellable  *cancellable,
					GError       **error)
{
  return g_file_attribute_info_list_new ();
}

static GFileAttributeInfoList *
ostree_repo_file_query_writable_namespaces (GFile         *file,
					GCancellable  *cancellable,
					GError       **error)
{
  return g_file_attribute_info_list_new ();
}

static GFileInputStream *
ostree_repo_file_read (GFile         *file,
                       GCancellable  *cancellable,
                       GError       **error)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  const char *checksum;
  g_autoptr(GInputStream) ret_stream = NULL;

  if (!ostree_repo_file_ensure_resolved (self, error))
    return FALSE;

  if (self->tree_contents)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                           "Can't open directory");
      return NULL;
    }

  checksum = ostree_repo_file_get_checksum (self);

  g_autoptr(GFileInfo) finfo = NULL;
  if (!ostree_repo_load_file (self->repo, checksum, NULL,
                              &finfo, NULL, cancellable, error))
    return NULL;
  if (g_file_info_get_file_type (finfo) == G_FILE_TYPE_REGULAR)
    {
      if (!ostree_repo_load_file (self->repo, checksum, &ret_stream,
                                  NULL, NULL, cancellable, error))
        return NULL;
    }
  else
    {
      g_autoptr(GFile) parent = g_file_get_parent (file);
      const char *target = g_file_info_get_symlink_target (finfo);
      g_autoptr(GFile) dest = g_file_resolve_relative_path (parent, target);
      return g_file_read (dest, cancellable, error);
    }

  return g_steal_pointer (&ret_stream);
}

static void
ostree_repo_file_file_iface_init (GFileIface *iface)
{
  iface->dup = ostree_repo_file_dup;
  iface->hash = ostree_repo_file_hash;
  iface->equal = ostree_repo_file_equal;
  iface->is_native = ostree_repo_file_is_native;
  iface->has_uri_scheme = ostree_repo_file_has_uri_scheme;
  iface->get_uri_scheme = ostree_repo_file_get_uri_scheme;
  iface->get_basename = ostree_repo_file_get_basename;
  iface->get_path = ostree_repo_file_get_path;
  iface->get_uri = ostree_repo_file_get_uri;
  iface->get_parse_name = ostree_repo_file_get_parse_name;
  iface->get_parent = ostree_repo_file_get_parent;
  iface->prefix_matches = ostree_repo_file_prefix_matches;
  iface->get_relative_path = ostree_repo_file_get_relative_path;
  iface->resolve_relative_path = ostree_repo_file_resolve_relative_path;
  iface->get_child_for_display_name = ostree_repo_file_get_child_for_display_name;
  iface->set_display_name = NULL;
  iface->enumerate_children = ostree_repo_file_enumerate_children;
  iface->query_info = ostree_repo_file_query_info;
  iface->query_filesystem_info = NULL;
  iface->find_enclosing_mount = NULL;
  iface->query_settable_attributes = ostree_repo_file_query_settable_attributes;
  iface->query_writable_namespaces = ostree_repo_file_query_writable_namespaces;
  iface->set_attribute = NULL;
  iface->set_attributes_from_info = NULL;
  iface->read_fn = ostree_repo_file_read;
  iface->append_to = NULL;
  iface->create = NULL;
  iface->replace = NULL;
  iface->open_readwrite = NULL;
  iface->create_readwrite = NULL;
  iface->replace_readwrite = NULL;
  iface->delete_file = NULL;
  iface->trash = NULL;
  iface->make_directory = NULL;
  iface->make_symbolic_link = NULL;
  iface->copy = NULL;
  iface->move = NULL;
  iface->monitor_dir = NULL;
  iface->monitor_file = NULL;

  iface->supports_thread_contexts = TRUE;
}
