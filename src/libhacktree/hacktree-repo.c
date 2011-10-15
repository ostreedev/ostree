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

#include "config.h"

#include "hacktree.h"
#include "htutil.h"

#include <gio/gunixoutputstream.h>

static gboolean
link_one_file (HacktreeRepo *self, const char *path,
               HacktreeObjectType type,
               gboolean ignore_exists, gboolean force,
               GChecksum **out_checksum,
               GError **error);
static char *
get_object_path (HacktreeRepo  *self,
                 const char    *checksum,
                 HacktreeObjectType type);

enum {
  PROP_0,

  PROP_PATH
};

G_DEFINE_TYPE (HacktreeRepo, hacktree_repo, G_TYPE_OBJECT)

#define GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), HACKTREE_TYPE_REPO, HacktreeRepoPrivate))

typedef struct _HacktreeRepoPrivate HacktreeRepoPrivate;

struct _HacktreeRepoPrivate {
  char *path;
  GFile *repo_file;
  char *head_ref_path;
  char *objects_path;

  gboolean inited;
  char *current_head;
};

static void
hacktree_repo_finalize (GObject *object)
{
  HacktreeRepo *self = HACKTREE_REPO (object);
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);

  g_free (priv->path);
  g_clear_object (&priv->repo_file);
  g_free (priv->head_ref_path);
  g_free (priv->objects_path);
  g_free (priv->current_head);

  G_OBJECT_CLASS (hacktree_repo_parent_class)->finalize (object);
}

static void
hacktree_repo_set_property(GObject         *object,
			   guint            prop_id,
			   const GValue    *value,
			   GParamSpec      *pspec)
{
  HacktreeRepo *self = HACKTREE_REPO (object);
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);

  switch (prop_id)
    {
    case PROP_PATH:
      priv->path = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
hacktree_repo_get_property(GObject         *object,
			   guint            prop_id,
			   GValue          *value,
			   GParamSpec      *pspec)
{
  HacktreeRepo *self = HACKTREE_REPO (object);
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_string (value, priv->path);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static GObject *
hacktree_repo_constructor (GType                  gtype,
                           guint                  n_properties,
                           GObjectConstructParam *properties)
{
  GObject *object;
  GObjectClass *parent_class;
  HacktreeRepoPrivate *priv;

  parent_class = G_OBJECT_CLASS (hacktree_repo_parent_class);
  object = parent_class->constructor (gtype, n_properties, properties);

  priv = GET_PRIVATE (object);

  g_assert (priv->path != NULL);
  
  priv->repo_file = g_file_new_for_path (priv->path);
  priv->head_ref_path = g_build_filename (priv->path, HACKTREE_REPO_DIR, "HEAD", NULL);
  priv->objects_path = g_build_filename (priv->path, HACKTREE_REPO_DIR, "objects", NULL);

  return object;
}

static void
hacktree_repo_class_init (HacktreeRepoClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (HacktreeRepoPrivate));

  object_class->constructor = hacktree_repo_constructor;
  object_class->get_property = hacktree_repo_get_property;
  object_class->set_property = hacktree_repo_set_property;
  object_class->finalize = hacktree_repo_finalize;

  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
hacktree_repo_init (HacktreeRepo *self)
{
}

HacktreeRepo*
hacktree_repo_new (const char *path)
{
  return g_object_new (HACKTREE_TYPE_REPO, "path", path, NULL);
}

static gboolean
parse_checksum_file (HacktreeRepo   *self,
                     const char     *path,
                     char          **sha256,
                     GError        **error)
{
  GError *temp_error = NULL;
  gboolean ret = FALSE;
  char *ret_sha256 = NULL;

  ret_sha256 = ht_util_get_file_contents_utf8 (path, &temp_error);
  if (ret_sha256 == NULL)
    {
      if (g_error_matches (temp_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  *sha256 = ret_sha256;
  ret = TRUE;
 out:
  return ret;
}

gboolean
hacktree_repo_check (HacktreeRepo *self, GError **error)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);

  if (priv->inited)
    return TRUE;

  if (!g_file_test (priv->objects_path, G_FILE_TEST_IS_DIR))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't find objects directory '%s'", priv->objects_path);
      return FALSE;
    }
  
  priv->inited = TRUE;

  return parse_checksum_file (self, priv->head_ref_path, &priv->current_head, error);
}

static gboolean
import_gvariant_object (HacktreeRepo  *self,
                        HacktreeSerializedVariantType type,
                        GVariant       *variant,
                        GChecksum    **out_checksum,
                        GError       **error)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);
  GVariant *serialized = NULL;
  gboolean ret = FALSE;
  gsize bytes_written;
  char *tmp_name = NULL;
  int fd = -1;
  GUnixOutputStream *stream = NULL;

  serialized = g_variant_new ("(uv)", (guint32)type, variant);

  tmp_name = g_build_filename (priv->objects_path, "variant-tmp-XXXXXX", NULL);
  fd = mkstemp (tmp_name);
  if (fd < 0)
    {
      ht_util_set_error_from_errno (error, errno);
      goto out;
    }

  stream = (GUnixOutputStream*)g_unix_output_stream_new (fd, FALSE);
  if (!g_output_stream_write_all ((GOutputStream*)stream,
                                  g_variant_get_data (serialized),
                                  g_variant_get_size (serialized),
                                  &bytes_written,
                                  NULL,
                                  error))
    goto out;
  if (!g_output_stream_close ((GOutputStream*)stream,
                              NULL, error))
    goto out;

  if (!link_one_file (self, tmp_name, HACKTREE_OBJECT_TYPE_META, 
                      TRUE, FALSE, out_checksum, error))
    goto out;
  
  ret = TRUE;
 out:
  /* Unconditionally unlink; if we suceeded, there's a new link, if not, clean up. */
  (void) unlink (tmp_name);
  if (fd != -1)
    close (fd);
  if (serialized != NULL)
    g_variant_unref (serialized);
  g_free (tmp_name);
  g_clear_object (&stream);
  return ret;
}

