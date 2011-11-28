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

#include "ostree-repo-file-enumerator.h"

static void ostree_repo_file_file_iface_init (GFileIface *iface);

static void
tree_replace_contents (OstreeRepoFile  *self,
                       GVariant        *new_files,
                       GVariant        *new_dirs);

struct _OstreeRepoFile
{
  GObject parent_instance;

  OstreeRepo *repo;

  char *commit;
  GError *commit_resolve_error;
  
  OstreeRepoFile *parent;
  int index;
  char *name;

  char *tree_contents_checksum;
  GVariant *tree_contents;
  char *tree_metadata_checksum;
  GVariant *tree_metadata;
};

#define ostree_repo_file_get_type _ostree_repo_file_get_type
G_DEFINE_TYPE_WITH_CODE (OstreeRepoFile, ostree_repo_file, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						ostree_repo_file_file_iface_init))

static void
ostree_repo_file_finalize (GObject *object)
{
  OstreeRepoFile *self;

  self = OSTREE_REPO_FILE (object);

  ot_clear_gvariant (&self->tree_contents);
  ot_clear_gvariant (&self->tree_metadata);
  g_free (self->tree_contents_checksum);
  g_free (self->tree_metadata_checksum);
  g_free (self->commit);
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
               ot_gfile_get_path_cached (self));
  return FALSE;
}

GFile * 
_ostree_repo_file_new_root (OstreeRepo  *repo,
                            const char  *commit)
{
  OstreeRepoFile *self;

  g_return_val_if_fail (repo != NULL, NULL);
  g_return_val_if_fail (commit != NULL, NULL);
  g_return_val_if_fail (strlen (commit) == 64, NULL);

  self = g_object_new (OSTREE_TYPE_REPO_FILE, NULL);
  self->repo = g_object_ref (repo);
  self->commit = g_strdup (commit);

  return G_FILE (self);
}


GFile *
_ostree_repo_file_new_child (OstreeRepoFile *parent,
                             const char  *name)
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

  return G_FILE (self);
}

OstreeRepoFile *
_ostree_repo_file_new_empty_tree (OstreeRepo  *repo)
{
  OstreeRepoFile *self;
  
  self = g_object_new (OSTREE_TYPE_REPO_FILE, NULL);
  self->repo = g_object_ref (repo);

  tree_replace_contents (self, NULL, NULL);

  return self;
}

