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

#define _GNU_SOURCE

#include "config.h"

#include "ostree.h"
#include "otutil.h"
#include "ostree-repo-file-enumerator.h"

#include <gio/gunixoutputstream.h>
#include <gio/gunixinputstream.h>

enum {
  PROP_0,

  PROP_PATH
};

G_DEFINE_TYPE (OstreeRepo, ostree_repo, G_TYPE_OBJECT)

#define GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), OSTREE_TYPE_REPO, OstreeRepoPrivate))

typedef struct _OstreeRepoPrivate OstreeRepoPrivate;

struct _OstreeRepoPrivate {
  char *path;
  GFile *repo_file;
  GFile *tmp_dir;
  GFile *local_heads_dir;
  GFile *remote_heads_dir;
  char *objects_path;
  char *config_path;

  gboolean inited;

  GKeyFile *config;
  gboolean archive;
};

static void
ostree_repo_finalize (GObject *object)
{
  OstreeRepo *self = OSTREE_REPO (object);
  OstreeRepoPrivate *priv = GET_PRIVATE (self);

  g_free (priv->path);
  g_clear_object (&priv->repo_file);
  g_clear_object (&priv->tmp_dir);
  g_clear_object (&priv->local_heads_dir);
  g_clear_object (&priv->remote_heads_dir);
  g_free (priv->objects_path);
  g_free (priv->config_path);
  if (priv->config)
    g_key_file_free (priv->config);

  G_OBJECT_CLASS (ostree_repo_parent_class)->finalize (object);
}

static void
ostree_repo_set_property(GObject         *object,
			   guint            prop_id,
			   const GValue    *value,
			   GParamSpec      *pspec)
{
  OstreeRepo *self = OSTREE_REPO (object);
  OstreeRepoPrivate *priv = GET_PRIVATE (self);

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
ostree_repo_get_property(GObject         *object,
			   guint            prop_id,
			   GValue          *value,
			   GParamSpec      *pspec)
{
  OstreeRepo *self = OSTREE_REPO (object);
  OstreeRepoPrivate *priv = GET_PRIVATE (self);

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
ostree_repo_constructor (GType                  gtype,
                           guint                  n_properties,
                           GObjectConstructParam *properties)
{
  GObject *object;
  GObjectClass *parent_class;
  OstreeRepoPrivate *priv;

  parent_class = G_OBJECT_CLASS (ostree_repo_parent_class);
  object = parent_class->constructor (gtype, n_properties, properties);

  priv = GET_PRIVATE (object);

  g_assert (priv->path != NULL);
  
  priv->repo_file = ot_util_new_file_for_path (priv->path);
  priv->tmp_dir = g_file_resolve_relative_path (priv->repo_file, "tmp");
  priv->local_heads_dir = g_file_resolve_relative_path (priv->repo_file, "refs/heads");
  priv->remote_heads_dir = g_file_resolve_relative_path (priv->repo_file, "refs/remotes");
  
  priv->objects_path = g_build_filename (priv->path, "objects", NULL);
  priv->config_path = g_build_filename (priv->path, "config", NULL);

  return object;
}

static void
ostree_repo_class_init (OstreeRepoClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (OstreeRepoPrivate));

  object_class->constructor = ostree_repo_constructor;
  object_class->get_property = ostree_repo_get_property;
  object_class->set_property = ostree_repo_set_property;
  object_class->finalize = ostree_repo_finalize;

  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
ostree_repo_init (OstreeRepo *self)
{
}

OstreeRepo*
ostree_repo_new (const char *path)
{
  return g_object_new (OSTREE_TYPE_REPO, "path", path, NULL);
}

static gboolean
parse_rev_file (OstreeRepo     *self,
                const char     *path,
                char          **sha256,
                GError        **error) G_GNUC_UNUSED;

static gboolean
parse_rev_file (OstreeRepo     *self,
                const char     *path,
                char          **sha256,
                GError        **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GError *temp_error = NULL;
  gboolean ret = FALSE;
  char *rev = NULL;

  rev = ot_util_get_file_contents_utf8 (path, &temp_error);
  if (rev == NULL)
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
  else
    {
      g_strchomp (rev);
    }

  if (g_str_has_prefix (rev, "ref: "))
    {
      GFile *ref;
      const char *ref_path;
      char *ref_sha256;
      gboolean subret;

      ref = g_file_resolve_relative_path (priv->local_heads_dir, rev + 5);
      ref_path = ot_gfile_get_path_cached (ref);

      subret = parse_rev_file (self, ref_path, &ref_sha256, error);
      g_clear_object (&ref);
        
      if (!subret)
        {
          g_free (ref_sha256);
          goto out;
        }
      
      g_free (rev);
      rev = ref_sha256;
    }
  else 
    {
      if (!ostree_validate_checksum_string (rev, error))
        goto out;
    }

  *sha256 = rev;
  rev = NULL;
  ret = TRUE;
 out:
  g_free (rev);
  return ret;
}

gboolean
ostree_repo_resolve_rev (OstreeRepo     *self,
                         const char     *rev,
                         gboolean        allow_noent,
                         char          **sha256,
                         GError        **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  char *tmp = NULL;
  char *tmp2 = NULL;
  char *ret_rev = NULL;
  GFile *child = NULL;
  GFile *origindir = NULL;
  const char *child_path = NULL;
  GError *temp_error = NULL;
  GVariant *commit = NULL;

  g_return_val_if_fail (rev != NULL, FALSE);

  if (strlen (rev) == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid empty rev");
      goto out;
    }
  else if (strstr (rev, "..") != NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid rev %s", rev);
      goto out;
    }
  else if (strlen (rev) == 64)
    {
      ret_rev = g_strdup (rev);
    }
  else if (g_str_has_suffix (rev, "^"))
    {
      tmp = g_strdup (rev);
      tmp[strlen(tmp) - 1] = '\0';

      if (!ostree_repo_resolve_rev (self, tmp, allow_noent, &tmp2, error))
        goto out;

      if (!ostree_repo_load_variant_checked (self, OSTREE_SERIALIZED_COMMIT_VARIANT, tmp2, &commit, error))
        goto out;
      
      g_variant_get_child (commit, 2, "s", &ret_rev);
      if (strlen (ret_rev) == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Commit %s has no parent", tmp2);
          goto out;

        }
    }
  else
    {
      const char *slash = strchr (rev, '/');
      if (slash != NULL && (slash == rev || !*(slash+1)))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid rev %s", rev);
          goto out;
        }
      else if (slash == NULL)
        {
          child = g_file_get_child (priv->local_heads_dir, rev);
          child_path = ot_gfile_get_path_cached (child);
        }
      else
        {
          const char *rest = slash + 1;

          if (strchr (rest, '/'))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid rev %s", rev);
              goto out;
            }
          
          child = g_file_get_child (priv->remote_heads_dir, rev);
          child_path = ot_gfile_get_path_cached (child);

        }
      if (!ot_util_gfile_load_contents_utf8 (child, NULL, &ret_rev, NULL, &temp_error))
        {
          if (allow_noent && g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&temp_error);
              g_free (ret_rev);
              ret_rev = NULL;
            }
          else
            {
              g_propagate_error (error, temp_error);
              g_prefix_error (error, "Couldn't open ref '%s': ", child_path);
              goto out;
            }
        }
      else
        {
          g_strchomp (ret_rev);
         
          if (!ostree_validate_checksum_string (ret_rev, error))
            goto out;
        }
    }

  *sha256 = ret_rev;
  ret_rev = NULL;
  ret = TRUE;
 out:
  if (commit)
    g_variant_unref (commit);
  g_free (tmp);
  g_free (tmp2);
  g_clear_object (&child);
  g_clear_object (&origindir);
  g_free (ret_rev);
  return ret;
}