static gboolean
load_gvariant_object_unknown (HacktreeRepo  *self,
                              const char    *sha256,
                              HacktreeSerializedVariantType *out_type,
                              GVariant     **out_variant,
                              GError       **error)
{
  GMappedFile *mfile = NULL;
  gboolean ret = FALSE;
  GVariant *ret_variant = NULL;
  GVariant *container = NULL;
  char *path = NULL;
  guint32 ret_type;

  path = get_object_path (self, sha256, HACKTREE_OBJECT_TYPE_META);
  
  mfile = g_mapped_file_new (path, FALSE, error);
  if (mfile == NULL)
    goto out;
  else
    {
      container = g_variant_new_from_data (G_VARIANT_TYPE (HACKTREE_SERIALIZED_VARIANT_FORMAT),
                                           g_mapped_file_get_contents (mfile),
                                           g_mapped_file_get_length (mfile),
                                           FALSE,
                                           (GDestroyNotify) g_mapped_file_unref,
                                           mfile);
      if (!g_variant_is_of_type (container, G_VARIANT_TYPE (HACKTREE_SERIALIZED_VARIANT_FORMAT)))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Corrupted metadata object '%s'", sha256);
          goto out;
        }
      g_variant_get (container, "(uv)",
                     &ret_type, &ret_variant);
      mfile = NULL;
    }

  ret = TRUE;
 out:
  if (!ret)
    {
      if (ret_variant)
        g_variant_unref (ret_variant);
    }
  else
    {
      *out_type = ret_type;
      *out_variant = ret_variant;
    }
  if (container != NULL)
    g_variant_unref (container);
  g_free (path);
  if (mfile != NULL)
    g_mapped_file_unref (mfile);
  return ret;
}

static gboolean
load_gvariant_object (HacktreeRepo  *self,
                      HacktreeSerializedVariantType expected_type,
                      const char    *sha256, 
                      GVariant     **out_variant,
                      GError       **error)
{
  gboolean ret = FALSE;
  HacktreeSerializedVariantType type;
  GVariant *ret_variant = NULL;

  if (!load_gvariant_object_unknown (self, sha256, &type, &ret_variant, error))
    goto out;

  if (type != expected_type)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted metadata object '%s'; found type %u, expected %u", sha256,
                   type, (guint32)expected_type);
      goto out;
      
    }

  ret = TRUE;
  *out_variant = ret_variant;
 out:
  if (!ret)
    {
      if (ret_variant)
        g_variant_unref (ret_variant);
    }
  return ret;
}