static gboolean
do_resolve_commit (OstreeRepoFile  *self,
                   GError         **error)
{
  gboolean ret = FALSE;
  GVariant *commit = NULL;
  GVariant *root_contents = NULL;
  GVariant *root_metadata = NULL;
  const char *tree_contents_checksum;
  const char *tree_meta_checksum;

  g_assert (self->parent == NULL);

  if (!ostree_repo_load_variant_checked (self->repo, OSTREE_SERIALIZED_COMMIT_VARIANT,
                                         self->commit, &commit, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  g_variant_get_child (commit, 6, "&s", &tree_contents_checksum);
  g_variant_get_child (commit, 7, "&s", &tree_meta_checksum);

  if (!ostree_repo_load_variant_checked (self->repo, OSTREE_SERIALIZED_TREE_VARIANT,
                                         tree_contents_checksum, &root_contents,
					 error))
    goto out;

  if (!ostree_repo_load_variant_checked (self->repo, OSTREE_SERIALIZED_DIRMETA_VARIANT,
                                         tree_meta_checksum, &root_metadata,
					 error))
    goto out;
  
  self->tree_metadata = root_metadata;
  root_metadata = NULL;
  self->tree_contents = root_contents;
  root_contents = NULL;
  self->tree_contents_checksum = g_strdup (tree_contents_checksum);
  self->tree_metadata_checksum = g_strdup (tree_meta_checksum);

  ret = TRUE;
 out:
  ot_clear_gvariant (&commit);
  ot_clear_gvariant (&root_metadata);
  ot_clear_gvariant (&root_contents);
  return ret;
}

static gboolean
do_resolve_nonroot (OstreeRepoFile     *self,
                    GError            **error)
{
  gboolean ret = FALSE;
  GVariant *container = NULL;
  GVariant *tree_contents = NULL;
  GVariant *tree_metadata = NULL;
  gboolean is_dir;
  int i;

  i = _ostree_repo_file_tree_find_child (self->parent, self->name, &is_dir, &container);
  
  if (i < 0)
    {
      set_error_noent ((GFile*)self, error);
      goto out;
    }

  if (is_dir)
    {
      const char *name;
      const char *content_checksum;
      const char *metadata_checksum;
      GVariant *files_variant;

      files_variant = g_variant_get_child_value (self->parent->tree_contents, 2);
      self->index = g_variant_n_children (files_variant) + i;
      ot_clear_gvariant (&files_variant);

      g_variant_get_child (container, i, "(&s&s&s)",
                           &name, &content_checksum, &metadata_checksum);

      if (!ot_util_validate_file_name (name, error))
        goto out;
          
      if (!ostree_repo_load_variant_checked (self->repo, OSTREE_SERIALIZED_TREE_VARIANT,
                                             content_checksum, &tree_contents,
                                             error))
        goto out;
          
      if (!ostree_repo_load_variant_checked (self->repo, OSTREE_SERIALIZED_DIRMETA_VARIANT,
                                             metadata_checksum, &tree_metadata,
                                             error))
        goto out;

      self->tree_contents = tree_contents;
      tree_contents = NULL;
      self->tree_metadata = tree_metadata;
      tree_metadata = NULL;
      self->tree_contents_checksum = g_strdup (content_checksum);
      self->tree_metadata_checksum = g_strdup (metadata_checksum);
    }
  else
    self->index = i;

  ret = TRUE;
 out:
  ot_clear_gvariant (&container);
  ot_clear_gvariant (&tree_metadata);
  ot_clear_gvariant (&tree_contents);
  return ret;
}

gboolean
_ostree_repo_file_ensure_resolved (OstreeRepoFile  *self,
                                   GError         **error)
{
  if (self->commit_resolve_error != NULL)
    goto out;

  if (self->parent == NULL)
    {
      if (self->tree_contents == NULL)
        (void)do_resolve_commit (self, &(self->commit_resolve_error));
    }
  else if (self->index == -1)
    {
      if (!_ostree_repo_file_ensure_resolved (self->parent, error))
        goto out;
      (void)do_resolve_nonroot (self, &(self->commit_resolve_error));
    }
  
 out:
  if (self->commit_resolve_error)
    {
      if (error)
	*error = g_error_copy (self->commit_resolve_error);
      return FALSE;
    }
  else
    return TRUE;
}

gboolean
_ostree_repo_file_get_xattrs (OstreeRepoFile  *self,
                              GVariant       **out_xattrs,
                              GCancellable    *cancellable,
                              GError         **error)
{
  gboolean ret = FALSE;
  GVariant *ret_xattrs = NULL;
  GVariant *metadata = NULL;
  GInputStream *input = NULL;
  GFile *local_file = NULL;

  if (!_ostree_repo_file_ensure_resolved (self, error))
    goto out;

  if (self->tree_metadata)
    ret_xattrs = g_variant_get_child_value (self->tree_metadata, 4);
  else if (ostree_repo_is_archive (self->repo))
    {
      local_file = _ostree_repo_file_nontree_get_local (self);
      if (!ostree_parse_packed_file (local_file, &metadata, &input, cancellable, error))
        goto out;
      ret_xattrs = g_variant_get_child_value (metadata, 4);
    }
  else
    {
      local_file = _ostree_repo_file_nontree_get_local (self);
      ret_xattrs = ostree_get_xattrs_for_file (local_file, error);
    }

  ret = TRUE;
  *out_xattrs = ret_xattrs;
  ret_xattrs = NULL;
 out:
  ot_clear_gvariant (&ret_xattrs);
  ot_clear_gvariant (&metadata);
  g_clear_object (&input);
  g_clear_object (&local_file);
  return ret;
}

GVariant *
_ostree_repo_file_tree_get_contents (OstreeRepoFile  *self)
{
  return self->tree_contents;
}

GVariant *
_ostree_repo_file_tree_get_metadata (OstreeRepoFile  *self)
{
  return self->tree_metadata;
}

void
_ostree_repo_file_tree_set_metadata (OstreeRepoFile *self,
                                     const char     *checksum,
                                     GVariant       *metadata)
{
  ot_clear_gvariant (&self->tree_metadata);
  self->tree_metadata = g_variant_ref (metadata);
  g_free (self->tree_metadata_checksum);
  self->tree_metadata_checksum = g_strdup (checksum);
}

void
_ostree_repo_file_make_empty_tree (OstreeRepoFile  *self)
{
  tree_replace_contents (self, NULL, NULL);
}

void
_ostree_repo_file_tree_set_content_checksum (OstreeRepoFile  *self,
                                             const char      *checksum)
{
  g_assert (self->parent == NULL);
  g_free (self->tree_contents_checksum);
  self->tree_contents_checksum = g_strdup (checksum);
}

const char *
_ostree_repo_file_tree_get_content_checksum (OstreeRepoFile  *self)
{
  return self->tree_contents_checksum;
}

GFile *
_ostree_repo_file_nontree_get_local (OstreeRepoFile  *self)
{
  return ostree_repo_get_object_path (self->repo, _ostree_repo_file_get_checksum (self), OSTREE_OBJECT_TYPE_FILE);
}

OstreeRepo *
_ostree_repo_file_get_repo (OstreeRepoFile  *self)
{
  return self->repo;
}

OstreeRepoFile *
_ostree_repo_file_get_root (OstreeRepoFile  *self)
{
  OstreeRepoFile *parent = self;

  while (parent->parent)
    parent = parent->parent;
  return parent;
}

const char *
_ostree_repo_file_get_checksum (OstreeRepoFile  *self)
{
  int n;
  gboolean is_dir;
  GVariant *files_variant;
  GVariant *dirs_variant;
  const char *checksum;

  if (!self->parent)
    return self->tree_metadata_checksum;

  n = _ostree_repo_file_tree_find_child (self->parent, self->name, &is_dir, NULL);
  g_assert (n >= 0);

  files_variant = g_variant_get_child_value (self->parent->tree_contents, 2);
  dirs_variant = g_variant_get_child_value (self->parent->tree_contents, 3);

  if (is_dir)
    {
      g_variant_get_child (dirs_variant, n,
                           "(@s@s&s)", NULL, NULL, &checksum);
    }
  else
    {
      g_variant_get_child (files_variant, n,
                           "(@s&s)", NULL, &checksum);
    }
  ot_clear_gvariant (&files_variant);
  ot_clear_gvariant (&dirs_variant);

  return checksum;
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

  path = ot_gfile_get_path_cached (file);
  uri_path = g_filename_to_uri (path, NULL, NULL);
  g_assert (g_str_has_prefix (uri_path, "file://"));
  ret = g_strconcat ("ostree://", self->commit, uri_path+strlen("file://"), NULL);
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
    return _ostree_repo_file_new_child (self->parent, self->name);
  else
    return _ostree_repo_file_new_root (self->repo, self->commit);
}

static guint
ostree_repo_file_hash (GFile *file)
{
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);
  
  if (self->parent)
    return g_file_hash (self->parent) + g_str_hash (self->name);
  else
    return g_str_hash (self->commit);
}

static gboolean
ostree_repo_file_equal (GFile *file1,
                        GFile *file2)
{
  OstreeRepoFile *self1 = OSTREE_REPO_FILE (file1);
  OstreeRepoFile *self2 = OSTREE_REPO_FILE (file2);

  if (self1->parent && self2->parent)
    {
      return g_str_equal (self1->name, self2->name)
        && g_file_equal ((GFile*)self1->parent, (GFile*)self2->parent);
    }
  else if (!self1->parent && !self2->parent)
    {
      return g_str_equal (self1->commit, self2->commit);
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

  parent_path = ot_gfile_get_path_cached (parent);
  descendant_path = ot_gfile_get_path_cached (descendant);
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

  parent_path = ot_gfile_get_path_cached (parent);
  descendant_path = ot_gfile_get_path_cached (descendant);
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
        return g_object_ref (_ostree_repo_file_get_root (self)); 

      if (self->parent)
        return ostree_repo_file_resolve_relative_path ((GFile*)_ostree_repo_file_get_root (self),
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

  parent = (OstreeRepoFile*)_ostree_repo_file_new_child (self, filename);
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

static GFile *
get_child_local_file (OstreeRepo   *repo,
                      const char   *checksum)
{
  return ostree_repo_get_object_path (repo, checksum, OSTREE_OBJECT_TYPE_FILE);
}

static gboolean
query_child_info_file_nonarchive (OstreeRepo       *repo,
                                  const char       *checksum,
                                  GFileAttributeMatcher *matcher,
                                  GFileInfo        *info,
                                  GCancellable     *cancellable,
                                  GError          **error)
{
  gboolean ret = FALSE;
  GFileInfo *local_info = NULL;
  GFile *local_file = NULL;
  int i ;
  const char *mapped_boolean[] = {
    "standard::is-symlink"
  };
  const char *mapped_string[] = {
  };
  const char *mapped_byte_string[] = {
    "standard::symlink-target"
  };
  const char *mapped_uint32[] = {
    "standard::type",
    "unix::device",
    "unix::mode",
    "unix::nlink",
    "unix::uid",
    "unix::gid",
    "unix::rdev"
  };
  const char *mapped_uint64[] = {
    "standard::size",
    "standard::allocated-size",
    "unix::inode"
  };

  if (!(g_file_attribute_matcher_matches (matcher, "unix::mode")
        || g_file_attribute_matcher_matches (matcher, "standard::type")))
    {
      ret = TRUE;
      goto out;
    }

  local_file = get_child_local_file (repo, checksum);
  local_info = g_file_query_info (local_file,
				  OSTREE_GIO_FAST_QUERYINFO,
				  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				  cancellable,
				  error);
  if (!local_info)
    goto out;

  for (i = 0; i < G_N_ELEMENTS (mapped_boolean); i++)
    g_file_info_set_attribute_boolean (info, mapped_boolean[i], g_file_info_get_attribute_boolean (local_info, mapped_boolean[i]));

  for (i = 0; i < G_N_ELEMENTS (mapped_string); i++)
    {
      const char *string = g_file_info_get_attribute_string (local_info, mapped_string[i]);
      if (string)
        g_file_info_set_attribute_string (info, mapped_string[i], string);
    }

  for (i = 0; i < G_N_ELEMENTS (mapped_byte_string); i++)
    {
      const char *byte_string = g_file_info_get_attribute_byte_string (local_info, mapped_byte_string[i]);
      if (byte_string)
        g_file_info_set_attribute_byte_string (info, mapped_byte_string[i], byte_string);
    }

  for (i = 0; i < G_N_ELEMENTS (mapped_uint32); i++)
    g_file_info_set_attribute_uint32 (info, mapped_uint32[i], g_file_info_get_attribute_uint32 (local_info, mapped_uint32[i]));

  for (i = 0; i < G_N_ELEMENTS (mapped_uint64); i++)
    g_file_info_set_attribute_uint64 (info, mapped_uint64[i], g_file_info_get_attribute_uint64 (local_info, mapped_uint64[i]));
  
  ret = TRUE;
 out:
  g_clear_object (&local_info);
  g_clear_object (&local_file);
  return ret;
}

static gboolean
query_child_info_file_archive (OstreeRepo       *repo,
                               const char       *checksum,
                               GFileAttributeMatcher *matcher,
                               GFileInfo        *info,
                               GCancellable     *cancellable,
                               GError          **error)
{
  gboolean ret = FALSE;
  GFile *local_file = NULL;
  GVariant *metadata = NULL;
  GInputStream *input = NULL;
  guint32 version, uid, gid, mode;
  guint64 content_len;
  guint32 file_type;
  gsize bytes_read;
  char *buf = NULL;

  local_file = get_child_local_file (repo, checksum);

  if (!ostree_parse_packed_file (local_file, &metadata, &input, cancellable, error))
    goto out;

  g_variant_get (metadata, "(uuuu@a(ayay)t)",
                 &version, &uid, &gid, &mode,
                 NULL, &content_len);
  uid = GUINT32_FROM_BE (uid);
  gid = GUINT32_FROM_BE (gid);
  mode = GUINT32_FROM_BE (mode);
  content_len = GUINT64_FROM_BE (content_len);

  g_file_info_set_attribute_boolean (info, "standard::is-symlink",
                                     S_ISLNK (mode));
  if (S_ISLNK (mode))
    file_type = G_FILE_TYPE_SYMBOLIC_LINK;
  else if (S_ISREG (mode))
    file_type = G_FILE_TYPE_REGULAR;
  else if (S_ISBLK (mode) || S_ISCHR(mode) || S_ISFIFO(mode))
    file_type = G_FILE_TYPE_SPECIAL;
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted packfile %s: Invalid mode", checksum);
      goto out;
    }
  g_file_info_set_attribute_uint32 (info, "standard::type", file_type);

  g_file_info_set_attribute_uint32 (info, "unix::uid", uid);
  g_file_info_set_attribute_uint32 (info, "unix::gid", gid);
  g_file_info_set_attribute_uint32 (info, "unix::mode", mode);

  if (file_type == G_FILE_TYPE_REGULAR)
    {
      g_file_info_set_attribute_uint64 (info, "standard::size", content_len);
    }
  else if (file_type == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      gsize len = MIN (PATH_MAX, content_len) + 1;
      buf = g_malloc (len);

      if (!g_input_stream_read_all (input, buf, len, &bytes_read, cancellable, error))
        goto out;
      buf[bytes_read] = '\0';

      g_file_info_set_attribute_byte_string (info, "standard::symlink-target", buf);
    }
  else if (file_type == G_FILE_TYPE_SPECIAL)
    {
      guint32 device;

      if (!g_input_stream_read_all (input, &device, 4, &bytes_read, cancellable, error))
        goto out;

      device = GUINT32_FROM_BE (device);
      g_file_info_set_attribute_uint32 (info, "unix::device", device);
    }

  ret = TRUE;
 out:
  g_free (buf);
  ot_clear_gvariant (&metadata);
  g_clear_object (&local_file);
  g_clear_object (&input);
  return ret;
}

static void
set_info_from_dirmeta (GFileInfo  *info,
                       GVariant   *metadata)
{
  guint32 version, uid, gid, mode;

  g_file_info_set_attribute_uint32 (info, "standard::type", G_FILE_TYPE_DIRECTORY);

  /* PARSE OSTREE_SERIALIZED_DIRMETA_VARIANT */
  g_variant_get (metadata, "(uuuu@a(ayay))",
                 &version, &uid, &gid, &mode,
                 NULL);
  version = GUINT32_FROM_BE (version);
  uid = GUINT32_FROM_BE (uid);
  gid = GUINT32_FROM_BE (gid);
  mode = GUINT32_FROM_BE (mode);

  g_file_info_set_attribute_uint32 (info, "unix::uid", uid);
  g_file_info_set_attribute_uint32 (info, "unix::gid", gid);
  g_file_info_set_attribute_uint32 (info, "unix::mode", mode);
}

static gboolean
query_child_info_dir (OstreeRepo         *repo,
                      const char         *metadata_checksum,
                      GFileAttributeMatcher *matcher,
                      GFileQueryInfoFlags flags,
                      GFileInfo        *info,
                      GCancellable    *cancellable,
                      GError         **error)
{
  gboolean ret = FALSE;
  GVariant *metadata = NULL;

  if (!g_file_attribute_matcher_matches (matcher, "unix::mode"))
    {
      ret = TRUE;
      goto out;
    }

  if (!ostree_repo_load_variant_checked (repo, OSTREE_SERIALIZED_DIRMETA_VARIANT,
                                         metadata_checksum, &metadata, error))
    goto out;

  set_info_from_dirmeta (info, metadata);
  
  ret = TRUE;
 out:
  ot_clear_gvariant (&metadata);
  return ret;
}

static gboolean
bsearch_in_file_variant (GVariant  *variant,
                         const char *name,
                         int        *out_pos)
{
  int i, n;
  int m;

  i = 0;
  n = g_variant_n_children (variant) - 1;
  m = 0;

  while (i <= n)
    {
      GVariant *child;
      const char *cur;
      int cmp;

      m = i + ((n - i) / 2);

      child = g_variant_get_child_value (variant, m);
      g_variant_get_child (child, 0, "&s", &cur, NULL);      

      cmp = strcmp (cur, name);
      if (cmp < 0)
        i = m + 1;
      else if (cmp > 0)
        n = m - 1;
      else
        {
          ot_clear_gvariant (&child);
          *out_pos = m;
          return TRUE;
        }
      ot_clear_gvariant (&child);
    }

  *out_pos = m;
  return FALSE;
}

static GVariant *
remove_variant_child (GVariant *variant,
                      int       n)
{
  GVariantBuilder builder;
  GVariantIter *iter;
  int i;
  GVariant *child;

  g_variant_builder_init (&builder, g_variant_get_type (variant));
  iter = g_variant_iter_new (variant);

  i = 0;
  while ((child = g_variant_iter_next_value (iter)) != NULL)
    {
      if (i != n)
        g_variant_builder_add_value (&builder, child);
      ot_clear_gvariant (&child);
    }
  g_variant_iter_free (iter);
  
  return g_variant_builder_end (&builder);
}

static GVariant *
insert_variant_child (GVariant  *variant,
                      int        n,
                      GVariant  *item)
{
  GVariantBuilder builder;
  GVariantIter *iter;
  int i;
  GVariant *child;

  g_variant_builder_init (&builder, g_variant_get_type (variant));
  iter = g_variant_iter_new (variant);

  i = 0;
  while ((child = g_variant_iter_next_value (iter)) != NULL)
    {
      if (i == n)
        g_variant_builder_add_value (&builder, item);
      g_variant_builder_add_value (&builder, child);
      ot_clear_gvariant (&child);
    }
  g_variant_iter_free (iter);
  
  return g_variant_builder_end (&builder);
}

static void
tree_replace_contents (OstreeRepoFile  *self,
                       GVariant        *new_files,
                       GVariant        *new_dirs)
{
  guint version;
  GVariant *metadata;
  GVariant *tmp_files = NULL;
  GVariant *tmp_dirs = NULL;
  
  if (!(new_files || new_dirs) && self->tree_contents)
    return;
  else if (!self->tree_contents)
    {
      version = GUINT32_TO_BE (0);
      metadata = g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0);
      tmp_dirs = g_variant_new_array (G_VARIANT_TYPE ("(ss)"), NULL, 0);
      tmp_files = g_variant_new_array (G_VARIANT_TYPE ("(ss)"), NULL, 0);
    }
  else
    {
      g_variant_get_child (self->tree_contents, 0, "u", &version);
      metadata = g_variant_get_child_value (self->tree_contents, 1);
      if (!new_files)
        tmp_files = g_variant_get_child_value (self->tree_contents, 2);
      if (!new_dirs)
        tmp_dirs = g_variant_get_child_value (self->tree_contents, 3);
    }

  ot_clear_gvariant (&self->tree_contents);
  self->tree_contents = g_variant_new ("(u@a{sv}@a(ss)@a(sss))", version, metadata,
                                       new_files ? new_files : tmp_files,
                                       new_dirs ? new_dirs : tmp_dirs);

  ot_clear_gvariant (&tmp_files);
  ot_clear_gvariant (&tmp_dirs);
}

void
_ostree_repo_file_tree_remove_child (OstreeRepoFile  *self,
                                     const char      *name)
{
  int i;
  GVariant *files_variant;
  GVariant *new_files_variant = NULL;
  GVariant *dirs_variant;
  GVariant *new_dirs_variant = NULL;

  files_variant = g_variant_get_child_value (self->tree_contents, 2);
  dirs_variant = g_variant_get_child_value (self->tree_contents, 3);

  if (bsearch_in_file_variant (files_variant, name, &i))
    {
      new_files_variant = remove_variant_child (files_variant, i);
    }
  else
    {
      if (bsearch_in_file_variant (dirs_variant, name, &i))
        {
          new_dirs_variant = remove_variant_child (dirs_variant, i);
        }
    }

  tree_replace_contents (self, new_files_variant, new_dirs_variant);

  ot_clear_gvariant (&files_variant);
  ot_clear_gvariant (&dirs_variant);
}

void
_ostree_repo_file_tree_add_file (OstreeRepoFile  *self,
                                 const char      *name,
                                 const char      *checksum)
{
  int n;
  GVariant *files_variant;
  GVariant *new_files_variant;

  files_variant = g_variant_get_child_value (self->tree_contents, 2);

  if (!bsearch_in_file_variant (files_variant, name, &n))
    {
      new_files_variant = insert_variant_child (files_variant, n,
                                                g_variant_new ("(ss)", name, checksum));
      g_variant_ref_sink (new_files_variant);
      tree_replace_contents (self, new_files_variant, NULL);
      ot_clear_gvariant (&new_files_variant);
    }
  ot_clear_gvariant (&files_variant);
}

void
_ostree_repo_file_tree_add_dir (OstreeRepoFile  *self,
                                const char      *name,
                                const char      *content_checksum,
                                const char      *metadata_checksum)
{
  int n;
  GVariant *dirs_variant;
  GVariant *new_dirs_variant;

  dirs_variant = g_variant_get_child_value (self->tree_contents, 3);

  if (!bsearch_in_file_variant (dirs_variant, name, &n))
    {
      new_dirs_variant = insert_variant_child (dirs_variant, n,
                                               g_variant_new ("(sss)", name, content_checksum,
                                                              metadata_checksum));
      g_variant_ref_sink (new_dirs_variant);
      tree_replace_contents (self, NULL, new_dirs_variant);
      ot_clear_gvariant (&new_dirs_variant);
    }
  ot_clear_gvariant (&dirs_variant);
}

int
_ostree_repo_file_tree_find_child  (OstreeRepoFile  *self,
                                    const char      *name,
                                    gboolean        *is_dir,
                                    GVariant       **out_container)
{
  int i;
  GVariant *files_variant = NULL;
  GVariant *dirs_variant = NULL;
  GVariant *ret_container = NULL;

  files_variant = g_variant_get_child_value (self->tree_contents, 2);
  dirs_variant = g_variant_get_child_value (self->tree_contents, 3);

  i = -1;
  if (bsearch_in_file_variant (files_variant, name, &i))
    {
      *is_dir = FALSE;
      ret_container = files_variant;
      files_variant = NULL;
    }
  else
    {
      if (bsearch_in_file_variant (dirs_variant, name, &i))
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
  ot_clear_gvariant (&ret_container);
  ot_clear_gvariant (&files_variant);
  ot_clear_gvariant (&dirs_variant);
  return i;
}

gboolean
_ostree_repo_file_tree_query_child (OstreeRepoFile  *self,
                                    int              n,
                                    const char      *attributes,
                                    GFileQueryInfoFlags flags,
                                    GFileInfo      **out_info,
                                    GCancellable    *cancellable,
                                    GError         **error)
{
  const char *name = NULL;
  gboolean ret = FALSE;
  GFileInfo *ret_info = NULL;
  GVariant *files_variant = NULL;
  GVariant *dirs_variant = NULL;
  GVariant *tree_child_metadata = NULL;
  GFileAttributeMatcher *matcher = NULL;
  int c;

  if (!_ostree_repo_file_ensure_resolved (self, error))
    goto out;

  matcher = g_file_attribute_matcher_new (attributes);

  ret_info = g_file_info_new ();

  g_assert (self->tree_contents);

  files_variant = g_variant_get_child_value (self->tree_contents, 2);
  dirs_variant = g_variant_get_child_value (self->tree_contents, 3);

  c = g_variant_n_children (files_variant);
  if (n < c)
    {
      const char *checksum;

      g_variant_get_child (files_variant, n, "(&s&s)", &name, &checksum);

      if (ostree_repo_is_archive (self->repo))
	{
	  if (!query_child_info_file_archive (self->repo, checksum, matcher, ret_info,
                                              cancellable, error))
	    goto out;
	}
      else
	{
	  if (!query_child_info_file_nonarchive (self->repo, checksum, matcher, ret_info,
                                                 cancellable, error))
	    goto out;
	}
    }
  else
    {
      const char *tree_checksum;
      const char *meta_checksum;

      n -= c;

      c = g_variant_n_children (dirs_variant);

      if (n < c)
        {
          g_variant_get_child (dirs_variant, n, "(&s&s&s)",
                               &name, &tree_checksum, &meta_checksum);

          if (!query_child_info_dir (self->repo, meta_checksum,
                                     matcher, flags, ret_info,
                                     cancellable, error))
            goto out;
        }
      else
        n -= c;
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
  *out_info = ret_info;
  ret_info = NULL;
 out:
  g_clear_object (&ret_info);
  if (matcher)
    g_file_attribute_matcher_unref (matcher);
  ot_clear_gvariant (&tree_child_metadata);
  ot_clear_gvariant (&files_variant);
  ot_clear_gvariant (&dirs_variant);
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
  gboolean ret = FALSE;
  GFileInfo *info = NULL;

  if (!_ostree_repo_file_ensure_resolved (self, error))
    goto out;

  if (!self->parent)
    {
      info = g_file_info_new ();
      set_info_from_dirmeta (info, self->tree_metadata);
    }
  else
    {
      if (!_ostree_repo_file_tree_query_child (self->parent, self->index, 
                                               attributes, flags, 
                                               &info, cancellable, error))
        goto out;
    }
      
  ret = TRUE;
 out:
  if (!ret)
    g_clear_object (&info);
  return info;
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
  gboolean ret = FALSE;
  GFile *local_file = NULL;
  GFileInputStream *ret_stream = NULL;
  OstreeRepoFile *self = OSTREE_REPO_FILE (file);

  if (self->tree_contents)
    {
      g_set_error_literal (error, G_IO_ERROR,
			   G_IO_ERROR_IS_DIRECTORY,
			   "Can't open directory");
      goto out;
    }

  if (ostree_repo_is_archive (self->repo))
    {
      g_set_error_literal (error, G_IO_ERROR,
			   G_IO_ERROR_NOT_SUPPORTED,
			   "Can't open archived file (yet)");
      goto out;
    }
  else
    {
      local_file = _ostree_repo_file_nontree_get_local (self);
      ret_stream = g_file_read (local_file, cancellable, error);
      if (!ret_stream)
	goto out;
    }
  
  ret = TRUE;
 out:
  g_clear_object (&local_file);
  if (!ret)
    g_clear_object (&ret_stream);
  return ret_stream;
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
