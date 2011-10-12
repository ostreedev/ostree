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
  char *objects_path;

  gboolean inited;
};

static void
hacktree_repo_finalize (GObject *object)
{
  HacktreeRepo *self = HACKTREE_REPO (object);
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);

  g_free (priv->path);
  g_free (priv->objects_path);

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
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);
  
}

HacktreeRepo*
hacktree_repo_new (const char *path)
{
  return g_object_new (HACKTREE_TYPE_REPO, "path", path, NULL);
}

gboolean
hacktree_repo_check (HacktreeRepo *self, GError **error)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);

  if (!g_file_test (priv->objects_path, G_FILE_TEST_IS_DIR))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't find objects directory '%s'", priv->objects_path);
      return FALSE;
    }
  
  priv->inited = TRUE;
  return TRUE;
}


static char *
prepare_dir_for_checksum_get_object_path (HacktreeRepo *self,
                                          GChecksum    *checksum,
                                          GError      **error)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);
  char *checksum_prefix = NULL;
  char *checksum_dir = NULL;
  char *object_path = NULL;
  GError *temp_error = NULL;

  checksum_prefix = g_strndup (g_checksum_get_string (checksum), 2);
  g_assert_cmpuint (strlen (checksum_prefix), ==, 2);
  checksum_dir = g_build_filename (priv->objects_path, checksum_prefix, NULL);

  if (!ht_util_ensure_directory (checksum_dir, FALSE, error))
    goto out;
  
  object_path = g_build_filename (checksum_dir, g_checksum_get_string (checksum) + 2, NULL);
 out:
  g_free (checksum_prefix);
  g_free (checksum_dir);
  return object_path;
}

static gboolean
link_one_file (HacktreeRepo *self, const char *path,
               gboolean ignore_exists, gboolean force, GError **error)
{
  HacktreeRepoPrivate *priv = GET_PRIVATE (self);
  char *src_basename = NULL;
  char *src_dirname = NULL;
  char *dest_basename = NULL;
  char *tmp_dest_basename = NULL;
  char *dest_dirname = NULL;
  GChecksum *id = NULL;
  DIR *src_dir = NULL;
  DIR *dest_dir = NULL;
  gboolean ret = FALSE;
  int fd;
  struct stat stbuf;
  char *dest_path = NULL;
  char *checksum_prefix;

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
  dest_path = prepare_dir_for_checksum_get_object_path (self, id, error);
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

  g_return_val_if_fail (priv->inited, FALSE);

  return link_one_file (self, path, ignore_exists, force, error);
}

static gboolean
iter_object_dir (HacktreeRepo   *repo,
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

  enumerator = g_file_enumerate_children (dir, "standard::*,unix::*", 
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
      
      /* 64 - 2 */
      if (strlen (name) == 62 && type != G_FILE_TYPE_DIRECTORY)
        {
          char *path = g_build_filename (dirpath, name, NULL);
          callback (repo, path, file_info, user_data);
          g_free (path);
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
  enumerator = g_file_enumerate_children (objectdir, "standard::*,unix::*", 
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