static gboolean
import_directory_meta (HacktreeRepo  *self,
                       const char *path,
                       GVariant  **out_variant,
                       GChecksum **out_checksum,
                       GError    **error)
{
  gboolean ret = FALSE;
  struct stat stbuf;
  GChecksum *ret_checksum = NULL;
  GVariant *dirmeta = NULL;
  char *xattrs = NULL;
  gsize xattr_len;

  if (lstat (path, &stbuf) < 0)
    {
      ht_util_set_error_from_errno (error, errno);
      goto out;
    }
  
  if (!S_ISDIR(stbuf.st_mode))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Not a directory: '%s'", path);
      goto out;
    }

  if (!hacktree_get_xattrs_for_directory (path, &xattrs, &xattr_len, error))
    goto out;

  dirmeta = g_variant_new ("(uuuu@ay)",
                           HACKTREE_DIR_META_VERSION,
                           (guint32)stbuf.st_uid,
                           (guint32)stbuf.st_gid,
                           (guint32)(stbuf.st_mode & ~S_IFMT),
                           g_variant_new_fixed_array (G_VARIANT_TYPE ("y"),
                                                      xattrs, xattr_len, 1));
  g_variant_ref_sink (dirmeta);

  if (!import_gvariant_object (self, HACKTREE_SERIALIZED_DIRMETA_VARIANT, 
                               dirmeta, &ret_checksum, error))
        goto out;

  ret = TRUE;
 out:
  if (!ret)
    {
      if (ret_checksum)
        g_checksum_free (ret_checksum);
      if (dirmeta != NULL)
        g_variant_unref (dirmeta);
    }
  else
    {
      *out_checksum = ret_checksum;
      *out_variant = dirmeta;
    }
  g_free (xattrs);
  return ret;
}

static char *
get_object_path (HacktreeRepo  *self,
                 const char    *checksum,
                 HacktreeObjectType type)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);
  char *checksum_prefix;
  char *base_path;
  char *ret;
  const char *type_string;

  checksum_prefix = g_strndup (checksum, 2);
  base_path = g_build_filename (priv->objects_path, checksum_prefix, checksum + 2, NULL);
  switch (type)
    {
    case HACKTREE_OBJECT_TYPE_FILE:
      type_string = ".file";
      break;
    case HACKTREE_OBJECT_TYPE_META:
      type_string = ".meta";
      break;
    default:
      g_assert_not_reached ();
    }
  ret = g_strconcat (base_path, type_string, NULL);
  g_free (base_path);
  g_free (checksum_prefix);
 
  return ret;
}

static char *
prepare_dir_for_checksum_get_object_path (HacktreeRepo *self,
                                          GChecksum    *checksum,
                                          HacktreeObjectType type,
                                          GError      **error)
{
  char *checksum_dir = NULL;
  char *object_path = NULL;

  object_path = get_object_path (self, g_checksum_get_string (checksum), type);
  checksum_dir = g_path_get_dirname (object_path);

  if (!ht_util_ensure_directory (checksum_dir, FALSE, error))
    goto out;
  
 out:
  g_free (checksum_dir);
  return object_path;
}

static gboolean
link_one_file (HacktreeRepo *self, const char *path, HacktreeObjectType type,
               gboolean ignore_exists, gboolean force,
               GChecksum **out_checksum,
               GError **error)
{
  char *src_basename = NULL;
  char *src_dirname = NULL;
  char *dest_basename = NULL;
  char *tmp_dest_basename = NULL;
  char *dest_dirname = NULL;
  GChecksum *id = NULL;
  DIR *src_dir = NULL;
  DIR *dest_dir = NULL;
  gboolean ret = FALSE;
  struct stat stbuf;
  char *dest_path = NULL;

  src_basename = g_path_get_basename (path);
  src_dirname = g_path_get_dirname (path);

  src_dir = opendir (src_dirname);
  if (src_dir == NULL)
    {
      ht_util_set_error_from_errno (error, errno);
      goto out;
    }

  if (!hacktree_stat_and_checksum_file (dirfd (src_dir), path, &id, &stbuf, error))
    goto out;
  dest_path = prepare_dir_for_checksum_get_object_path (self, id, type, error);
  if (!dest_path)
    goto out;

  dest_basename = g_path_get_basename (dest_path);
  dest_dirname = g_path_get_dirname (dest_path);
  dest_dir = opendir (dest_dirname);
  if (dest_dir == NULL)
    {
      ht_util_set_error_from_errno (error, errno);
      goto out;
    }

  if (force)
    {
      tmp_dest_basename = g_strconcat (dest_basename, ".tmp", NULL);
      (void) unlinkat (dirfd (dest_dir), tmp_dest_basename, 0);
    }
  else
    tmp_dest_basename = g_strdup (dest_basename);
  
  if (linkat (dirfd (src_dir), src_basename, dirfd (dest_dir), tmp_dest_basename, 0) < 0)
    {
      if (errno != EEXIST || !ignore_exists)
        {
          ht_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  if (force)
    {
      if (renameat (dirfd (dest_dir), tmp_dest_basename, 
                    dirfd (dest_dir), dest_basename) < 0)
        {
          ht_util_set_error_from_errno (error, errno);
          goto out;
        }
      (void) unlinkat (dirfd (dest_dir), tmp_dest_basename, 0);
    }

  *out_checksum = id;
  id = NULL;
  ret = TRUE;
 out:
  if (id != NULL)
    g_checksum_free (id);
  if (src_dir != NULL)
    closedir (src_dir);
  if (dest_dir != NULL)
    closedir (dest_dir);
  g_free (src_basename);
  g_free (src_dirname);
  g_free (dest_basename);
  g_free (tmp_dest_basename);
  g_free (dest_dirname);
  return ret;
}

gboolean
hacktree_repo_link_file (HacktreeRepo *self,
                         const char   *path,
                         gboolean      ignore_exists,
                         gboolean      force,
                         GError      **error)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);
  GChecksum *checksum = NULL;

  g_return_val_if_fail (priv->inited, FALSE);

  if (!link_one_file (self, path, HACKTREE_OBJECT_TYPE_FILE,
                      ignore_exists, force, &checksum, error))
    return FALSE;
  g_checksum_free (checksum);
  return TRUE;
}