static gboolean
write_checksum_file (GFile *parentdir,
                     const char *name,
                     const char *sha256,
                     GError **error)
{
  gboolean ret = FALSE;
  GFile *child = NULL;
  GOutputStream *out = NULL;
  gsize bytes_written;

  child = g_file_get_child (parentdir, name);

  if ((out = (GOutputStream*)g_file_replace (child, NULL, FALSE, 0, NULL, error)) == NULL)
    goto out;
  if (!g_output_stream_write_all (out, sha256, strlen (sha256), &bytes_written, NULL, error))
    goto out;
  if (!g_output_stream_write_all (out, "\n", 1, &bytes_written, NULL, error))
    goto out;
  if (!g_output_stream_close (out, NULL, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_object (&child);
  g_clear_object (&out);
  return ret;
}

/**
 * ostree_repo_get_config:
 * @self:
 *
 * Returns: (transfer none): The repository configuration; do not modify
 */
GKeyFile *
ostree_repo_get_config (OstreeRepo *self)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);

  g_return_val_if_fail (priv->inited, NULL);

  return priv->config;
}

/**
 * ostree_repo_copy_config:
 * @self:
 *
 * Returns: (transfer full): A newly-allocated copy of the repository config
 */
GKeyFile *
ostree_repo_copy_config (OstreeRepo *self)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GKeyFile *copy;
  char *data;
  gsize len;

  g_return_val_if_fail (priv->inited, NULL);

  copy = g_key_file_new ();
  data = g_key_file_to_data (priv->config, &len, NULL);
  if (!g_key_file_load_from_data (copy, data, len, 0, NULL))
    g_assert_not_reached ();
  g_free (data);
  return copy;
}

/**
 * ostree_repo_write_config:
 * @self:
 * @new_config: Overwrite the config file with this data.  Do not change later!
 * @error: a #GError
 *
 * Save @new_config in place of this repository's config file.  Note
 * that @new_config should not be modified after - this function
 * simply adds a reference.
 */
gboolean
ostree_repo_write_config (OstreeRepo *self,
                          GKeyFile   *new_config,
                          GError    **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  char *data = NULL;
  gsize len;
  gboolean ret = FALSE;

  g_return_val_if_fail (priv->inited, FALSE);

  data = g_key_file_to_data (new_config, &len, error);
  if (!g_file_set_contents (priv->config_path, data, len, error))
    goto out;
  
  g_key_file_free (priv->config);
  priv->config = g_key_file_new ();
  if (!g_key_file_load_from_data (priv->config, data, len, 0, error))
    goto out;

  ret = TRUE;
 out:
  g_free (data);
  return ret;
}

gboolean
ostree_repo_check (OstreeRepo *self, GError **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  char *version = NULL;;
  GError *temp_error = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (priv->inited)
    return TRUE;

  if (!g_file_test (priv->objects_path, G_FILE_TEST_IS_DIR))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't find objects directory '%s'", priv->objects_path);
      goto out;
    }
  
  priv->config = g_key_file_new ();
  if (!g_key_file_load_from_file (priv->config, priv->config_path, 0, error))
    {
      g_prefix_error (error, "Couldn't parse config file: ");
      goto out;
    }

  version = g_key_file_get_value (priv->config, "core", "repo_version", &temp_error);
  if (temp_error)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  if (strcmp (version, "0") != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid repository version '%s'", version);
      goto out;
    }

  priv->archive = g_key_file_get_boolean (priv->config, "core", "archive", &temp_error);
  if (temp_error)
    {
      if (g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  priv->inited = TRUE;
  
  ret = TRUE;
 out:
  g_free (version);
  return ret;
}

const char *
ostree_repo_get_path (OstreeRepo  *self)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  return priv->path;
}

gboolean      
ostree_repo_is_archive (OstreeRepo  *self)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);

  g_return_val_if_fail (priv->inited, FALSE);

  return priv->archive;
}

static GVariant *
pack_metadata_variant (OstreeSerializedVariantType type,
                       GVariant                   *variant)
{
  return g_variant_new ("(uv)", GUINT32_TO_BE ((guint32)type), variant);
}

