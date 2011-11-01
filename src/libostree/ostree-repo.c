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

#define _GNU_SOURCE

#include "config.h"

#include "ostree.h"
#include "otutil.h"

#include <gio/gunixoutputstream.h>
#include <gio/gunixinputstream.h>

static gboolean
link_one_file (OstreeRepo *self, const char *path,
               OstreeObjectType type,
               gboolean ignore_exists, gboolean force,
               GChecksum **out_checksum,
               GError **error);
static char *
get_object_path (OstreeRepo  *self,
                 const char    *checksum,
                 OstreeObjectType type);

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
      char *ref_path;
      char *ref_sha256;
      gboolean subret;

      ref = g_file_resolve_relative_path (priv->local_heads_dir, rev + 5);
      ref_path = g_file_get_path (ref);

      subret = parse_rev_file (self, ref_path, &ref_sha256, error);
      g_clear_object (&ref);
      g_free (ref_path);
        
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

static gboolean
resolve_rev (OstreeRepo     *self,
             const char     *rev,
             gboolean        allow_noent,
             char          **sha256,
             GError        **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  char *ret_rev = NULL;
  GFile *child = NULL;
  char *child_path = NULL;
  GError *temp_error = NULL;

 if (strlen (rev) == 64)
   {
     ret_rev = g_strdup (rev);
   }
 else
   {
     child = g_file_get_child (priv->local_heads_dir, rev);
     child_path = g_file_get_path (child);
     if (!ot_util_gfile_load_contents_utf8 (child, NULL, &ret_rev, NULL, &temp_error))
       {
         if (allow_noent && g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
           {
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
  g_clear_object (&child);
  g_free (child_path);
  g_free (ret_rev);
  return ret;
}

gboolean
ostree_repo_resolve_rev (OstreeRepo     *self,
                         const char     *rev,
                         char          **sha256,
                         GError        **error)
{
  return resolve_rev (self, rev, FALSE, sha256, error);
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
  
  g_key_file_unref (priv->config);
  priv->config = g_key_file_ref (new_config);

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

static gboolean
import_gvariant_object (OstreeRepo  *self,
                        OstreeSerializedVariantType type,
                        GVariant       *variant,
                        GChecksum    **out_checksum,
                        GError       **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GVariant *serialized = NULL;
  gboolean ret = FALSE;
  gsize bytes_written;
  char *tmp_name = NULL;
  int fd = -1;
  GUnixOutputStream *stream = NULL;

  serialized = g_variant_new ("(uv)", (guint32)type, variant);

  tmp_name = g_build_filename (priv->objects_path, "variant-tmp-XXXXXX", NULL);
  fd = g_mkstemp (tmp_name);
  if (fd < 0)
    {
      ot_util_set_error_from_errno (error, errno);
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

  if (!link_one_file (self, tmp_name, OSTREE_OBJECT_TYPE_META, 
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
load_gvariant_object_unknown (OstreeRepo  *self,
                              const char    *sha256,
                              OstreeSerializedVariantType *out_type,
                              GVariant     **out_variant,
                              GError       **error)
{
  gboolean ret = FALSE;
  char *path = NULL;

  path = get_object_path (self, sha256, OSTREE_OBJECT_TYPE_META);
  ret = ostree_parse_metadata_file (path, out_type, out_variant, error);
  g_free (path);

  return ret;
}

static gboolean
load_gvariant_object (OstreeRepo  *self,
                      OstreeSerializedVariantType expected_type,
                      const char    *sha256, 
                      GVariant     **out_variant,
                      GError       **error)
{
  gboolean ret = FALSE;
  OstreeSerializedVariantType type;
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
import_directory_meta (OstreeRepo  *self,
                       const char *path,
                       GVariant  **out_variant,
                       GChecksum **out_checksum,
                       GError    **error)
{
  gboolean ret = FALSE;
  struct stat stbuf;
  GChecksum *ret_checksum = NULL;
  GVariant *dirmeta = NULL;
  GVariant *xattrs = NULL;

  if (lstat (path, &stbuf) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }
  
  if (!S_ISDIR(stbuf.st_mode))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Not a directory: '%s'", path);
      goto out;
    }

  xattrs = ostree_get_xattrs_for_path (path, error);
  if (!xattrs)
    goto out;

  dirmeta = g_variant_new ("(uuuu@a(ayay))",
                           OSTREE_DIR_META_VERSION,
                           (guint32)stbuf.st_uid,
                           (guint32)stbuf.st_gid,
                           (guint32)(stbuf.st_mode & (S_ISUID|S_ISGID|S_ISVTX|S_IRWXU|S_IRWXG|S_IRWXO)),
                           xattrs);
  xattrs = NULL; /* was floating */
  g_variant_ref_sink (dirmeta);

  if (!import_gvariant_object (self, OSTREE_SERIALIZED_DIRMETA_VARIANT, 
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
  if (xattrs)
    g_variant_unref (xattrs);
  return ret;
}

static char *
get_object_path (OstreeRepo  *self,
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

  object_path = get_object_path (self, checksum, type);
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

static gboolean
link_one_file (OstreeRepo *self, const char *path, OstreeObjectType type,
               gboolean ignore_exists, gboolean force,
               GChecksum **out_checksum,
               GError **error)
{
  gboolean ret = FALSE;
  struct stat stbuf;
  GChecksum *id = NULL;
  gboolean did_exist;

  if (!ostree_stat_and_checksum_file (-1, path, type, &id, &stbuf, error))
    goto out;

  if (!ostree_repo_store_object_trusted (self, path, g_checksum_get_string (id), type,
                                         ignore_exists, force, &did_exist, error))
    goto out;

  *out_checksum = id;
  id = NULL;
  ret = TRUE;
 out:
  if (id != NULL)
    g_checksum_free (id);
  return ret;
}

gboolean
ostree_repo_link_file (OstreeRepo *self,
                       const char   *path,
                       gboolean      ignore_exists,
                       gboolean      force,
                       GError      **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GChecksum *checksum = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (priv->inited, FALSE);

  if (!link_one_file (self, path, OSTREE_OBJECT_TYPE_FILE,
                      ignore_exists, force, &checksum, error))
    return FALSE;
  g_checksum_free (checksum);
  return TRUE;
}

static gboolean
unpack_and_checksum_packfile (OstreeRepo   *self,
                              const char   *path,
                              gchar       **out_filename,
                              GChecksum   **out_checksum,
                              GError      **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  GFile *file = NULL;
  char *temp_path = NULL;
  GFile *temp_file = NULL;
  GFileOutputStream *temp_out = NULL;
  char *metadata_buf = NULL;
  GVariant *metadata = NULL;
  GVariant *xattrs = NULL;
  GFileInputStream *in = NULL;
  GChecksum *ret_checksum = NULL;
  guint32 metadata_len;
  guint32 version, uid, gid, mode;
  guint64 content_len;
  gsize bytes_read, bytes_written;
  char buf[8192];
  int temp_fd = -1;

  file = ot_util_new_file_for_path (path);

  in = g_file_read (file, NULL, error);
  if (!in)
    goto out;
      
  if (!g_input_stream_read_all ((GInputStream*)in, &metadata_len, 4, &bytes_read, NULL, error))
    goto out;
  if (bytes_read != 4)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted packfile; too short while reading metadata length");
      goto out;
    }
      
  metadata_len = GUINT32_FROM_BE (metadata_len);
  metadata_buf = g_malloc (metadata_len);

  if (!g_input_stream_read_all ((GInputStream*)in, metadata_buf, metadata_len, &bytes_read, NULL, error))
    goto out;
  if (bytes_read != metadata_len)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted packfile; too short while reading metadata");
      goto out;
    }

  metadata = g_variant_new_from_data (G_VARIANT_TYPE (OSTREE_PACK_FILE_VARIANT_FORMAT),
                                      metadata_buf, metadata_len, FALSE, NULL, NULL);
      
  g_variant_get (metadata, "(uuuu@a(ayay)t)",
                 &version, &uid, &gid, &mode,
                 &xattrs, &content_len);
  uid = GUINT32_FROM_BE (uid);
  gid = GUINT32_FROM_BE (gid);
  mode = GUINT32_FROM_BE (mode);
  content_len = GUINT64_FROM_BE (content_len);

  temp_path = g_build_filename (priv->path, "tmp-packfile-XXXXXX");
  temp_file = ot_util_new_file_for_path (temp_path);
      
  ret_checksum = g_checksum_new (G_CHECKSUM_SHA256);

  if (S_ISREG (mode))
    {
      temp_fd = g_mkstemp (temp_path);
      if (temp_fd < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
      close (temp_fd);
      temp_fd = -1;
      temp_out = g_file_replace (temp_file, NULL, FALSE, 0, NULL, error);
      if (!temp_out)
        goto out;

      do
        {
          if (!g_input_stream_read_all ((GInputStream*)in, buf, sizeof(buf), &bytes_read, NULL, error))
            goto out;
          g_checksum_update (ret_checksum, (guint8*)buf, bytes_read);
          if (!g_output_stream_write_all ((GOutputStream*)temp_out, buf, bytes_read, &bytes_written, NULL, error))
            goto out;
        }
      while (bytes_read > 0);

      if (!g_output_stream_close ((GOutputStream*)temp_out, NULL, error))
        goto out;
    }
  else if (S_ISLNK (mode))
    {
      g_assert (sizeof (buf) > PATH_MAX);

      if (!g_input_stream_read_all ((GInputStream*)in, buf, sizeof(buf), &bytes_read, NULL, error))
        goto out;
      buf[bytes_read] = '\0';
      if (symlink (buf, temp_path) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else if (S_ISCHR (mode) || S_ISBLK (mode))
    {
      guint32 dev;

      if (!g_input_stream_read_all ((GInputStream*)in, &dev, 4, &bytes_read, NULL, error))
        goto out;
      if (bytes_read != 4)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Corrupted packfile; too short while reading device id");
          goto out;
        }
      dev = GUINT32_FROM_BE (dev);
      if (mknod (temp_path, mode, dev) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted packfile; invalid mode %u", mode);
      goto out;
    }

  if (!S_ISLNK (mode))
    {
      if (chmod (temp_path, mode) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  if (!ostree_set_xattrs (temp_path, xattrs, error))
    goto out;

  ostree_checksum_update_stat (ret_checksum, uid, gid, mode);
  g_checksum_update (ret_checksum, (guint8*)g_variant_get_data (xattrs), g_variant_get_size (xattrs));

  ret = TRUE;
  *out_checksum = ret_checksum;
  ret_checksum = NULL;
  *out_filename = temp_path;
  temp_path = NULL;
 out:
  if (ret_checksum)
    g_checksum_free (ret_checksum);
  g_free (metadata_buf);
  if (temp_path)
    (void) unlink (temp_path);
  g_free (temp_path);
  g_clear_object (&file);
  g_clear_object (&in);
  g_clear_object (&temp_file);
  g_clear_object (&temp_out);
  if (metadata)
   g_variant_unref (metadata);
  if (xattrs)
    g_variant_unref (xattrs);
  return ret;
}

gboolean
ostree_repo_store_packfile (OstreeRepo       *self,
                            const char       *expected_checksum,
                            const char       *path,
                            OstreeObjectType  objtype,
                            GError          **error)
{
  gboolean ret = FALSE;
  char *tempfile = NULL;
  GChecksum *checksum = NULL;
  struct stat stbuf;
  gboolean did_exist;

  if (objtype == OSTREE_OBJECT_TYPE_META)
    {
      if (!ostree_stat_and_checksum_file (-1, path, objtype, &checksum, &stbuf, error))
        goto out;
    }
  else
    {
      if (!unpack_and_checksum_packfile (self, path, &tempfile, &checksum, error))
        goto out;

    }

  if (strcmp (g_checksum_get_string (checksum), expected_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted object %s (actual checksum is %s)",
                   expected_checksum, g_checksum_get_string (checksum));
      goto out;
    }

  if (!ostree_repo_store_object_trusted (self, tempfile ? tempfile : path,
                                         expected_checksum,
                                         objtype,
                                         TRUE, FALSE, &did_exist, error))
    goto out;

  ret = TRUE;
 out:
  if (tempfile)
    (void) unlink (tempfile);
  g_free (tempfile);
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

static gboolean
parse_tree (OstreeRepo    *self,
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

  if (!load_gvariant_object (self, OSTREE_SERIALIZED_TREE_VARIANT,
                             sha256, &tree_variant, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
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

      g_variant_get_child (dirs_variant, i, "(sss)",
                           &dirname, &tree_checksum, &meta_checksum);
      
      if (!parse_tree (self, tree_checksum, &child_tree, error))
        goto out;

      if (!load_gvariant_object (self, OSTREE_SERIALIZED_DIRMETA_VARIANT,
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
load_commit_and_trees (OstreeRepo   *self,
                       const char     *commit_sha256,
                       GVariant      **out_commit,
                       ParsedDirectoryData **out_root_data,
                       GError        **error)
{
  GVariant *ret_commit = NULL;
  ParsedDirectoryData *ret_root_data = NULL;
  ParsedTreeData *tree_data = NULL;
  char *ret_metadata_checksum = NULL;
  GVariant *root_metadata = NULL;
  gboolean ret = FALSE;
  const char *tree_contents_checksum;
  const char *tree_meta_checksum;

  if (!load_gvariant_object (self, OSTREE_SERIALIZED_COMMIT_VARIANT,
                             commit_sha256, &ret_commit, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  g_variant_get_child (ret_commit, 6, "&s", &tree_contents_checksum);
  g_variant_get_child (ret_commit, 7, "&s", &tree_meta_checksum);

  if (!load_gvariant_object (self, OSTREE_SERIALIZED_DIRMETA_VARIANT,
                             tree_meta_checksum, &root_metadata, error))
    goto out;

  if (!parse_tree (self, tree_contents_checksum, &tree_data, error))
    goto out;

  ret_root_data = g_new0 (ParsedDirectoryData, 1);
  ret_root_data->tree_data = tree_data;
  ret_root_data->metadata_sha256 = g_strdup (tree_meta_checksum);
  ret_root_data->meta_data = root_metadata;
  root_metadata = NULL;

  ret = TRUE;
  *out_commit = ret_commit;
  ret_commit = NULL;
  *out_root_data = ret_root_data;
  ret_root_data = NULL;
 out:
  if (ret_commit)
    g_variant_unref (ret_commit);
  parsed_directory_data_free (ret_root_data);
  g_free (ret_metadata_checksum);
  if (root_metadata)
    g_variant_unref (root_metadata);
  return ret;
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
  if (!import_gvariant_object (self, OSTREE_SERIALIZED_TREE_VARIANT, serialized_tree, out_checksum, error))
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
walk_parsed_tree (OstreeRepo  *self,
                  const char    *filename,
                  ParsedTreeData *tree,
                  int            *out_filename_index, /* out*/
                  char          **out_component, /* out, must free */
                  ParsedTreeData **out_tree, /* out, but do not free */
                  GError        **error)
{
  gboolean ret = FALSE;
  GPtrArray *components = NULL;
  ParsedTreeData *current_tree = tree;
  const char *component = NULL;
  const char *file_sha1 = NULL;
  ParsedDirectoryData *dir = NULL;
  int i;
  int ret_filename_index = 0;

  components = ot_util_path_split (filename);
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
      else
        {
          g_assert (dir != NULL);
          current_tree = dir->tree_data;
          ret_filename_index++;
        }
    }

  ret = TRUE;
  *out_filename_index = i;
  *out_component = components->pdata[components->len-1];
  components->pdata[components->len-1] = NULL; /* steal */
  *out_tree = current_tree;
 out:
  g_ptr_array_free (components, TRUE);
  return ret;
}

static gboolean
remove_files_from_tree (OstreeRepo   *self,
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
      char *component = NULL;
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
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No such file or directory: %s",
                       filename);
          g_free (component);
          goto out;
        }
      g_free (component);
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
  
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_assert (tree != NULL);

  if (!link_one_file (self, abspath, OSTREE_OBJECT_TYPE_FILE,
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
  g_free (component_abspath);
  g_free (abspath);
  return ret;
}

static gboolean
add_files_to_tree_and_import (OstreeRepo   *self,
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
ostree_repo_write_ref (OstreeRepo  *self,
                       gboolean     is_local,
                       const char  *name,
                       const char  *rev,
                       GError     **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  return write_checksum_file (is_local ? priv->local_heads_dir : priv->remote_heads_dir, 
                              name, rev, error);
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
                          OSTREE_COMMIT_VERSION,
                          create_empty_gvariant_dict (),
                          parent ? parent : "",
                          subject, body ? body : "",
                          g_date_time_to_unix (now) / G_TIME_SPAN_SECOND,
                          g_checksum_get_string (root_checksum),
                          root->metadata_sha256);
  if (!import_gvariant_object (self, OSTREE_SERIALIZED_COMMIT_VARIANT,
                               commit, &ret_commit, error))
    goto out;

  if (!ostree_repo_write_ref (self, TRUE, branch, g_checksum_get_string (ret_commit), error))
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
ostree_repo_commit (OstreeRepo *self,
                    const char   *branch,
                    const char   *parent,
                    const char   *subject,
                    const char   *body,
                    GVariant     *metadata,
                    const char   *base,
                    GPtrArray    *modified_files,
                    GPtrArray    *removed_files,
                    GChecksum   **out_commit,
                    GError      **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  ParsedDirectoryData *root = NULL;
  GVariant *previous_commit = NULL;
  GChecksum *ret_commit_checksum = NULL;
  GVariant *root_metadata = NULL;
  GChecksum *root_meta_checksum = NULL;
  char *current_head = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (priv->inited, FALSE);
  g_return_val_if_fail (branch != NULL, FALSE);
  g_return_val_if_fail (subject != NULL, FALSE);

  if (parent == NULL)
    parent = branch;

  if (!resolve_rev (self, parent, TRUE, &current_head, error))
    goto out;

  if (current_head)
    {
      if (!load_commit_and_trees (self, current_head, &previous_commit, &root, error))
        goto out;
      if (!import_directory_meta (self, base, &root_metadata, &root_meta_checksum, error))
        goto out;
      g_variant_unref (root->meta_data);
      root->meta_data = root_metadata;
      root_metadata = NULL;
      root->metadata_sha256 = g_strdup (g_checksum_get_string (root_meta_checksum));
    }
  else
    {
      /* Initial commit */
      if (!import_root (self, base, &root, error))
        goto out;
    }

  if (!remove_files_from_tree (self, base, removed_files, root->tree_data, error))
    goto out;

  if (!add_files_to_tree_and_import (self, base, modified_files, root->tree_data, error))
    goto out;

  if (!commit_parsed_tree (self, branch, current_head,
                           subject, body, metadata, root,
                           &ret_commit_checksum, error))
    goto out;
  
  ret = TRUE;
  *out_commit = ret_commit_checksum;
  ret_commit_checksum = NULL;
 out:
  if (ret_commit_checksum)
    g_checksum_free (ret_commit_checksum);
  g_free (current_head);
  if (previous_commit)
    g_variant_unref (previous_commit);
  parsed_directory_data_free (root);
  if (root_metadata)
    g_variant_unref (root_metadata);
  if (root_meta_checksum)
    g_checksum_free (root_meta_checksum);
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

  if (parent == NULL)
    parent = branch;

  /* We're overwriting the tree */
  if (!import_root (self, base, &root, error))
    goto out;

  if (!resolve_rev (self, parent, TRUE, &current_head, error))
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
  g_free (dirpath);
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
ostree_repo_load_variant (OstreeRepo *repo,
                            const char   *sha256,
                            OstreeSerializedVariantType *out_type,
                            GVariant    **out_variant,
                            GError      **error)
{
  gboolean ret = FALSE;
  OstreeSerializedVariantType ret_type;
  GVariant *ret_variant = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  
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
      g_prefix_error (error, "Failed to load metadata variant '%s': ", sha256);
    }
  return ret;
}

static gboolean
checkout_tree (OstreeRepo    *self,
               ParsedTreeData  *tree,
               const char      *destination,
               GError         **error);

static gboolean
checkout_one_directory (OstreeRepo  *self,
                        const char *destination,
                        const char *dirname,
                        ParsedDirectoryData *dir,
                        GError         **error)
{
  gboolean ret = FALSE;
  char *dest_path = NULL;
  guint32 version, uid, gid, mode;
  GVariant *xattr_variant = NULL;

  dest_path = g_build_filename (destination, dirname, NULL);
      
  /* PARSE OSTREE_SERIALIZED_DIRMETA_VARIANT */
  g_variant_get (dir->meta_data, "(uuuu@a(ayay))",
                 &version, &uid, &gid, &mode,
                 &xattr_variant);

  if (mkdir (dest_path, (mode_t)mode) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      g_prefix_error (error, "Failed to create directory '%s': ", dest_path);
      goto out;
    }
      
  if (!checkout_tree (self, dir->tree_data, dest_path, error))
    goto out;

  /* TODO - xattrs */
      
  ret = TRUE;
 out:
  g_free (dest_path);
  g_variant_unref (xattr_variant);
  return ret;
}

static gboolean
checkout_tree (OstreeRepo    *self,
               ParsedTreeData  *tree,
               const char      *destination,
               GError         **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;

  g_hash_table_iter_init (&hash_iter, tree->files);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *filename = key;
      const char *checksum = value;
      char *object_path;
      char *dest_path;

      object_path = get_object_path (self, checksum, OSTREE_OBJECT_TYPE_FILE);
      dest_path = g_build_filename (destination, filename, NULL);
      if (link (object_path, dest_path) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          g_free (object_path);
          g_free (dest_path);
          goto out;
        }
      g_free (object_path);
      g_free (dest_path);
    }

  g_hash_table_iter_init (&hash_iter, tree->directories);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *dirname = key;
      ParsedDirectoryData *dir = value;
      
      if (!checkout_one_directory (self, destination, dirname, dir, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_checkout (OstreeRepo *self,
                      const char   *rev,
                      const char   *destination,
                      GError      **error)
{
  gboolean ret = FALSE;
  GVariant *commit = NULL;
  char *resolved = NULL;
  char *root_meta_sha = NULL;
  ParsedDirectoryData *root = NULL;

  if (g_file_test (destination, G_FILE_TEST_EXISTS))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Destination path '%s' already exists",
                   destination);
      goto out;
    }

  if (!resolve_rev (self, rev, FALSE, &resolved, error))
    goto out;

  if (!load_commit_and_trees (self, resolved, &commit, &root, error))
    goto out;

  if (!checkout_one_directory (self, destination, NULL, root, error))
    goto out;

  ret = TRUE;
 out:
  g_free (resolved);
  if (commit)
    g_variant_unref (commit);
  parsed_directory_data_free (root);
  g_free (root_meta_sha);
  return ret;
}