typedef struct _ParsedTreeData ParsedTreeData;
typedef struct _ParsedDirectoryData ParsedDirectoryData;

static void parsed_tree_data_free (ParsedTreeData *pdata);

struct _ParsedDirectoryData {
  ParsedTreeData *tree_data;
  char *metadata_sha256;
  GVariant *meta_data;
};

static void
parsed_directory_data_free (ParsedDirectoryData *pdata)
{
  if (pdata == NULL)
    return;
  parsed_tree_data_free (pdata->tree_data);
  g_free (pdata->metadata_sha256);
  g_variant_unref (pdata->meta_data);
  g_free (pdata);
}

struct _ParsedTreeData {
  GHashTable *files;  /* char* filename -> char* checksum */
  GHashTable *directories;  /* char* dirname -> ParsedDirectoryData* */
};

static ParsedTreeData *
parsed_tree_data_new (void)
{
  ParsedTreeData *ret = g_new0 (ParsedTreeData, 1);
  ret->files = g_hash_table_new_full (g_str_hash, g_str_equal,
                                      (GDestroyNotify)g_free, 
                                      (GDestroyNotify)g_free);
  ret->directories = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            (GDestroyNotify)g_free, 
                                            (GDestroyNotify)parsed_directory_data_free);
  return ret;
}

static void
parsed_tree_data_free (ParsedTreeData *pdata)
{
  if (pdata == NULL)
    return;
  g_hash_table_destroy (pdata->files);
  g_hash_table_destroy (pdata->directories);
  g_free (pdata);
}

static gboolean
parse_tree (HacktreeRepo    *self,
            const char      *sha256,
            ParsedTreeData **out_pdata,
            GError         **error)
{
  gboolean ret = FALSE;
  ParsedTreeData *ret_pdata = NULL;
  int i, n;
  guint32 version;
  GVariant *tree_variant = NULL;
  GVariant *meta_variant = NULL;
  GVariant *files_variant = NULL;
  GVariant *dirs_variant = NULL;

  if (!load_gvariant_object (self, HACKTREE_SERIALIZED_TREE_VARIANT,
                             sha256, &tree_variant, error))
    goto out;

  g_variant_get (tree_variant, "(u@a{sv}@a(ss)@a(sss))",
                 &version, &meta_variant, &files_variant, &dirs_variant);

  ret_pdata = parsed_tree_data_new ();
  n = g_variant_n_children (files_variant);
  for (i = 0; i < n; i++)
    {
      const char *filename;
      const char *checksum;

      g_variant_get_child (files_variant, i, "(ss)", &filename, &checksum);

      g_hash_table_insert (ret_pdata->files, g_strdup (filename), g_strdup (checksum));
    }

  n = g_variant_n_children (dirs_variant);
  for (i = 0; i < n; i++)
    {
      const char *dirname;
      const char *tree_checksum;
      const char *meta_checksum;
      ParsedTreeData *child_tree = NULL;
      GVariant *metadata = NULL;
      ParsedDirectoryData *child_dir = NULL;

      g_variant_get_child (files_variant, i, "(sss)",
                           &dirname, &tree_checksum, &meta_checksum);
      
      if (!parse_tree (self, tree_checksum, &child_tree, error))
        goto out;

      if (!load_gvariant_object (self, HACKTREE_SERIALIZED_DIRMETA_VARIANT,
                                 meta_checksum, &metadata, error))
        {
          parsed_tree_data_free (child_tree);
          goto out;
        }

      child_dir = g_new0 (ParsedDirectoryData, 1);
      child_dir->tree_data = child_tree;
      child_dir->metadata_sha256 = g_strdup (meta_checksum);
      child_dir->meta_data = g_variant_ref_sink (metadata);

      g_hash_table_insert (ret_pdata->directories, g_strdup (dirname), child_dir);
    }

  ret = TRUE;
 out:
  if (!ret)
    parsed_tree_data_free (ret_pdata);
  else
    *out_pdata = ret_pdata;
  if (tree_variant)
    g_variant_unref (tree_variant);
  if (meta_variant)
    g_variant_unref (meta_variant);
  if (files_variant)
    g_variant_unref (files_variant);
  if (dirs_variant)
    g_variant_unref (dirs_variant);
  return ret;
}