static gboolean
write_gvariant_to_tmp (OstreeRepo  *self,
                       OstreeSerializedVariantType type,
                       GVariant    *variant,
                       GFile        **out_tmpname,
                       GChecksum    **out_checksum,
                       GError       **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GVariant *serialized = NULL;
  gboolean ret = FALSE;
  gsize bytes_written;
  char *tmp_name = NULL;
  char *dest_name = NULL;
  int fd = -1;
  GUnixOutputStream *stream = NULL;
  GChecksum *checksum = NULL;

  serialized = pack_metadata_variant (type, variant);

  tmp_name = g_build_filename (ot_gfile_get_path_cached (priv->tmp_dir), "variant-tmp-XXXXXX", NULL);
  fd = g_mkstemp (tmp_name);
  if (fd < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  checksum = g_checksum_new (G_CHECKSUM_SHA256);

  stream = (GUnixOutputStream*)g_unix_output_stream_new (fd, FALSE);
  if (!g_output_stream_write_all ((GOutputStream*)stream,
                                  g_variant_get_data (serialized),
                                  g_variant_get_size (serialized),
                                  &bytes_written,
                                  NULL,
                                  error))
    goto out;

  g_checksum_update (checksum, (guint8*)g_variant_get_data (serialized), g_variant_get_size (serialized));

  if (!g_output_stream_close ((GOutputStream*)stream,
                              NULL, error))
    goto out;

  dest_name = g_build_filename (ot_gfile_get_path_cached (priv->tmp_dir), g_checksum_get_string (checksum), NULL);
  if (rename (tmp_name, dest_name) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  ret = TRUE;
  *out_tmpname = ot_util_new_file_for_path (dest_name);
  *out_checksum = checksum;
  checksum = NULL;
 out:
  /* Unconditionally unlink; if we suceeded, there's a new link, if not, clean up. */
  (void) unlink (tmp_name);
  if (fd != -1)
    close (fd);
  if (checksum)
    g_checksum_free (checksum);
  if (serialized != NULL)
    g_variant_unref (serialized);
  g_free (tmp_name);
  g_free (dest_name);
  g_clear_object (&stream);
  return ret;
}

static gboolean
import_gvariant_object (OstreeRepo  *self,
                        OstreeSerializedVariantType type,
                        GVariant       *variant,
                        GChecksum    **out_checksum,
                        GError       **error)
{
  gboolean ret = FALSE;
  GFile *tmp_path = NULL;
  GChecksum *ret_checksum = NULL;
  gboolean did_exist;
  
  if (!write_gvariant_to_tmp (self, type, variant, &tmp_path, &ret_checksum, error))
    goto out;

  if (!ostree_repo_store_object_trusted (self, ot_gfile_get_path_cached (tmp_path),
                                         g_checksum_get_string (ret_checksum),
                                         OSTREE_OBJECT_TYPE_META,
                                         TRUE, FALSE, &did_exist, error))
    goto out;

  ret = TRUE;
  *out_checksum = ret_checksum;
  ret_checksum = NULL;
 out:
  (void) g_file_delete (tmp_path, NULL, NULL);
  g_clear_object (&tmp_path);
  if (ret_checksum)
    g_checksum_free (ret_checksum);
  return ret;
}

gboolean
ostree_repo_load_variant_checked (OstreeRepo  *self,
                                  OstreeSerializedVariantType expected_type,
                                  const char    *sha256, 
                                  GVariant     **out_variant,
                                  GError       **error)
{
  gboolean ret = FALSE;
  OstreeSerializedVariantType type;
  GVariant *ret_variant = NULL;

  if (!ostree_repo_load_variant (self, sha256, &type, &ret_variant, error))
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
  ret_variant = NULL;
 out:
  if (ret_variant)
    g_variant_unref (ret_variant);
  return ret;
}

static gboolean
import_directory_meta (OstreeRepo  *self,
                       const char *path,
                       GVariant  **out_variant,
                       GChecksum **out_checksum,
                       GError    **error)
{
  gboolean ret = FALSE;
  GChecksum *ret_checksum = NULL;
  GVariant *dirmeta = NULL;
  GFile *f = NULL;

  f = ot_util_new_file_for_path (path);

  if (!ostree_get_directory_metadata (f, &dirmeta, NULL, error))
    goto out;
  
  if (!import_gvariant_object (self, OSTREE_SERIALIZED_DIRMETA_VARIANT, 
                               dirmeta, &ret_checksum, error))
    goto out;

  ret = TRUE;
  *out_variant = dirmeta;
  dirmeta = NULL;
  *out_checksum = ret_checksum;
  ret_checksum = NULL;
 out:
  g_clear_object (&f);
  if (ret_checksum)
    g_checksum_free (ret_checksum);
  if (dirmeta != NULL)
    g_variant_unref (dirmeta);
  return ret;
}

char *
ostree_repo_get_object_path (OstreeRepo  *self,
                             const char    *checksum,
                             OstreeObjectType type)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  char *ret;
  char *relpath;

  relpath = ostree_get_relative_object_path (checksum, type, priv->archive);
  ret = g_build_filename (priv->path, relpath, NULL);
  g_free (relpath);
 
  return ret;
}

static char *
prepare_dir_for_checksum_get_object_path (OstreeRepo *self,
                                          const char   *checksum,
                                          OstreeObjectType type,
                                          GError      **error)
{
  char *checksum_dir = NULL;
  char *object_path = NULL;

  object_path = ostree_repo_get_object_path (self, checksum, type);
  checksum_dir = g_path_get_dirname (object_path);

  if (!ot_util_ensure_directory (checksum_dir, FALSE, error))
    goto out;
  
 out:
  g_free (checksum_dir);
  return object_path;
}

static gboolean
link_object_trusted (OstreeRepo   *self,
                     const char   *path,
                     const char   *checksum,
                     OstreeObjectType objtype,
                     gboolean      ignore_exists,
                     gboolean      force,
                     gboolean     *did_exist,
                     GError      **error)
{
  char *src_basename = NULL;
  char *src_dirname = NULL;
  char *dest_basename = NULL;
  char *tmp_dest_basename = NULL;
  char *dest_dirname = NULL;
  DIR *src_dir = NULL;
  DIR *dest_dir = NULL;
  gboolean ret = FALSE;
  char *dest_path = NULL;

  src_basename = g_path_get_basename (path);
  src_dirname = g_path_get_dirname (path);

  src_dir = opendir (src_dirname);
  if (src_dir == NULL)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  dest_path = prepare_dir_for_checksum_get_object_path (self, checksum, objtype, error);
  if (!dest_path)
    goto out;

  dest_basename = g_path_get_basename (dest_path);
  dest_dirname = g_path_get_dirname (dest_path);
  dest_dir = opendir (dest_dirname);
  if (dest_dir == NULL)
    {
      ot_util_set_error_from_errno (error, errno);
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
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
      else
        *did_exist = TRUE;
    }
  else
    *did_exist = FALSE;

  if (force)
    {
      if (renameat (dirfd (dest_dir), tmp_dest_basename, 
                    dirfd (dest_dir), dest_basename) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
      (void) unlinkat (dirfd (dest_dir), tmp_dest_basename, 0);
    }

  ret = TRUE;
 out:
  if (src_dir != NULL)
    closedir (src_dir);
  if (dest_dir != NULL)
    closedir (dest_dir);
  g_free (src_basename);
  g_free (src_dirname);
  g_free (dest_basename);
  g_free (tmp_dest_basename);
  g_free (dest_dirname);
  g_free (dest_path);
  return ret;
}

static gboolean
archive_file_trusted (OstreeRepo   *self,
                      const char   *path,
                      const char   *checksum,
                      OstreeObjectType objtype,
                      gboolean      ignore_exists,
                      gboolean      force,
                      gboolean     *did_exist,
                      GError      **error)
{
  GFile *infile = NULL;
  GFile *outfile = NULL;
  GFileOutputStream *out = NULL;
  gboolean ret = FALSE;
  char *dest_path = NULL;
  char *dest_tmp_path = NULL;

  infile = ot_util_new_file_for_path (path);

  dest_path = prepare_dir_for_checksum_get_object_path (self, checksum, objtype, error);
  if (!dest_path)
    goto out;

  dest_tmp_path = g_strconcat (dest_path, ".tmp", NULL);

  outfile = ot_util_new_file_for_path (dest_tmp_path);
  out = g_file_replace (outfile, NULL, FALSE, 0, NULL, error);
  if (!out)
    goto out;

  if (!ostree_pack_object ((GOutputStream*)out, infile, objtype, NULL, error))
    goto out;
  
  if (!g_output_stream_close ((GOutputStream*)out, NULL, error))
    goto out;

  if (rename (dest_tmp_path, dest_path) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  ret = TRUE;
 out:
  g_free (dest_path);
  g_free (dest_tmp_path);
  g_clear_object (&infile);
  g_clear_object (&outfile);
  g_clear_object (&out);
  return ret;
}
  
gboolean      
ostree_repo_store_object_trusted (OstreeRepo   *self,
                                  const char   *path,
                                  const char   *checksum,
                                  OstreeObjectType objtype,
                                  gboolean      ignore_exists,
                                  gboolean      force,
                                  gboolean     *did_exist,
                                  GError      **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  if (priv->archive && objtype == OSTREE_OBJECT_TYPE_FILE)
    return archive_file_trusted (self, path, checksum, objtype, ignore_exists, force, did_exist, error);
  else
    return link_object_trusted (self, path, checksum, objtype, ignore_exists, force, did_exist, error);
}

gboolean
ostree_repo_store_packfile (OstreeRepo       *self,
                            const char       *expected_checksum,
                            const char       *path,
                            OstreeObjectType  objtype,
                            gboolean         *did_exist,
                            GError          **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  GString *tempfile_path = NULL;
  GChecksum *checksum = NULL;

  tempfile_path = g_string_new (priv->path);
  g_string_append_printf (tempfile_path, "/tmp-unpack-%s", expected_checksum);
  
  if (!ostree_unpack_object (path, objtype, tempfile_path->str, &checksum, error))
    goto out;

  if (strcmp (g_checksum_get_string (checksum), expected_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted object %s (actual checksum is %s)",
                   expected_checksum, g_checksum_get_string (checksum));
      goto out;
    }

  if (!ostree_repo_store_object_trusted (self, tempfile_path ? tempfile_path->str : path,
                                         expected_checksum,
                                         objtype,
                                         TRUE, FALSE, did_exist, error))
    goto out;

  ret = TRUE;
 out:
  if (tempfile_path)
    {
      (void) unlink (tempfile_path->str);
      g_string_free (tempfile_path, TRUE);
    }
  if (checksum)
    g_checksum_free (checksum);
  return ret;
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

static GVariant *
create_empty_gvariant_dict (void)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE("a{sv}"));
  return g_variant_builder_end (&builder);
}

static gboolean
import_parsed_tree (OstreeRepo    *self,
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
  GSList *sorted_filenames = NULL;
  GSList *iter;
  gpointer key, value;

  g_variant_builder_init (&files_builder, G_VARIANT_TYPE ("a(ss)"));
  g_variant_builder_init (&dirs_builder, G_VARIANT_TYPE ("a(sss)"));
  builders_initialized = TRUE;

  g_hash_table_iter_init (&hash_iter, tree->files);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      sorted_filenames = g_slist_prepend (sorted_filenames, (char*)name);
    }

  sorted_filenames = g_slist_sort (sorted_filenames, (GCompareFunc)strcmp);

  for (iter = sorted_filenames; iter; iter = iter->next)
    {
      const char *name = iter->data;
      const char *value;

      value = g_hash_table_lookup (tree->files, name);
      g_variant_builder_add (&files_builder, "(ss)", name, value);
    }

  g_slist_free (sorted_filenames);
  sorted_filenames = NULL;

  g_hash_table_iter_init (&hash_iter, tree->directories);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      sorted_filenames = g_slist_prepend (sorted_filenames, (char*)name);
    }

  sorted_filenames = g_slist_sort (sorted_filenames, (GCompareFunc)strcmp);

  for (iter = sorted_filenames; iter; iter = iter->next)
    {
      const char *name = iter->data;
      GChecksum *dir_checksum = NULL;
      ParsedDirectoryData *dir;

      dir = g_hash_table_lookup (tree->directories, name);

      if (!import_parsed_tree (self, dir->tree_data, &dir_checksum, error))
        goto out;

      g_variant_builder_add (&dirs_builder, "(sss)",
                             name, g_checksum_get_string (dir_checksum), dir->metadata_sha256);
      g_checksum_free (dir_checksum);
    }

  g_slist_free (sorted_filenames);
  sorted_filenames = NULL;

  serialized_tree = g_variant_new ("(u@a{sv}@a(ss)@a(sss))",
                                   GUINT32_TO_BE (0),
                                   create_empty_gvariant_dict (),
                                   g_variant_builder_end (&files_builder),
                                   g_variant_builder_end (&dirs_builder));
  builders_initialized = FALSE;
  g_variant_ref_sink (serialized_tree);
  if (!import_gvariant_object (self, OSTREE_SERIALIZED_TREE_VARIANT, serialized_tree, out_checksum, error))
    goto out;
  
  ret = TRUE;
 out:
  g_slist_free (sorted_filenames);
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

  if (strcmp (filename, ".") == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Self-reference '.' in filename '%s' not allowed (yet)", filename);
      goto out;
    }
  
  if (ot_util_filename_has_dotdot (filename))
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
add_one_directory_to_tree_and_import (OstreeRepo   *self,
                                      const char     *basename,
                                      const char     *abspath,
                                      ParsedTreeData *tree,
                                      ParsedDirectoryData **dir, /*inout*/
                                      GError        **error)
{
  gboolean ret = FALSE;
  GVariant *dirmeta = NULL;
  GChecksum *dir_meta_checksum = NULL;
  ParsedDirectoryData *dir_value = *dir;
  
  g_assert (tree != NULL);

  if (!import_directory_meta (self, abspath, &dirmeta, &dir_meta_checksum, error))
    goto out;

  if (dir_value)
    {
      g_variant_unref (dir_value->meta_data);
      dir_value->meta_data = dirmeta;
    }
  else
    {
      dir_value = g_new0 (ParsedDirectoryData, 1);
      dir_value->tree_data = parsed_tree_data_new ();
      dir_value->metadata_sha256 = g_strdup (g_checksum_get_string (dir_meta_checksum));
      dir_value->meta_data = dirmeta;
      g_hash_table_insert (tree->directories, g_strdup (basename), dir_value);
    }

  ret = TRUE;
  *dir = dir_value;
 out:
  if (dir_meta_checksum)
    g_checksum_free (dir_meta_checksum);
  return ret;
}

static gboolean
add_one_file_to_tree_and_import (OstreeRepo   *self,
                                 const char     *basename,
                                 const char     *abspath,
                                 ParsedTreeData *tree,
                                 GError        **error)
{
  gboolean ret = FALSE;
  GChecksum *checksum = NULL;
  gboolean did_exist;
  GFile *f = NULL;
  
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_assert (tree != NULL);

  f = ot_util_new_file_for_path (abspath);

  if (!ostree_checksum_file (f, OSTREE_OBJECT_TYPE_FILE, &checksum, NULL, error))
    goto out;

  if (!ostree_repo_store_object_trusted (self, abspath, g_checksum_get_string (checksum),
                                         OSTREE_OBJECT_TYPE_FILE, TRUE, FALSE, &did_exist, error))
    goto out;

  g_hash_table_replace (tree->files, g_strdup (basename),
                        g_strdup (g_checksum_get_string (checksum)));

  ret = TRUE;
 out:
  g_clear_object (&f);
  if (checksum)
    g_checksum_free (checksum);
  return ret;
}

static gboolean
add_one_path_to_tree_and_import (OstreeRepo     *self,
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
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }
  is_directory = S_ISDIR(stbuf.st_mode);
       
  if (components)
    g_ptr_array_free (components, TRUE);
  components = ot_util_path_split (filename);
  g_assert (components->len > 0);

  current_tree = tree;
  for (i = 0; i < components->len; i++)
    {
      component = components->pdata[i];
      g_free (component_abspath);
      component_abspath = ot_util_path_join_n (base, components, i);
      file_sha1 = g_hash_table_lookup (current_tree->files, component);
      dir = g_hash_table_lookup (current_tree->directories, component);

      g_assert_cmpstr (component, !=, ".");

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
                                                     component_abspath, current_tree, &dir,
                                                     error))
            goto out;
          g_assert (dir != NULL);
          current_tree = dir->tree_data;
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
                                                     abspath, current_tree, &dir,
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
  if (components)
    g_ptr_array_unref (components);
  g_free (component_abspath);
  g_free (abspath);
  return ret;
}