static gboolean
load_commit_and_trees (HacktreeRepo   *self,
                       const char     *commit_sha256,
                       GVariant      **out_commit,
                       ParsedTreeData **out_tree_data, 
                       GError        **error)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);
  GVariant *ret_commit = NULL;
  ParsedTreeData *ret_tree_data = NULL;
  gboolean ret = FALSE;
  const char *tree_checksum;

  if (!priv->current_head)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Can't load current commit; no HEAD reference");
      goto out;
    }

  if (!load_gvariant_object (self, HACKTREE_SERIALIZED_COMMIT_VARIANT,
                             commit_sha256, &ret_commit, error))
    goto out;

  g_variant_get_child (ret_commit, 5, "&s", &tree_checksum);

  if (!parse_tree (self, tree_checksum, &ret_tree_data, error))
    goto out;

  ret = TRUE;
 out:
  if (!ret)
    {
      if (ret_commit)
        g_variant_unref (ret_commit);
      parsed_tree_data_free (ret_tree_data);
    }
  else
    {
      *out_commit = ret_commit;
      *out_tree_data = ret_tree_data;
    }
  return FALSE;
}

static GVariant *
create_empty_gvariant_dict (void)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE("a{sv}"));
  return g_variant_builder_end (&builder);
}

static gboolean
import_parsed_tree (HacktreeRepo    *self,
                    ParsedTreeData  *tree,
                    GChecksum      **out_checksum,
                    GError         **error)
{
  gboolean ret = FALSE;
  GVariant *serialized_tree = NULL;
  gboolean builders_initialized = FALSE;
  GVariantBuilder files_builder;
  GVariantBuilder dirs_builder;
  GHashTableIter hash_iter;
  gpointer key, value;

  g_variant_builder_init (&files_builder, G_VARIANT_TYPE ("a(ss)"));
  g_variant_builder_init (&dirs_builder, G_VARIANT_TYPE ("a(sss)"));
  builders_initialized = TRUE;

  g_hash_table_iter_init (&hash_iter, tree->files);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      const char *checksum = value;

      g_variant_builder_add (&files_builder, "(ss)", name, checksum);
    }

  g_hash_table_iter_init (&hash_iter, tree->directories);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      GChecksum *dir_checksum = NULL;
      ParsedDirectoryData *dir = value;

      if (!import_parsed_tree (self, dir->tree_data, &dir_checksum, error))
        goto out;

      g_variant_builder_add (&dirs_builder, "(sss)",
                             name, g_checksum_get_string (dir_checksum), dir->metadata_sha256);
    }

  serialized_tree = g_variant_new ("(u@a{sv}@a(ss)@a(sss))",
                                   0,
                                   create_empty_gvariant_dict (),
                                   g_variant_builder_end (&files_builder),
                                   g_variant_builder_end (&dirs_builder));
  builders_initialized = FALSE;
  g_variant_ref_sink (serialized_tree);
  if (!import_gvariant_object (self, HACKTREE_SERIALIZED_TREE_VARIANT, serialized_tree, out_checksum, error))
    goto out;
  
  ret = TRUE;
 out:
  if (builders_initialized)
    {
      g_variant_builder_clear (&files_builder);
      g_variant_builder_clear (&dirs_builder);
    }
  if (serialized_tree)
    g_variant_unref (serialized_tree);
  return ret;
}

static gboolean
check_path (const char *filename,
            GError    **error)
{
  gboolean ret = FALSE;

  if (!*filename)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid empty filename");
      goto out;
    }

  if (ht_util_filename_has_dotdot (filename))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Path uplink '..' in filename '%s' not allowed (yet)", filename);
      goto out;
    }
  
  if (g_path_is_absolute (filename))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Absolute filename '%s' not allowed (yet)", filename);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
walk_parsed_tree (HacktreeRepo  *self,
                  const char    *filename,
                  ParsedTreeData *tree,
                  int            *out_filename_index, /* out*/
                  char          **out_component, /* out, but do not free */
                  ParsedTreeData **out_tree, /* out, but do not free */
                  GError        **error)
{
  gboolean ret = FALSE;
  GPtrArray *components = NULL;
  ParsedTreeData *current_tree = tree;
  const char *component = NULL;
  const char *file_sha1;
  ParsedDirectoryData *dir;
  int i;
  int ret_filename_index = 0;

  components = ht_util_path_split (filename);
  g_assert (components != NULL);

  current_tree = tree;
  for (i = 0; i < components->len - 1; i++)
    {
      component = components->pdata[i];
      file_sha1 = g_hash_table_lookup (current_tree->files, component);
      dir = g_hash_table_lookup (current_tree->directories, component);
          
      if (!(file_sha1 || dir))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No such file or directory: %s",
                       filename);
          goto out;
        }
      else if (file_sha1)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Encountered non-directory '%s' in '%s'",
                       (char*)component,
                       filename);
          goto out;
        }
      else if (!dir)
        g_assert_not_reached ();
      current_tree = dir->tree_data;
      ret_filename_index++;
    }

  ret = TRUE;
  g_assert (!(file_sha1 && dir));
  *out_filename_index = i;
  *out_component = components->pdata[i-1];
  *out_tree = current_tree;
 out:
  g_ptr_array_free (components, TRUE);
  return ret;
}

static gboolean
remove_files_from_tree (HacktreeRepo   *self,
                        const char     *base,
                        GPtrArray      *removed_files,
                        ParsedTreeData *tree,
                        GError        **error)
{
  gboolean ret = FALSE;
  int i;

  for (i = 0; i < removed_files->len; i++)
    {
      const char *filename = removed_files->pdata[i];
      int filename_index;
      const char *component;
      ParsedTreeData *parent;
      const char *file_sha1;
      ParsedTreeData *dir;

      if (!check_path (filename, error))
        goto out;
       
      if (!walk_parsed_tree (self, filename, tree,
                             &filename_index, (char**)&component, &parent,
                             error))
        goto out;

      file_sha1 = g_hash_table_lookup (parent->files, component);
      dir = g_hash_table_lookup (parent->directories, component);

      if (file_sha1)
        g_hash_table_remove (parent->files, component);
      else if (dir)
        g_hash_table_remove (parent->directories, component);
      else
        g_assert_not_reached ();
    }
  
  ret = TRUE;
 out:
  return ret;
}

static gboolean
add_one_directory_to_tree_and_import (HacktreeRepo   *self,
                                      const char     *basename,
                                      const char     *abspath,
                                      ParsedTreeData *tree,
                                      ParsedDirectoryData *dir,
                                      GError        **error)
{
  gboolean ret = FALSE;
  GVariant *dirmeta = NULL;
  GChecksum *dir_meta_checksum = NULL;

  g_assert (tree != NULL);

  if (!import_directory_meta (self, abspath, &dirmeta, &dir_meta_checksum, error))
    goto out;

  if (dir)
    {
      g_variant_unref (dir->meta_data);
      dir->meta_data = dirmeta;
    }
  else
    {
      dir = g_new0 (ParsedDirectoryData, 1);
      dir->tree_data = parsed_tree_data_new ();
      dir->metadata_sha256 = g_strdup (g_checksum_get_string (dir_meta_checksum));
      dir->meta_data = dirmeta;
      g_hash_table_insert (tree->directories, g_strdup (basename), dir);
    }

  ret = TRUE;
 out:
  if (dir_meta_checksum)
    g_checksum_free (dir_meta_checksum);
  return ret;
}

static gboolean
add_one_file_to_tree_and_import (HacktreeRepo   *self,
                                 const char     *basename,
                                 const char     *abspath,
                                 ParsedTreeData *tree,
                                 GError        **error)
{
  gboolean ret = FALSE;
  GChecksum *checksum = NULL;
  
  g_assert (tree != NULL);

  if (!link_one_file (self, abspath, HACKTREE_OBJECT_TYPE_FILE,
                      TRUE, FALSE, &checksum, error))
    goto out;

  g_hash_table_replace (tree->files, g_strdup (basename),
                        g_strdup (g_checksum_get_string (checksum)));

  ret = TRUE;
 out:
  if (checksum)
    g_checksum_free (checksum);
  return ret;
}