gboolean      
ostree_repo_write_ref (OstreeRepo  *self,
                       const char  *remote,
                       const char  *name,
                       const char  *rev,
                       GError     **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GFile *dir = NULL;

  if (remote == NULL)
    dir = g_object_ref (priv->local_heads_dir);
  else
    {
      dir = g_file_get_child (priv->remote_heads_dir, remote);

      if (!ot_util_ensure_directory (ot_gfile_get_path_cached (dir), FALSE, error))
        goto out;
    }

  if (!write_checksum_file (dir, name, rev, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_object (&dir);
  return ret;
}

static gboolean
commit_parsed_tree (OstreeRepo *self,
                    const char   *branch,
                    const char   *parent,
                    const char   *subject,
                    const char   *body,
                    GVariant     *metadata,
                    ParsedDirectoryData *root,
                    GChecksum   **out_commit,
                    GError      **error)
{
  gboolean ret = FALSE;
  GChecksum *root_checksum = NULL;
  GChecksum *ret_commit = NULL;
  GVariant *commit = NULL;
  GDateTime *now = NULL;

  g_assert (branch != NULL);
  g_assert (subject != NULL);

  if (!import_parsed_tree (self, root->tree_data, &root_checksum, error))
    goto out;

  now = g_date_time_new_now_utc ();
  commit = g_variant_new ("(u@a{sv}ssstss)",
                          GUINT32_TO_BE (OSTREE_COMMIT_VERSION),
                          metadata ? metadata : create_empty_gvariant_dict (),
                          parent ? parent : "",
                          subject, body ? body : "",
                          GUINT64_TO_BE (g_date_time_to_unix (now)),
                          g_checksum_get_string (root_checksum),
                          root->metadata_sha256);
  g_variant_ref_sink (commit);
  if (!import_gvariant_object (self, OSTREE_SERIALIZED_COMMIT_VARIANT,
                               commit, &ret_commit, error))
    goto out;

  if (!ostree_repo_write_ref (self, NULL, branch, g_checksum_get_string (ret_commit), error))
    goto out;

  ret = TRUE;
  *out_commit = ret_commit;
 out:
  if (root_checksum)
    g_checksum_free (root_checksum);
  if (commit)
    g_variant_unref (commit);
  if (now)
    g_date_time_unref (now);
  return ret;
}

static gboolean
import_root (OstreeRepo           *self,
             const char           *base,
             ParsedDirectoryData **out_root,
             GError              **error)
{
  gboolean ret = FALSE;
  ParsedDirectoryData *ret_root = NULL;
  GVariant *root_metadata = NULL;
  GChecksum *root_meta_checksum = NULL;

  if (!import_directory_meta (self, base, &root_metadata, &root_meta_checksum, error))
    goto out;

  ret_root = g_new0 (ParsedDirectoryData, 1);
  ret_root->tree_data = parsed_tree_data_new ();
  ret_root->meta_data = root_metadata;
  root_metadata = NULL;
  ret_root->metadata_sha256 = g_strdup (g_checksum_get_string (root_meta_checksum));

  ret = TRUE;
  *out_root = ret_root;
  ret_root = NULL;
 out:
  if (root_metadata)
    g_variant_unref (root_metadata);
  if (root_meta_checksum)
    g_checksum_free (root_meta_checksum);
  parsed_directory_data_free (ret_root);
  return ret;
}

gboolean      
ostree_repo_commit_from_filelist_fd (OstreeRepo *self,
                                     const char   *branch,
                                     const char   *parent,
                                     const char   *subject,
                                     const char   *body,
                                     GVariant     *metadata,
                                     const char   *base,
                                     int           fd,
                                     char          separator,
                                     GChecksum   **out_commit,
                                     GError      **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  ParsedDirectoryData *root = NULL;
  GChecksum *ret_commit_checksum = NULL;
  GUnixInputStream *in = NULL;
  GDataInputStream *datain = NULL;
  char *filename = NULL;
  gsize filename_len;
  GError *temp_error = NULL;
  GVariant *root_metadata = NULL;
  GChecksum *root_meta_checksum = NULL;
  char *current_head = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (priv->inited, FALSE);
  g_return_val_if_fail (branch != NULL, FALSE);
  g_return_val_if_fail (subject != NULL, FALSE);
  g_return_val_if_fail (metadata == NULL || g_variant_is_of_type (metadata, G_VARIANT_TYPE ("a{sv}")), FALSE);

  if (parent == NULL)
    parent = branch;

  /* We're overwriting the tree */
  if (!import_root (self, base, &root, error))
    goto out;

  if (!ostree_repo_resolve_rev (self, parent, TRUE, &current_head, error))
    goto out;

  in = (GUnixInputStream*)g_unix_input_stream_new (fd, FALSE);
  datain = g_data_input_stream_new ((GInputStream*)in);

  while ((filename = g_data_input_stream_read_upto (datain, &separator, 1,
                                                    &filename_len, NULL, &temp_error)) != NULL)
    {
      if (!g_data_input_stream_read_byte (datain, NULL, &temp_error))
        {
          if (temp_error != NULL)
            {
              g_propagate_prefixed_error (error, temp_error, "%s", "While reading filelist: ");
              goto out;
            }
        }
      if (!add_one_path_to_tree_and_import (self, base, filename, root->tree_data, error))
        goto out;
      g_free (filename);
      filename = NULL;
    }
  if (filename == NULL && temp_error != NULL)
    {
      g_propagate_prefixed_error (error, temp_error, "%s", "While reading filelist: ");
      goto out;
    }
  if (!commit_parsed_tree (self, branch, current_head, subject, body, metadata,
                           root, &ret_commit_checksum, error))
    goto out;
  
  ret = TRUE;
  *out_commit = ret_commit_checksum;
  ret_commit_checksum = NULL;
 out:
  if (ret_commit_checksum)
    g_checksum_free (ret_commit_checksum);
  g_free (current_head);
  if (root_metadata)
    g_variant_unref (root_metadata);
  if (root_meta_checksum)
    g_checksum_free (root_meta_checksum);
  g_clear_object (&datain);
  g_clear_object (&in);
  g_free (filename);
  parsed_directory_data_free (root);
  return ret;
  
}

static gboolean
iter_object_dir (OstreeRepo   *self,
                 GFile          *dir,
                 OstreeRepoObjectIter  callback,
                 gpointer                user_data,
                 GError                **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  GFileEnumerator *enumerator = NULL;
  GFileInfo *file_info = NULL;
  const char *dirpath = NULL;

  dirpath = ot_gfile_get_path_cached (dir);

  enumerator = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO, 
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
              || g_str_has_suffix (name, ".file")
              || g_str_has_suffix (name, ".packfile")))
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
  return ret;
}