static gboolean
add_one_path_to_tree_and_import (HacktreeRepo   *self,
                                 const char     *base,
                                 const char     *filename,
                                 ParsedTreeData *tree,
                                 GError        **error)
{
  gboolean ret = FALSE;
  GPtrArray *components = NULL;
  struct stat stbuf;
  char *component_abspath = NULL;
  ParsedTreeData *current_tree = tree;
  const char *component = NULL;
  const char *file_sha1;
  char *abspath = NULL;
  ParsedDirectoryData *dir;
  int i;
  gboolean is_directory;
      
  if (!check_path (filename, error))
    goto out;

  abspath = g_build_filename (base, filename, NULL);

  if (lstat (abspath, &stbuf) < 0)
    {
      ht_util_set_error_from_errno (error, errno);
      goto out;
    }
  is_directory = S_ISDIR(stbuf.st_mode);
       
  if (components)
    g_ptr_array_free (components, TRUE);
  components = ht_util_path_split (filename);
  g_assert (components->len > 0);

  current_tree = tree;
  for (i = 0; i < components->len; i++)
    {
      component = components->pdata[i];
      g_free (component_abspath);
      component_abspath = ht_util_path_join_n (base, components, i);
      file_sha1 = g_hash_table_lookup (current_tree->files, component);
      dir = g_hash_table_lookup (current_tree->directories, component);
          
      if (i < components->len - 1)
        {
          if (file_sha1 != NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Encountered non-directory '%s' in '%s'",
                           component,
                           filename);
              goto out;
            }
          /* Implicitly add intermediate directories */
          if (!add_one_directory_to_tree_and_import (self, component,
                                                     abspath, current_tree, dir,
                                                     error))
            goto out;
        }
      else if (is_directory)
        {
          if (file_sha1 != NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "File '%s' can't be overwritten by directory",
                           filename);
              goto out;
            }
          if (!add_one_directory_to_tree_and_import (self, component,
                                                     abspath, current_tree, dir,
                                                     error))
            goto out;
        }
      else 
        {
          g_assert (!is_directory);
          if (dir != NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "File '%s' can't be overwritten by directory",
                           filename);
              goto out;
            }
          if (!add_one_file_to_tree_and_import (self, component, abspath,
                                                current_tree, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  g_free (component_abspath);
  g_free (abspath);
  return ret;
}

static gboolean
add_files_to_tree_and_import (HacktreeRepo   *self,
                              const char     *base,
                              GPtrArray      *added_files,
                              ParsedTreeData *tree,
                              GError        **error)
{
  gboolean ret = FALSE;
  int i;

  for (i = 0; i < added_files->len; i++)
    {
      const char *path = added_files->pdata[i];

      if (!add_one_path_to_tree_and_import (self, base, path, tree, error))
        goto out;
    }
  
  ret = TRUE;
 out:
  return ret;
}

gboolean
hacktree_repo_commit (HacktreeRepo *self,
                      const char   *subject,
                      const char   *body,
                      GVariant     *metadata,
                      const char   *base,
                      GPtrArray    *modified_files,
                      GPtrArray    *removed_files,
                      GChecksum   **out_commit,
                      GError      **error)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  ParsedTreeData *tree = NULL;
  GVariant *previous_commit = NULL;
  GVariant *commit = NULL;
  GChecksum *root_checksum = NULL;
  GChecksum *ret_commit_checksum = NULL;
  GDateTime *now = NULL;

  g_return_val_if_fail (priv->inited, FALSE);

  if (priv->current_head)
    {
      if (!load_commit_and_trees (self, priv->current_head, &previous_commit, &tree, error))
        goto out;
    }
  else
    {
      /* Initial commit */
      tree = parsed_tree_data_new ();
    }

  if (!remove_files_from_tree (self, base, removed_files, tree, error))
    goto out;

  if (!add_files_to_tree_and_import (self, base, modified_files, tree, error))
    goto out;
  
  if (!import_parsed_tree (self, tree, &root_checksum, error))
    goto out;

  now = g_date_time_new_now_utc ();
  commit = g_variant_new ("(u@a{sv}sssts)",
                          HACKTREE_COMMIT_VERSION,
                          create_empty_gvariant_dict (),
                          priv->current_head ? priv->current_head : "",
                          subject, body,
                          g_date_time_to_unix (now) / G_TIME_SPAN_SECOND,
                          g_checksum_get_string (root_checksum));
  if (!import_gvariant_object (self, HACKTREE_SERIALIZED_COMMIT_VARIANT,
                               commit, &ret_commit_checksum, error))
    goto out;

  ret = TRUE;
 out:
  if (!ret)
    {
      if (ret_commit_checksum)
        g_checksum_free (ret_commit_checksum);
    }
  else
    {
      *out_commit = ret_commit_checksum;
    }
  if (root_checksum)
    g_checksum_free (root_checksum);
  if (previous_commit)
    g_variant_unref (previous_commit);
  parsed_tree_data_free (tree);
  if (commit)
    g_variant_unref (commit);
  if (now)
    g_date_time_unref (now);
  return ret;
}