gboolean
ostree_repo_iter_objects (OstreeRepo  *self,
                            OstreeRepoObjectIter callback,
                            gpointer       user_data,
                            GError        **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GFile *objectdir = NULL;
  GFileEnumerator *enumerator = NULL;
  gboolean ret = FALSE;
  GFileInfo *file_info = NULL;
  GError *temp_error = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (priv->inited, FALSE);

  objectdir = ot_util_new_file_for_path (priv->objects_path);
  enumerator = g_file_enumerate_children (objectdir, OSTREE_GIO_FAST_QUERYINFO, 
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
ostree_repo_load_variant (OstreeRepo *self,
                          const char   *sha256,
                          OstreeSerializedVariantType *out_type,
                          GVariant    **out_variant,
                          GError      **error)
{
  gboolean ret = FALSE;
  OstreeSerializedVariantType ret_type;
  GVariant *ret_variant = NULL;
  char *path = NULL;
  GFile *f = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  path = ostree_repo_get_object_path (self, sha256, OSTREE_OBJECT_TYPE_META);
  f = ot_util_new_file_for_path (path);
  if (!ostree_parse_metadata_file (f, &ret_type, &ret_variant, error))
    goto out;

  ret = TRUE;
  *out_type = ret_type;
  *out_variant = ret_variant;
  ret_variant = NULL;
 out:
  if (ret_variant)
    g_variant_unref (ret_variant);
  g_clear_object (&f);
  g_free (path);
  return ret;
}

static gboolean
checkout_tree (OstreeRepo    *self,
               OstreeRepoFile *dir,
               const char      *destination,
               GCancellable    *cancellable,
               GError         **error);

static gboolean
checkout_one_directory (OstreeRepo  *self,
                        const char *destination,
                        const char *dirname,
                        OstreeRepoFile *dir,
                        GFileInfo      *dir_info,
                        GCancellable    *cancellable,
                        GError         **error)
{
  gboolean ret = FALSE;
  GFile *dest_file = NULL;
  char *dest_path = NULL;
  GVariant *xattr_variant = NULL;

  dest_path = g_build_filename (destination, dirname, NULL);
  dest_file = ot_util_new_file_for_path (dest_path);

  if (!_ostree_repo_file_get_xattrs (dir, &xattr_variant, NULL, error))
    goto out;

  if (mkdir (dest_path, (mode_t)g_file_info_get_attribute_uint32 (dir_info, "unix::mode")) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      g_prefix_error (error, "Failed to create directory '%s': ", dest_path);
      goto out;
    }

  if (!ostree_set_xattrs (dest_file, xattr_variant, cancellable, error))
    goto out;
      
  if (!checkout_tree (self, dir, dest_path, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_object (&dest_file);
  g_free (dest_path);
  if (xattr_variant)
    g_variant_unref (xattr_variant);
  return ret;
}

static gboolean
checkout_tree (OstreeRepo    *self,
               OstreeRepoFile *dir,
               const char      *destination,
               GCancellable    *cancellable,
               GError         **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  GFileInfo *file_info = NULL;
  GFileEnumerator *dir_enum = NULL;
  GFile *child = NULL;
  char *object_path = NULL;
  char *dest_path = NULL;

  dir_enum = g_file_enumerate_children ((GFile*)dir, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  while ((file_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

      child = g_file_get_child ((GFile*)dir, name);

      if (type == G_FILE_TYPE_DIRECTORY)
        {
          if (!checkout_one_directory (self, destination, name, (OstreeRepoFile*)child, file_info, cancellable, error))
            goto out;
        }
      else
        {
          const char *checksum = _ostree_repo_file_get_checksum ((OstreeRepoFile*)child);

          dest_path = g_build_filename (destination, name, NULL);
          object_path = ostree_repo_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_FILE);

          if (priv->archive)
            {
              if (!ostree_unpack_object (object_path, OSTREE_OBJECT_TYPE_FILE, dest_path, NULL, error))
                goto out;
            }
          else
            {
              if (link (object_path, dest_path) < 0)
                {
                  ot_util_set_error_from_errno (error, errno);
                  goto out;
                }
            }
        }

      g_free (object_path);
      object_path = NULL;
      g_free (dest_path);
      dest_path = NULL;
      g_clear_object (&file_info);
      g_clear_object (&child);
    }
  if (file_info == NULL && temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  g_clear_object (&dir_enum);
  g_clear_object (&file_info);
  g_clear_object (&child);
  g_free (object_path);
  g_free (dest_path);
  return ret;
}

gboolean
ostree_repo_checkout (OstreeRepo *self,
                      const char   *rev,
                      const char   *destination,
                      GCancellable *cancellable,
                      GError      **error)
{
  gboolean ret = FALSE;
  char *resolved = NULL;
  OstreeRepoFile *root = NULL;
  GFileInfo *root_info = NULL;

  if (g_file_test (destination, G_FILE_TEST_EXISTS))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Destination path '%s' already exists",
                   destination);
      goto out;
    }

  if (!ostree_repo_resolve_rev (self, rev, FALSE, &resolved, error))
    goto out;

  root = (OstreeRepoFile*)_ostree_repo_file_new_root (self, resolved);
  if (!_ostree_repo_file_ensure_resolved (root, error))
    goto out;

  root_info = g_file_query_info ((GFile*)root, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 NULL, error);
  if (!root_info)
    goto out;

  if (!checkout_one_directory (self, destination, NULL, root, root_info, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  g_free (resolved);
  g_clear_object (&root);
  g_clear_object (&root_info);
  return ret;
}

static gboolean
get_file_checksum (GFile  *f,
                   GFileInfo *f_info,
                   char  **out_checksum,
                   GCancellable *cancellable,
                   GError   **error)
{
  gboolean ret = FALSE;
  GChecksum *tmp_checksum = NULL;
  GVariant *dirmeta = NULL;
  GVariant *packed_dirmeta = NULL;
  char *ret_checksum = NULL;

  if (OSTREE_IS_REPO_FILE (f))
    {
      ret_checksum = g_strdup (_ostree_repo_file_get_checksum ((OstreeRepoFile*)f));
    }
  else
    {
      if (g_file_info_get_file_type (f_info) == G_FILE_TYPE_DIRECTORY)
        {
          tmp_checksum = g_checksum_new (G_CHECKSUM_SHA256);
          if (!ostree_get_directory_metadata (f, &dirmeta, cancellable, error))
            goto out;
          packed_dirmeta = pack_metadata_variant (OSTREE_SERIALIZED_DIRMETA_VARIANT, dirmeta);
          g_checksum_update (tmp_checksum, g_variant_get_data (packed_dirmeta),
                             g_variant_get_size (packed_dirmeta));
          ret_checksum = g_strdup (g_checksum_get_string (tmp_checksum));
        }
      else
        {
          if (!ostree_checksum_file (f, OSTREE_OBJECT_TYPE_FILE,
                                     &tmp_checksum, cancellable, error))
            goto out;
          ret_checksum = g_strdup (g_checksum_get_string (tmp_checksum));
        }
    }

  ret = TRUE;
  *out_checksum = ret_checksum;
  ret_checksum = NULL;
 out:
  g_free (ret_checksum);
  if (tmp_checksum)
    g_checksum_free (tmp_checksum);
  if (dirmeta)
    g_variant_unref (dirmeta);
  if (packed_dirmeta)
    g_variant_unref (packed_dirmeta);
  return ret;
}

OstreeRepoDiffItem *
ostree_repo_diff_item_ref (OstreeRepoDiffItem *diffitem)
{
  g_atomic_int_inc (&diffitem->refcount);
  return diffitem;
}

void
ostree_repo_diff_item_unref (OstreeRepoDiffItem *diffitem)
{
  if (!g_atomic_int_dec_and_test (&diffitem->refcount))
    return;

  g_clear_object (&diffitem->src);
  g_clear_object (&diffitem->target);
  g_clear_object (&diffitem->src_info);
  g_clear_object (&diffitem->target_info);
  g_free (diffitem->src_checksum);
  g_free (diffitem->target_checksum);
  g_free (diffitem);
}

static OstreeRepoDiffItem *
diff_item_new (GFile          *a,
               GFileInfo      *a_info,
               GFile          *b,
               GFileInfo      *b_info,
               char           *checksum_a,
               char           *checksum_b)
{
  OstreeRepoDiffItem *ret = g_new0 (OstreeRepoDiffItem, 1);
  ret->refcount = 1;
  ret->src = a ? g_object_ref (a) : NULL;
  ret->src_info = a_info ? g_object_ref (a_info) : NULL;
  ret->target = b ? g_object_ref (b) : NULL;
  ret->target_info = b_info ? g_object_ref (b_info) : b_info;
  ret->src_checksum = g_strdup (checksum_a);
  ret->target_checksum = g_strdup (checksum_b);
  return ret;
}
               

static gboolean
diff_files (GFile          *a,
            GFileInfo      *a_info,
            GFile          *b,
            GFileInfo      *b_info,
            OstreeRepoDiffItem **out_item,
            GCancellable   *cancellable,
            GError        **error)
{
  gboolean ret = FALSE;
  char *checksum_a = NULL;
  char *checksum_b = NULL;
  OstreeRepoDiffItem *ret_item = NULL;

  if (!get_file_checksum (a, a_info, &checksum_a, cancellable, error))
    goto out;
  if (!get_file_checksum (b, b_info, &checksum_b, cancellable, error))
    goto out;

  if (strcmp (checksum_a, checksum_b) != 0)
    {
      ret_item = diff_item_new (a, a_info, b, b_info,
                                checksum_a, checksum_b);
    }

  ret = TRUE;
  *out_item = ret_item;
  ret_item = NULL;
 out:
  if (ret_item)
    ostree_repo_diff_item_unref (ret_item);
  g_free (checksum_a);
  g_free (checksum_b);
  return ret;
}

static gboolean
diff_add_dir_recurse (GFile          *d,
                      GPtrArray      *added,
                      GCancellable   *cancellable,
                      GError        **error)
{
  gboolean ret = FALSE;
  GFileEnumerator *dir_enum = NULL;
  GError *temp_error = NULL;
  GFile *child = NULL;
  GFileInfo *child_info = NULL;

  dir_enum = g_file_enumerate_children (d, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;

      name = g_file_info_get_name (child_info);

      g_clear_object (&child);
      child = g_file_get_child (d, name);

      g_ptr_array_add (added, g_object_ref (child));

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!diff_add_dir_recurse (child, added, cancellable, error))
            goto out;
        }
      
      g_clear_object (&child_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  g_clear_object (&child_info);
  g_clear_object (&child);
  g_clear_object (&dir_enum);
  return ret;
}

static gboolean
diff_dirs (GFile          *a,
           GFile          *b,
           GPtrArray      *modified,
           GPtrArray      *removed,
           GPtrArray      *added,
           GCancellable   *cancellable,
           GError        **error)
{
  gboolean ret = FALSE;
  GFileEnumerator *dir_enum = NULL;
  GError *temp_error = NULL;
  GFile *child_a = NULL;
  GFile *child_b = NULL;
  GFileInfo *child_a_info = NULL;
  GFileInfo *child_b_info = NULL;

  dir_enum = g_file_enumerate_children (a, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_a_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;
      GFileType child_a_type;
      GFileType child_b_type;

      name = g_file_info_get_name (child_a_info);

      g_clear_object (&child_a);
      child_a = g_file_get_child (a, name);
      child_a_type = g_file_info_get_file_type (child_a_info);

      g_clear_object (&child_b);
      child_b = g_file_get_child (b, name);

      g_clear_object (&child_b_info);
      child_b_info = g_file_query_info (child_b, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        &temp_error);
      if (!child_b_info)
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&temp_error);
              g_ptr_array_add (removed, g_object_ref (child_a));
            }
          else
            {
              g_propagate_error (error, temp_error);
              goto out;
            }
        }
      else
        {
          child_b_type = g_file_info_get_file_type (child_b_info);
          if (child_a_type != child_b_type)
            {
              OstreeRepoDiffItem *diff_item = diff_item_new (child_a, child_a_info,
                                                             child_b, child_b_info, NULL, NULL);
              
              g_ptr_array_add (modified, diff_item);
            }
          else
            {
              OstreeRepoDiffItem *diff_item = NULL;

              if (!diff_files (child_a, child_a_info, child_b, child_b_info, &diff_item, cancellable, error))
                goto out;
              
              if (diff_item)
                g_ptr_array_add (modified, diff_item); /* Transfer ownership */

              if (child_a_type == G_FILE_TYPE_DIRECTORY)
                {
                  if (!diff_dirs (child_a, child_b, modified,
                                  removed, added, cancellable, error))
                    goto out;
                }
            }
        }
      
      g_clear_object (&child_a_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  g_clear_object (&dir_enum);
  dir_enum = g_file_enumerate_children (b, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_b_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;

      name = g_file_info_get_name (child_b_info);

      g_clear_object (&child_a);
      child_a = g_file_get_child (a, name);

      g_clear_object (&child_b);
      child_b = g_file_get_child (b, name);

      g_clear_object (&child_a_info);
      child_a_info = g_file_query_info (child_a, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        &temp_error);
      if (!child_a_info)
        {
          if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&temp_error);
              g_ptr_array_add (added, g_object_ref (child_b));
              if (g_file_info_get_file_type (child_b_info) == G_FILE_TYPE_DIRECTORY)
                {
                  if (!diff_add_dir_recurse (child_b, added, cancellable, error))
                    goto out;
                }
            }
          else
            {
              g_propagate_error (error, temp_error);
              goto out;
            }
        }
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  g_clear_object (&dir_enum);
  g_clear_object (&child_a_info);
  g_clear_object (&child_b_info);
  g_clear_object (&child_a);
  g_clear_object (&child_b);
  return ret;
}

gboolean
ostree_repo_read_commit (OstreeRepo *self,
                         const char *rev, 
                         GFile       **out_root,
                         GCancellable *cancellable,
                         GError **error)
{
  gboolean ret = FALSE;
  GFile *ret_root = NULL;
  char *resolved_rev = NULL;

  if (!ostree_repo_resolve_rev (self, rev, FALSE, &resolved_rev, error))
    goto out;

  ret_root = _ostree_repo_file_new_root (self, resolved_rev);
  if (!_ostree_repo_file_ensure_resolved ((OstreeRepoFile*)ret_root, error))
    goto out;

  ret = TRUE;
  *out_root = ret_root;
  ret_root = NULL;
 out:
  g_free (resolved_rev);
  g_clear_object (&ret_root);
  return ret;
}
                       
gboolean
ostree_repo_diff (OstreeRepo     *self,
                  GFile          *src,
                  GFile          *target,
                  GPtrArray     **out_modified,
                  GPtrArray     **out_removed,
                  GPtrArray     **out_added,
                  GCancellable   *cancellable,
                  GError        **error)
{
  gboolean ret = FALSE;
  GPtrArray *ret_modified = NULL;
  GPtrArray *ret_removed = NULL;
  GPtrArray *ret_added = NULL;

  ret_modified = g_ptr_array_new_with_free_func ((GDestroyNotify)ostree_repo_diff_item_unref);
  ret_removed = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  ret_added = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  if (!diff_dirs (src, target, ret_modified, ret_removed, ret_added, cancellable, error))
    goto out;

  ret = TRUE;
  *out_modified = ret_modified;
  ret_modified = NULL;
  *out_removed = ret_removed;
  ret_removed = NULL;
  *out_added = ret_added;
  ret_added = NULL;
 out:
  if (ret_modified)
    g_ptr_array_free (ret_modified, TRUE);
  if (ret_removed)
    g_ptr_array_free (ret_removed, TRUE);
  if (ret_added)
    g_ptr_array_free (ret_added, TRUE);
  return ret;
}