static gboolean
iter_object_dir (HacktreeRepo   *self,
                 GFile          *dir,
                 HacktreeRepoObjectIter  callback,
                 gpointer                user_data,
                 GError                **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  GFileEnumerator *enumerator = NULL;
  GFileInfo *file_info = NULL;
  char *dirpath = NULL;

  dirpath = g_file_get_path (dir);

  enumerator = g_file_enumerate_children (dir, "standard::name,standard::type,unix::*", 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          NULL, 
                                          error);
  if (!enumerator)
    goto out;
  
  while ((file_info = g_file_enumerator_next_file (enumerator, NULL, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;
      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");
      
      if (type != G_FILE_TYPE_DIRECTORY
          && (g_str_has_suffix (name, ".meta")
              || g_str_has_suffix (name, ".file")))
        {
          char *dot;
          char *path;
          
          dot = strrchr (name, '.');
          g_assert (dot);
          
          if ((dot - name) == 62)
            {
              path = g_build_filename (dirpath, name, NULL);
              callback (self, path, file_info, user_data);
              g_free (path);
            }
        }

      g_object_unref (file_info);
    }
  if (file_info == NULL && temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  if (!g_file_enumerator_close (enumerator, NULL, error))
    goto out;

  ret = TRUE;
 out:
  g_free (dirpath);
  return ret;
}

gboolean
hacktree_repo_iter_objects (HacktreeRepo  *self,
                            HacktreeRepoObjectIter callback,
                            gpointer       user_data,
                            GError        **error)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);
  GFile *objectdir = NULL;
  GFileEnumerator *enumerator = NULL;
  gboolean ret = FALSE;
  GFileInfo *file_info = NULL;
  GError *temp_error = NULL;

  g_return_val_if_fail (priv->inited, FALSE);

  objectdir = g_file_new_for_path (priv->objects_path);
  enumerator = g_file_enumerate_children (objectdir, "standard::name,standard::type,unix::*", 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          NULL, 
                                          error);
  if (!enumerator)
    goto out;

  while ((file_info = g_file_enumerator_next_file (enumerator, NULL, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");
      
      if (strlen (name) == 2 && type == G_FILE_TYPE_DIRECTORY)
        {
          GFile *objdir = g_file_get_child (objectdir, name);
          if (!iter_object_dir (self, objdir, callback, user_data, error))
            {
              g_object_unref (objdir);
              goto out;
            }
          g_object_unref (objdir);
        }
      g_object_unref (file_info);
    }
  if (file_info == NULL && temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  if (!g_file_enumerator_close (enumerator, NULL, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_object (&file_info);
  g_clear_object (&enumerator);
  g_clear_object (&objectdir);
  return ret;
}

gboolean
hacktree_repo_load_variant (HacktreeRepo *repo,
                            const char   *sha256,
                            HacktreeSerializedVariantType *out_type,
                            GVariant    **out_variant,
                            GError      **error)
{
  gboolean ret = FALSE;
  HacktreeSerializedVariantType ret_type;
  GVariant *ret_variant = NULL;
  
  if (!load_gvariant_object_unknown (repo, sha256, &ret_type, &ret_variant, error))
    goto out;

  ret = TRUE;
  *out_type = ret_type;
  *out_variant = ret_variant;
 out:
  if (!ret)
    {
      if (ret_variant)
        g_variant_unref (ret_variant);
    }
  return ret;
}

const char *
hacktree_repo_get_head (HacktreeRepo  *self)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);

  g_return_val_if_fail (priv->inited, NULL);

  return priv->current_head;
}
