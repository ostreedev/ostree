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

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#include "ostree-libarchive-input-stream.h"
#endif

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
  gboolean in_transaction;

  GKeyFile *config;
  OstreeRepoMode mode;

  GHashTable *pending_transaction_tmpfiles;
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
  g_hash_table_destroy (priv->pending_transaction_tmpfiles);
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
  
  priv->repo_file = ot_gfile_new_for_path (priv->path);
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
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  
  priv->pending_transaction_tmpfiles = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                              g_free,
                                                              g_free);
}

OstreeRepo*
ostree_repo_new (const char *path)
{
  return g_object_new (OSTREE_TYPE_REPO, "path", path, NULL);
}

static gboolean
parse_rev_file (OstreeRepo     *self,
                GFile          *f,
                char          **sha256,
                GError        **error) G_GNUC_UNUSED;

static gboolean
parse_rev_file (OstreeRepo     *self,
                GFile          *f,
                char          **sha256,
                GError        **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GError *temp_error = NULL;
  gboolean ret = FALSE;
  char *rev = NULL;

  if (!ot_gfile_load_contents_utf8 (f, &rev, NULL, NULL, &temp_error))
    goto out;

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
      char *ref_sha256;
      gboolean subret;

      ref = g_file_resolve_relative_path (priv->local_heads_dir, rev + 5);
      subret = parse_rev_file (self, ref, &ref_sha256, error);
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

  ot_transfer_out_value(sha256, &rev);
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

      if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_COMMIT, tmp2, &commit, error))
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
      if (!ot_gfile_load_contents_utf8 (child, &ret_rev, NULL, NULL, &temp_error))
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

  ot_transfer_out_value(sha256, &ret_rev);
  ret = TRUE;
 out:
  ot_clear_gvariant (&commit);
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

static gboolean
keyfile_get_boolean_with_default (GKeyFile      *keyfile,
                                  const char    *section,
                                  const char    *value,
                                  gboolean       default_value,
                                  gboolean      *out_bool,
                                  GError       **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  gboolean ret_bool;

  ret_bool = g_key_file_get_boolean (keyfile, section, value, &temp_error);
  if (temp_error)
    {
      if (g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
        {
          g_clear_error (&temp_error);
          ret_bool = default_value;
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  ret = TRUE;
  *out_bool = ret_bool;
 out:
  return ret;
}

static gboolean
keyfile_get_value_with_default (GKeyFile      *keyfile,
                                const char    *section,
                                const char    *value,
                                const char    *default_value,
                                char         **out_value,
                                GError       **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  char *ret_value;

  ret_value = g_key_file_get_value (keyfile, section, value, &temp_error);
  if (temp_error)
    {
      if (g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
        {
          g_clear_error (&temp_error);
          ret_value = g_strdup (default_value);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  ret = TRUE;
  ot_transfer_out_value(out_value, &ret_value);
 out:
  g_free (ret_value);
  return ret;
}
                                
gboolean
ostree_repo_check (OstreeRepo *self, GError **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  char *version = NULL;;
  char *mode = NULL;;
  gboolean is_archive;

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

  version = g_key_file_get_value (priv->config, "core", "repo_version", error);
  if (!version)
    goto out;

  if (strcmp (version, "0") != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid repository version '%s'", version);
      goto out;
    }

  if (!keyfile_get_boolean_with_default (priv->config, "core", "archive",
                                         FALSE, &is_archive, error))
    goto out;
  
  if (is_archive)
    priv->mode = OSTREE_REPO_MODE_ARCHIVE;
  else
    {
      if (!keyfile_get_value_with_default (priv->config, "core", "mode",
                                           "bare", &mode, error))
        goto out;

      if (strcmp (mode, "bare") == 0)
        priv->mode = OSTREE_REPO_MODE_BARE;
      else if (strcmp (mode, "archive") == 0)
        priv->mode = OSTREE_REPO_MODE_ARCHIVE;
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid mode '%s' in repository configuration", mode);
          goto out;
        }
    }

  priv->inited = TRUE;
  
  ret = TRUE;
 out:
  g_free (mode);
  g_free (version);
  return ret;
}

const char *
ostree_repo_get_path (OstreeRepo  *self)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  return priv->path;
}

GFile *
ostree_repo_get_tmpdir (OstreeRepo  *self)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  return priv->tmp_dir;
}

OstreeRepoMode
ostree_repo_get_mode (OstreeRepo  *self)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);

  g_return_val_if_fail (priv->inited, FALSE);

  return priv->mode;
}

static OstreeObjectType
get_objtype_for_repo_file (OstreeRepo *self)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  if (priv->mode == OSTREE_REPO_MODE_ARCHIVE)
    return OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META;
  else
    return OSTREE_OBJECT_TYPE_RAW_FILE;
}

GFile *
ostree_repo_get_file_object_path (OstreeRepo   *self,
                                  const char   *checksum)
{
  return ostree_repo_get_object_path (self, checksum, get_objtype_for_repo_file (self));
}

static char *
create_checksum_and_objtype (const char *checksum,
                             OstreeObjectType objtype)
{
  return g_strconcat (checksum, ".", ostree_object_type_to_string (objtype), NULL);
}

gboolean      
ostree_repo_has_object (OstreeRepo           *self,
                        OstreeObjectType      objtype,
                        const char           *checksum,
                        gboolean             *out_have_object,
                        GCancellable         *cancellable,
                        GError             **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  char *tmp_key = NULL;
  GFile *object_path = NULL;

  tmp_key = create_checksum_and_objtype (checksum, objtype);

  if (g_hash_table_lookup (priv->pending_transaction_tmpfiles, tmp_key))
    {
      *out_have_object = TRUE;
    }
  else
    {
      object_path = ostree_repo_get_object_path (self, checksum, objtype);
      
      *out_have_object = g_file_query_exists (object_path, cancellable);
    }
  
  ret = TRUE;
  /* out: */
  g_free (tmp_key);
  g_clear_object (&object_path);
  return ret;
}

static GFileInfo *
dup_file_info_owned_by_me (GFileInfo  *file_info)
{
  GFileInfo *ret = g_file_info_dup (file_info);

  g_file_info_set_attribute_uint32 (ret, "unix::uid", geteuid ());
  g_file_info_set_attribute_uint32 (ret, "unix::gid", getegid ());

  return ret;
}

static gboolean
stage_object_impl (OstreeRepo         *self,
                   OstreeObjectType    objtype,
                   GFileInfo          *file_info,
                   GVariant           *xattrs,
                   GInputStream       *input,
                   const char         *expected_checksum,
                   GChecksum         **out_checksum,
                   GCancellable       *cancellable,
                   GError            **error);

static gboolean
impl_stage_archive_file_object_from_raw (OstreeRepo         *self,
                                         GFileInfo          *file_info,
                                         GVariant           *xattrs,
                                         GInputStream       *input,
                                         const char         *expected_checksum,
                                         GChecksum         **out_checksum,
                                         GCancellable       *cancellable,
                                         GError            **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GChecksum *ret_checksum = NULL;
  GVariant *archive_metadata = NULL;
  GFileInfo *temp_info = NULL;
  GFile *meta_temp_file = NULL;
  GFile *content_temp_file = NULL;
  GVariant *serialized = NULL;
  GInputStream *mem = NULL;
  const char *actual_checksum;
  
  archive_metadata = ostree_create_archive_file_metadata (file_info, xattrs);
  
  serialized = ostree_wrap_metadata_variant (OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META, archive_metadata);
  mem = g_memory_input_stream_new_from_data (g_variant_get_data (serialized),
                                             g_variant_get_size (serialized),
                                             NULL);

  if (!ostree_create_temp_file_from_input (priv->tmp_dir,
                                           "archive-tmp-", NULL,
                                           NULL, NULL, mem,
                                           OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META,
                                           &meta_temp_file,
                                           NULL,
                                           cancellable, error))
    goto out;

  temp_info = dup_file_info_owned_by_me (file_info);
  if (!ostree_create_temp_file_from_input (priv->tmp_dir,
                                           "archive-tmp-", NULL,
                                           temp_info, NULL, input,
                                           OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT,
                                           &content_temp_file,
                                           out_checksum ? &ret_checksum : NULL,
                                           cancellable, error))
    goto out;

  if (ret_checksum)
    {
      ostree_checksum_update_stat (ret_checksum,
                                   g_file_info_get_attribute_uint32 (file_info, "unix::uid"),
                                   g_file_info_get_attribute_uint32 (file_info, "unix::gid"),
                                   g_file_info_get_attribute_uint32 (file_info, "unix::mode"));
      /* FIXME - ensure empty xattrs are the same as NULL xattrs */
      if (xattrs)
        g_checksum_update (ret_checksum, (guint8*)g_variant_get_data (xattrs), g_variant_get_size (xattrs));
    }

  if (expected_checksum)
    {
      if (strcmp (g_checksum_get_string (ret_checksum), expected_checksum) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Corrupted %s object %s (actual checksum is %s)",
                       ostree_object_type_to_string (OSTREE_OBJECT_TYPE_RAW_FILE),
                       expected_checksum, g_checksum_get_string (ret_checksum));
          goto out;
        }
      actual_checksum = expected_checksum;
    }
  else
    actual_checksum = g_checksum_get_string (ret_checksum);

  g_hash_table_insert (priv->pending_transaction_tmpfiles,
                       create_checksum_and_objtype (actual_checksum, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT),
                       g_strdup (ot_gfile_get_basename_cached (content_temp_file)));
  g_hash_table_insert (priv->pending_transaction_tmpfiles,
                       create_checksum_and_objtype (actual_checksum, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META),
                       g_strdup (ot_gfile_get_basename_cached (meta_temp_file)));

  ret = TRUE;
  ot_transfer_out_value (out_checksum, &ret_checksum);
 out:
  ot_clear_gvariant (&serialized);
  ot_clear_gvariant (&archive_metadata);
  g_clear_object (&mem);
  g_clear_object (&temp_info);
  g_clear_object (&meta_temp_file);
  g_clear_object (&content_temp_file);
  ot_clear_checksum (&ret_checksum);
  return ret;
}

static gboolean
stage_object_impl (OstreeRepo         *self,
                   OstreeObjectType    objtype,
                   GFileInfo          *file_info,
                   GVariant           *xattrs,
                   GInputStream       *input,
                   const char         *expected_checksum,
                   GChecksum         **out_checksum,
                   GCancellable       *cancellable,
                   GError            **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GChecksum *ret_checksum = NULL;
  GFileInfo *temp_info = NULL;
  GFile *temp_file = NULL;
  gboolean already_exists;
  const char *actual_checksum;

  g_return_val_if_fail (priv->in_transaction, FALSE);
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  g_assert (expected_checksum || out_checksum);

  if (expected_checksum)
    {
      if (!ostree_repo_has_object (self, objtype, expected_checksum, &already_exists, cancellable, error))
        goto out;
    }
  else
    already_exists = FALSE;

  g_assert (objtype != OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT);
  g_assert (objtype != OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META);

  if (objtype == OSTREE_OBJECT_TYPE_RAW_FILE)
    {
      g_assert (file_info != NULL);
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
        g_assert (input != NULL);
    }
  else if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    {
      g_assert (xattrs == NULL);
      g_assert (input != NULL);
    }

  if (!already_exists)
    {
       if (objtype == OSTREE_OBJECT_TYPE_RAW_FILE && priv->mode == OSTREE_REPO_MODE_ARCHIVE)
        {
          if (!impl_stage_archive_file_object_from_raw (self, file_info, xattrs, input,
                                                        expected_checksum,
                                                        out_checksum ? &ret_checksum : NULL,
                                                        cancellable, error))
            goto out;
        }
      else 
        {
          if (!ostree_create_temp_file_from_input (priv->tmp_dir,
                                                   "store-tmp-", NULL,
                                                   file_info, xattrs, input,
                                                   objtype,
                                                   &temp_file,
                                                   out_checksum ? &ret_checksum : NULL,
                                                   cancellable, error))
            goto out;
      
          if (!ret_checksum)
            actual_checksum = expected_checksum;
          else
            {
              actual_checksum = g_checksum_get_string (ret_checksum);
              if (expected_checksum && strcmp (actual_checksum, expected_checksum) != 0)
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Corrupted %s object %s (actual checksum is %s)",
                               ostree_object_type_to_string (objtype),
                               expected_checksum, actual_checksum);
                  goto out;
                }
            }
          
          g_hash_table_insert (priv->pending_transaction_tmpfiles,
                               create_checksum_and_objtype (actual_checksum, objtype),
                               g_strdup (ot_gfile_get_basename_cached (temp_file)));
          g_clear_object (&temp_file);
        }
    }
  
  ret = TRUE;
  ot_transfer_out_value(out_checksum, &ret_checksum);
 out:
  if (temp_file)
    (void) unlink (ot_gfile_get_path_cached (temp_file));
  g_clear_object (&temp_file);
  g_clear_object (&temp_info);
  ot_clear_checksum (&ret_checksum);
  return ret;
}

static gboolean
commit_staged_file (OstreeRepo         *self,
                    GFile              *file,
                    const char         *checksum,
                    OstreeObjectType    objtype,
                    GCancellable       *cancellable,
                    GError            **error)
{
  gboolean ret = FALSE;
  GFile *dest_file = NULL;
  GFile *checksum_dir = NULL;

  dest_file = ostree_repo_get_object_path (self, checksum, objtype);
  checksum_dir = g_file_get_parent (dest_file);

  if (!ot_gfile_ensure_directory (checksum_dir, FALSE, error))
    goto out;
  
  if (link (ot_gfile_get_path_cached (file), ot_gfile_get_path_cached (dest_file)) < 0)
    {
      if (errno != EEXIST)
        {
          ot_util_set_error_from_errno (error, errno);
          g_prefix_error (error, "Storing file '%s': ",
                          ot_gfile_get_path_cached (file));
          goto out;
        }
    }

  (void) unlink (ot_gfile_get_path_cached (file));

  ret = TRUE;
 out:
  g_clear_object (&dest_file);
  g_clear_object (&checksum_dir);
  return ret;
}

gboolean
ostree_repo_prepare_transaction (OstreeRepo     *self,
                                 GCancellable   *cancellable,
                                 GError        **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  
  g_return_val_if_fail (priv->in_transaction == FALSE, FALSE);

  priv->in_transaction = TRUE;

  return TRUE;
}

gboolean      
ostree_repo_commit_transaction (OstreeRepo     *self,
                                GCancellable   *cancellable,
                                GError        **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GFile *f = NULL;
  GHashTableIter iter;
  gpointer key, value;
  char *checksum = NULL;

  g_return_val_if_fail (priv->in_transaction == TRUE, FALSE);

  priv->in_transaction = FALSE;

  g_hash_table_iter_init (&iter, priv->pending_transaction_tmpfiles);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *checksum_and_type = key;
      const char *filename = value;
      const char *type_str;
      OstreeObjectType objtype;

      type_str = strrchr (checksum_and_type, '.');
      g_assert (type_str);
      g_free (checksum);
      checksum = g_strndup (checksum_and_type, type_str - checksum_and_type);

      objtype = ostree_object_type_from_string (type_str + 1);

      g_clear_object (&f);
      f = g_file_get_child (priv->tmp_dir, filename);
      
      if (!commit_staged_file (self, f, checksum, objtype, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  g_free (checksum);
  g_hash_table_remove_all (priv->pending_transaction_tmpfiles);
  g_clear_object (&f);
  return ret;
}

static gboolean
stage_gvariant_object (OstreeRepo         *self,
                       OstreeObjectType    type,
                       GVariant           *variant,
                       GChecksum         **out_checksum,
                       GCancellable       *cancellable,
                       GError            **error)
{
  gboolean ret = FALSE;
  GChecksum *ret_checksum = NULL;
  GVariant *serialized = NULL;
  GInputStream *mem = NULL;

  serialized = ostree_wrap_metadata_variant (type, variant);
  mem = g_memory_input_stream_new_from_data (g_variant_get_data (serialized),
                                             g_variant_get_size (serialized),
                                             NULL);
  
  if (!stage_object_impl (self, type,
                          NULL, NULL, mem,
                          NULL, &ret_checksum, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_checksum, &ret_checksum);
 out:
  g_clear_object (&mem);
  ot_clear_checksum (&ret_checksum);
  ot_clear_gvariant (&serialized)
  return ret;
}

gboolean
ostree_repo_load_variant (OstreeRepo  *self,
                          OstreeObjectType  expected_type,
                          const char    *sha256, 
                          GVariant     **out_variant,
                          GError       **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GFile *object_path = NULL;
  GFile *tmpfile = NULL;
  GVariant *ret_variant = NULL;
  char *pending_key = NULL;
  const char *pending_tmpfile;

  g_return_val_if_fail (OSTREE_OBJECT_TYPE_IS_META (expected_type), FALSE);

  pending_key = create_checksum_and_objtype (sha256, expected_type);
  if ((pending_tmpfile = g_hash_table_lookup (priv->pending_transaction_tmpfiles, pending_key)) != NULL)
    {
      tmpfile = g_file_get_child (priv->tmp_dir, pending_tmpfile);
      if (!ostree_map_metadata_file (tmpfile, expected_type, &ret_variant, error))
        goto out;
    }
  else
    {
      object_path = ostree_repo_get_object_path (self, sha256, expected_type);
      if (!ostree_map_metadata_file (object_path, expected_type, &ret_variant, error))
        goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_variant, &ret_variant);
 out:
  g_clear_object (&object_path);
  g_clear_object (&tmpfile);
  g_free (pending_key);
  ot_clear_gvariant (&ret_variant);
  return ret;
}

static gboolean
stage_directory_meta (OstreeRepo   *self,
                      GFileInfo    *file_info,
                      GVariant     *xattrs,
                      GChecksum   **out_checksum,
                      GCancellable *cancellable,
                      GError      **error)
{
  gboolean ret = FALSE;
  GChecksum *ret_checksum = NULL;
  GVariant *dirmeta = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  dirmeta = ostree_create_directory_metadata (file_info, xattrs);
  
  if (!stage_gvariant_object (self, OSTREE_OBJECT_TYPE_DIR_META, 
                              dirmeta, &ret_checksum, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_checksum, &ret_checksum);
 out:
  ot_clear_checksum (&ret_checksum);
  ot_clear_gvariant (&dirmeta);
  return ret;
}

GFile *
ostree_repo_get_object_path (OstreeRepo  *self,
                             const char    *checksum,
                             OstreeObjectType type)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  char *path;
  char *relpath;
  GFile *ret;

  relpath = ostree_get_relative_object_path (checksum, type);
  path = g_build_filename (priv->path, relpath, NULL);
  g_free (relpath);
  ret = ot_gfile_new_for_path (path);
  g_free (path);
 
  return ret;
}

gboolean      
ostree_repo_stage_object_trusted (OstreeRepo   *self,
                                  OstreeObjectType objtype,
                                  const char   *checksum,
                                  GFileInfo        *file_info,
                                  GVariant         *xattrs,
                                  GInputStream     *input,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  return stage_object_impl (self, objtype,
                            file_info, xattrs, input,
                            checksum, NULL, cancellable, error);
}

gboolean
ostree_repo_stage_object (OstreeRepo       *self,
                          OstreeObjectType  objtype,
                          const char       *expected_checksum,
                          GFileInfo        *file_info,
                          GVariant         *xattrs,
                          GInputStream     *input,
                          GCancellable     *cancellable,
                          GError          **error)
{
  gboolean ret = FALSE;
  GChecksum *actual_checksum = NULL;
  
  if (!stage_object_impl (self, objtype,
                          file_info, xattrs, input,
                          expected_checksum, &actual_checksum, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  ot_clear_checksum (&actual_checksum);
  return ret;
}

static GVariant *
create_empty_gvariant_dict (void)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE("a{sv}"));
  return g_variant_builder_end (&builder);
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

      if (!ot_gfile_ensure_directory (dir, FALSE, error))
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
do_commit_write_ref (OstreeRepo *self,
                     const char   *branch,
                     const char   *parent,
                     const char   *subject,
                     const char   *body,
                     GVariant     *metadata,
                     const char   *root_contents_checksum,
                     const char   *root_metadata_checksum,
                     GChecksum   **out_commit,
                     GCancellable *cancellable,
                     GError      **error)
{
  gboolean ret = FALSE;
  GChecksum *ret_commit = NULL;
  GVariant *commit = NULL;
  GDateTime *now = NULL;

  g_assert (branch != NULL);
  g_assert (subject != NULL);

  now = g_date_time_new_now_utc ();
  commit = g_variant_new ("(u@a{sv}ssstss)",
                          GUINT32_TO_BE (OSTREE_COMMIT_VERSION),
                          metadata ? metadata : create_empty_gvariant_dict (),
                          parent ? parent : "",
                          subject, body ? body : "",
                          GUINT64_TO_BE (g_date_time_to_unix (now)),
                          root_contents_checksum,
                          root_metadata_checksum);
  g_variant_ref_sink (commit);
  if (!stage_gvariant_object (self, OSTREE_OBJECT_TYPE_COMMIT,
                              commit, &ret_commit, NULL, error))
    goto out;

  if (!ostree_repo_commit_transaction (self, cancellable, error))
    goto out;

  if (!ostree_repo_write_ref (self, NULL, branch, g_checksum_get_string (ret_commit), error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_commit, &ret_commit);
 out:
  ot_clear_checksum (&ret_commit);
  ot_clear_gvariant (&commit);
  if (now)
    g_date_time_unref (now);
  return ret;
}

static GVariant *
create_tree_variant_from_hashes (GHashTable            *file_checksums,
                                 GHashTable            *dir_contents_checksums,
                                 GHashTable            *dir_metadata_checksums)
{
  GVariantBuilder files_builder;
  GVariantBuilder dirs_builder;
  GHashTableIter hash_iter;
  GSList *sorted_filenames = NULL;
  GSList *iter;
  gpointer key, value;
  GVariant *serialized_tree;

  g_variant_builder_init (&files_builder, G_VARIANT_TYPE ("a(ss)"));
  g_variant_builder_init (&dirs_builder, G_VARIANT_TYPE ("a(sss)"));

  g_hash_table_iter_init (&hash_iter, file_checksums);
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

      value = g_hash_table_lookup (file_checksums, name);
      g_variant_builder_add (&files_builder, "(ss)", name, value);
    }
  
  g_slist_free (sorted_filenames);
  sorted_filenames = NULL;

  g_hash_table_iter_init (&hash_iter, dir_metadata_checksums);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      sorted_filenames = g_slist_prepend (sorted_filenames, (char*)name);
    }

  sorted_filenames = g_slist_sort (sorted_filenames, (GCompareFunc)strcmp);

  for (iter = sorted_filenames; iter; iter = iter->next)
    {
      const char *name = iter->data;

      g_variant_builder_add (&dirs_builder, "(sss)",
                             name,
                             g_hash_table_lookup (dir_contents_checksums, name),
                             g_hash_table_lookup (dir_metadata_checksums, name));
    }

  g_slist_free (sorted_filenames);
  sorted_filenames = NULL;

  serialized_tree = g_variant_new ("(u@a{sv}@a(ss)@a(sss))",
                                   GUINT32_TO_BE (0),
                                   create_empty_gvariant_dict (),
                                   g_variant_builder_end (&files_builder),
                                   g_variant_builder_end (&dirs_builder));
  g_variant_ref_sink (serialized_tree);

  return serialized_tree;
}

static GFileInfo *
create_modified_file_info (GFileInfo               *info,
                           OstreeRepoCommitModifier *modifier)
{
  GFileInfo *ret;

  if (!modifier)
    return (GFileInfo*)g_object_ref (info);

  ret = g_file_info_dup (info);
  
  if (modifier->uid >= 0)
    g_file_info_set_attribute_uint32 (ret, "unix::uid", modifier->uid);
  if (modifier->gid >= 0)
    g_file_info_set_attribute_uint32 (ret, "unix::gid", modifier->gid);

  return ret;
}

static gboolean
stage_directory_recurse (OstreeRepo           *self,
                         GFile                *base,
                         GFile                *dir,
                         OstreeRepoCommitModifier *modifier,
                         GChecksum           **out_contents_checksum,
                         GChecksum           **out_metadata_checksum,
                         GCancellable         *cancellable,
                         GError              **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  GChecksum *ret_metadata_checksum = NULL;
  GChecksum *ret_contents_checksum = NULL;
  GFileEnumerator *dir_enum = NULL;
  GFileInfo *child_info = NULL;
  GFileInfo *modified_info = NULL;
  GFile *child = NULL;
  GHashTable *file_checksums = NULL;
  GHashTable *dir_metadata_checksums = NULL;
  GHashTable *dir_contents_checksums = NULL;
  GChecksum *child_file_checksum = NULL;
  GVariant *xattrs = NULL;
  GVariant *serialized_tree = NULL;
  GInputStream *file_input = NULL;

  child_info = g_file_query_info (dir, OSTREE_GIO_FAST_QUERYINFO,
                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                  cancellable, error);
  if (!child_info)
    goto out;

  modified_info = create_modified_file_info (child_info, modifier);

  xattrs = ostree_get_xattrs_for_file (dir, error);
  if (!xattrs)
    goto out;

  if (!stage_directory_meta (self, modified_info, xattrs, &ret_metadata_checksum,
                             cancellable, error))
    goto out;
  
  g_clear_object (&child_info);
  g_clear_object (&modified_info);

  dir_enum = g_file_enumerate_children ((GFile*)dir, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;
  
  file_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          (GDestroyNotify)g_free, (GDestroyNotify)g_free);
  dir_metadata_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  (GDestroyNotify)g_free, (GDestroyNotify)g_free);
  dir_contents_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  (GDestroyNotify)g_free, (GDestroyNotify)g_free);

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name = g_file_info_get_name (child_info);

      g_clear_object (&modified_info);
      modified_info = create_modified_file_info (child_info, modifier);

      g_clear_object (&child);
      child = g_file_get_child (dir, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
        {
          GChecksum *child_dir_metadata_checksum = NULL;
          GChecksum *child_dir_contents_checksum = NULL;

          if (!stage_directory_recurse (self, base, child, modifier, &child_dir_contents_checksum,
                                        &child_dir_metadata_checksum, cancellable, error))
            goto out;

          g_hash_table_replace (dir_contents_checksums, g_strdup (name),
                                g_strdup (g_checksum_get_string (child_dir_contents_checksum)));
          g_hash_table_replace (dir_metadata_checksums, g_strdup (name),
                                g_strdup (g_checksum_get_string (child_dir_metadata_checksum)));
          ot_clear_checksum (&child_dir_contents_checksum);
          ot_clear_checksum (&child_dir_metadata_checksum);
        }
      else
        {
          ot_clear_checksum (&child_file_checksum);
          ot_clear_gvariant (&xattrs);
          g_clear_object (&file_input);

          if (g_file_info_get_file_type (modified_info) == G_FILE_TYPE_REGULAR)
            {
              file_input = (GInputStream*)g_file_read (child, cancellable, error);
              if (!file_input)
                goto out;
            }

          xattrs = ostree_get_xattrs_for_file (child, error);
          if (!xattrs)
            goto out;

          if (!stage_object_impl (self, OSTREE_OBJECT_TYPE_RAW_FILE,
                                  modified_info, xattrs, file_input, NULL,
                                  &child_file_checksum, cancellable, error))
            goto out;

          g_hash_table_replace (file_checksums, g_strdup (name),
                                g_strdup (g_checksum_get_string (child_file_checksum)));
        }

      g_clear_object (&child_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  serialized_tree = create_tree_variant_from_hashes (file_checksums,
                                                     dir_contents_checksums,
                                                     dir_metadata_checksums);

  if (!stage_gvariant_object (self, OSTREE_OBJECT_TYPE_DIR_TREE,
                              serialized_tree, &ret_contents_checksum,
                              cancellable, error))
    goto out;

  ot_transfer_out_value(out_metadata_checksum, &ret_metadata_checksum);
  ot_transfer_out_value(out_contents_checksum, &ret_contents_checksum);
  ret = TRUE;
 out:
  g_clear_object (&dir_enum);
  g_clear_object (&child);
  g_clear_object (&modified_info);
  g_clear_object (&child_info);
  g_clear_object (&file_input);
  if (file_checksums)
    g_hash_table_destroy (file_checksums);
  if (dir_metadata_checksums)
    g_hash_table_destroy (dir_metadata_checksums);
  if (dir_contents_checksums)
    g_hash_table_destroy (dir_contents_checksums);
  ot_clear_checksum (&ret_metadata_checksum);
  ot_clear_checksum (&ret_contents_checksum);
  ot_clear_checksum (&child_file_checksum);
  ot_clear_gvariant (&serialized_tree);
  ot_clear_gvariant (&xattrs);
  return ret;
}

gboolean      
ostree_repo_commit_directory (OstreeRepo *self,
                              const char   *branch,
                              const char   *parent,
                              const char   *subject,
                              const char   *body,
                              GVariant     *metadata,
                              GFile        *dir,
                              OstreeRepoCommitModifier *modifier,
                              GChecksum   **out_commit,
                              GCancellable *cancellable,
                              GError      **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  GChecksum *ret_commit_checksum = NULL;
  GChecksum *root_metadata_checksum = NULL;
  GChecksum *root_contents_checksum = NULL;
  char *current_head = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (priv->inited, FALSE);
  g_return_val_if_fail (branch != NULL, FALSE);
  g_return_val_if_fail (subject != NULL, FALSE);
  g_return_val_if_fail (metadata == NULL || g_variant_is_of_type (metadata, G_VARIANT_TYPE ("a{sv}")), FALSE);

  if (!ostree_repo_prepare_transaction (self, cancellable, error))
    goto out;

  if (parent == NULL)
    parent = branch;

  if (!ostree_repo_resolve_rev (self, parent, TRUE, &current_head, error))
    goto out;

  if (!stage_directory_recurse (self, dir, dir, modifier, &root_contents_checksum, &root_metadata_checksum, cancellable, error))
    goto out;

  if (!do_commit_write_ref (self, branch, current_head, subject, body, metadata,
                            g_checksum_get_string (root_contents_checksum),
                            g_checksum_get_string (root_metadata_checksum),
                            &ret_commit_checksum, cancellable, error))
    goto out;
  
  ret = TRUE;
  ot_transfer_out_value(out_commit, &ret_commit_checksum);
 out:
  ot_clear_checksum (&ret_commit_checksum);
  g_free (current_head);
  ot_clear_checksum (&root_metadata_checksum);
  ot_clear_checksum (&root_contents_checksum);
  return ret;
}

#ifdef HAVE_LIBARCHIVE

static void
propagate_libarchive_error (GError      **error,
                            struct archive *a)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s", archive_error_string (a));
}

static GFileInfo *
file_info_from_archive_entry_and_modifier (struct archive_entry  *entry,
                                           OstreeRepoCommitModifier *modifier)
{
  GFileInfo *info = g_file_info_new ();
  GFileInfo *modified_info = NULL;
  const struct stat *st;
  guint32 file_type;

  st = archive_entry_stat (entry);

  file_type = ot_gfile_type_for_mode (st->st_mode);
  g_file_info_set_attribute_boolean (info, "standard::is-symlink", S_ISLNK (st->st_mode));
  g_file_info_set_attribute_uint32 (info, "standard::type", file_type);
  g_file_info_set_attribute_uint32 (info, "unix::uid", st->st_uid);
  g_file_info_set_attribute_uint32 (info, "unix::gid", st->st_gid);
  g_file_info_set_attribute_uint32 (info, "unix::mode", st->st_mode);

  if (file_type == G_FILE_TYPE_REGULAR)
    {
      g_file_info_set_attribute_uint64 (info, "standard::size", st->st_size);
    }
  else if (file_type == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      g_file_info_set_attribute_byte_string (info, "standard::symlink-target", archive_entry_symlink (entry));
    }
  else if (file_type == G_FILE_TYPE_SPECIAL)
    {
      g_file_info_set_attribute_uint32 (info, "unix::rdev", st->st_rdev);
    }

  modified_info = create_modified_file_info (info, modifier);

  g_object_unref (info);
  
  return modified_info;
}

static gboolean
import_libarchive_entry_file (OstreeRepo           *self,
                              struct archive       *a,
                              struct archive_entry *entry,
                              GFileInfo            *file_info,
                              GChecksum           **out_checksum,
                              GCancellable         *cancellable,
                              GError              **error)
{
  gboolean ret = FALSE;
  GInputStream *archive_stream = NULL;
  GChecksum *ret_checksum = NULL;
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    archive_stream = ostree_libarchive_input_stream_new (a);
  
  if (!stage_object_impl (self, OSTREE_OBJECT_TYPE_RAW_FILE,
                          file_info, NULL, archive_stream,
                          NULL, &ret_checksum,
                          cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_checksum, &ret_checksum);
 out:
  g_clear_object (&archive_stream);
  ot_clear_checksum (&ret_checksum);
  return ret;
}

static gboolean
stage_mutable_tree_recurse (OstreeRepo           *self,
                            OstreeMutableTree    *tree,
                            char                **out_contents_checksum,
                            GCancellable         *cancellable,
                            GError              **error)
{
  gboolean ret = FALSE;
  GChecksum *ret_contents_checksum_obj = NULL;
  char *ret_contents_checksum = NULL;
  GHashTable *dir_metadata_checksums;
  GHashTable *dir_contents_checksums;
  GVariant *serialized_tree = NULL;
  GHashTableIter hash_iter;
  gpointer key, value;

  dir_contents_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  (GDestroyNotify)g_free, (GDestroyNotify)g_free);
  dir_metadata_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  (GDestroyNotify)g_free, (GDestroyNotify)g_free);

  g_hash_table_iter_init (&hash_iter, ostree_mutable_tree_get_subdirs (tree));
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      OstreeMutableTree *child_dir = value;
      char *child_dir_contents_checksum;

      if (!stage_mutable_tree_recurse (self, child_dir, &child_dir_contents_checksum, cancellable, error))
        goto out;
      
      g_hash_table_replace (dir_contents_checksums, g_strdup (name), child_dir_contents_checksum);
      g_hash_table_replace (dir_metadata_checksums, g_strdup (name),
                            g_strdup (ostree_mutable_tree_get_metadata_checksum (child_dir)));
    }
    
  serialized_tree = create_tree_variant_from_hashes (ostree_mutable_tree_get_files (tree),
                                                     dir_contents_checksums,
                                                     dir_metadata_checksums);
      
  if (!stage_gvariant_object (self, OSTREE_OBJECT_TYPE_DIR_TREE,
                              serialized_tree, &ret_contents_checksum_obj,
                              cancellable, error))
    goto out;
  ret_contents_checksum = g_strdup (g_checksum_get_string (ret_contents_checksum_obj));

  ret = TRUE;
  ot_transfer_out_value(out_contents_checksum, &ret_contents_checksum);
 out:
  if (dir_contents_checksums)
    g_hash_table_destroy (dir_contents_checksums);
  if (dir_metadata_checksums)
    g_hash_table_destroy (dir_metadata_checksums);
  g_free (ret_contents_checksum);
  ot_clear_checksum (&ret_contents_checksum_obj);
  ot_clear_gvariant (&serialized_tree);
  return ret;
}

static gboolean
stage_libarchive_entry_into_root (OstreeRepo           *self,
                                  OstreeMutableTree    *root,
                                  struct archive       *a,
                                  struct archive_entry *entry,
                                  OstreeRepoCommitModifier *modifier,
                                  GCancellable         *cancellable,
                                  GError              **error)
{
  gboolean ret = FALSE;
  const char *pathname;
  const char *hardlink;
  const char *basename;
  GFileInfo *file_info = NULL;
  GChecksum *tmp_checksum = NULL;
  GPtrArray *split_path = NULL;
  GPtrArray *hardlink_split_path = NULL;
  OstreeMutableTree *subdir = NULL;
  OstreeMutableTree *parent = NULL;
  OstreeMutableTree *hardlink_source_parent = NULL;
  char *hardlink_source_checksum = NULL;
  OstreeMutableTree *hardlink_source_subdir = NULL;

  pathname = archive_entry_pathname (entry); 
      
  if (!ot_util_path_split_validate (pathname, &split_path, error))
    goto out;

  if (split_path->len == 0)
    {
      parent = NULL;
      basename = NULL;
    }
  else
    {
      if (!ostree_mutable_tree_walk (root, split_path, 0, &parent, error))
        goto out;
      basename = (char*)split_path->pdata[split_path->len-1];
    }

  hardlink = archive_entry_hardlink (entry);
  if (hardlink)
    {
      const char *hardlink_basename;
      
      g_assert (parent != NULL);

      if (!ot_util_path_split_validate (hardlink, &hardlink_split_path, error))
        goto out;
      if (hardlink_split_path->len == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid hardlink path %s", hardlink);
          goto out;
        }
      
      hardlink_basename = hardlink_split_path->pdata[hardlink_split_path->len - 1];
      
      if (!ostree_mutable_tree_walk (root, hardlink_split_path, 0, &hardlink_source_parent, error))
        goto out;
      
      if (!ostree_mutable_tree_lookup (hardlink_source_parent, hardlink_basename,
                                       &hardlink_source_checksum,
                                       &hardlink_source_subdir,
                                       error))
        {
              g_prefix_error (error, "While resolving hardlink target: ");
              goto out;
        }
      
      if (hardlink_source_subdir)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Hardlink %s refers to directory %s",
                       pathname, hardlink);
          goto out;
        }
      g_assert (hardlink_source_checksum);
      
      if (!ostree_mutable_tree_replace_file (parent,
                                             basename,
                                             hardlink_source_checksum,
                                             error))
        goto out;
    }
  else
    {
      file_info = file_info_from_archive_entry_and_modifier (entry, modifier);

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_UNKNOWN)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unsupported file for import: %s", pathname);
          goto out;
        }

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {

          if (!stage_directory_meta (self, file_info, NULL, &tmp_checksum, cancellable, error))
            goto out;

          if (parent == NULL)
            {
              subdir = g_object_ref (root);
            }
          else
            {
              if (!ostree_mutable_tree_ensure_dir (parent, basename, &subdir, error))
                goto out;
            }

          ostree_mutable_tree_set_metadata_checksum (subdir, g_checksum_get_string (tmp_checksum));
        }
      else 
        {
          if (parent == NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Can't import file as root");
              goto out;
            }

          if (!import_libarchive_entry_file (self, a, entry, file_info, &tmp_checksum, cancellable, error))
            goto out;
          
          if (!ostree_mutable_tree_replace_file (parent, basename,
                                                 g_checksum_get_string (tmp_checksum),
                                                 error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  g_clear_object (&file_info);
  ot_clear_checksum (&tmp_checksum);
  g_clear_object (&parent);
  g_clear_object (&subdir);
  g_clear_object (&hardlink_source_parent);
  g_free (hardlink_source_checksum);
  g_clear_object (&hardlink_source_subdir);
  if (hardlink_split_path)
    g_ptr_array_unref (hardlink_split_path);
  if (split_path)
    g_ptr_array_unref (split_path);
  return ret;
}
                          
static gboolean
stage_libarchive_into_root (OstreeRepo           *self,
                            OstreeMutableTree    *root,
                            GFile                *archive_f,
                            OstreeRepoCommitModifier *modifier,
                            GCancellable         *cancellable,
                            GError              **error)
{
  gboolean ret = FALSE;
  struct archive *a;
  struct archive_entry *entry;
  int r;

  a = archive_read_new ();
  archive_read_support_compression_all (a);
  archive_read_support_format_all (a);
  if (archive_read_open_filename (a, ot_gfile_get_path_cached (archive_f), 8192) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }

  while (TRUE)
    {
      r = archive_read_next_header (a, &entry);
      if (r == ARCHIVE_EOF)
        break;
      else if (r != ARCHIVE_OK)
        {
          propagate_libarchive_error (error, a);
          goto out;
        }

      if (!stage_libarchive_entry_into_root (self, root, a, entry, modifier, cancellable, error))
        goto out;
    }
  if (archive_read_close (a) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }

  ret = TRUE;
 out:
  (void)archive_read_close (a);
  return ret;
}
#endif
  
gboolean      
ostree_repo_commit_tarfiles (OstreeRepo *self,
                             const char   *branch,
                             const char   *parent,
                             const char   *subject,
                             const char   *body,
                             GVariant     *metadata,
                             GPtrArray    *tarfiles,
                             OstreeRepoCommitModifier *modifier,
                             GChecksum   **out_commit,
                             GCancellable *cancellable,
                             GError      **error)
{
#ifdef HAVE_LIBARCHIVE
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  GChecksum *ret_commit_checksum = NULL;
  OstreeMutableTree *root = NULL;
  char *root_contents_checksum = NULL;
  char *current_head = NULL;
  int i;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (priv->inited, FALSE);
  g_return_val_if_fail (branch != NULL, FALSE);
  g_return_val_if_fail (subject != NULL, FALSE);
  g_return_val_if_fail (metadata == NULL || g_variant_is_of_type (metadata, G_VARIANT_TYPE ("a{sv}")), FALSE);

  if (!ostree_repo_prepare_transaction (self, cancellable, error))
    goto out;

  if (parent == NULL)
    parent = branch;

  if (!ostree_repo_resolve_rev (self, parent, TRUE, &current_head, error))
    goto out;

  root = ostree_mutable_tree_new ();

  for (i = 0; i < tarfiles->len; i++)
    {
      GFile *archive_f = tarfiles->pdata[i];

      if (!stage_libarchive_into_root (self, root, archive_f, modifier, cancellable, error))
        goto out;
    }

  if (!stage_mutable_tree_recurse (self, root, &root_contents_checksum, cancellable, error))
    goto out;

  if (!do_commit_write_ref (self, branch, current_head, subject, body, metadata,
                            root_contents_checksum, ostree_mutable_tree_get_metadata_checksum (root), &ret_commit_checksum,
                            cancellable, error))
    goto out;
  
  ret = TRUE;
  *out_commit = ret_commit_checksum;
  ret_commit_checksum = NULL;
 out:
  ot_clear_checksum (&ret_commit_checksum);
  g_free (current_head);
  g_free (root_contents_checksum);
  g_clear_object (&root);
  return ret;
#else
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This version of ostree is not compiled with libarchive support");
  return FALSE;
#endif
}

OstreeRepoCommitModifier *
ostree_repo_commit_modifier_new (void)
{
  OstreeRepoCommitModifier *modifier = g_new0 (OstreeRepoCommitModifier, 1);
  modifier->uid = -1;
  modifier->gid = -1;

  modifier->refcount = 1;

  return modifier;
}

void
ostree_repo_commit_modifier_unref (OstreeRepoCommitModifier *modifier)
{
  if (!modifier)
    return;
  if (!g_atomic_int_dec_and_test (&modifier->refcount))
    return;

  g_free (modifier);
  return;
}


static gboolean
iter_object_dir (OstreeRepo             *self,
                 GFile                  *dir,
                 OstreeRepoObjectIter    callback,
                 gpointer                user_data,
                 GError                **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  GFileEnumerator *enumerator = NULL;
  GFileInfo *file_info = NULL;
  const char *dirname = NULL;

  dirname = ot_gfile_get_basename_cached (dir);

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
      char *dot = NULL;
      GFile *child = NULL;
      GString *checksum = NULL;
      OstreeObjectType objtype;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

      if (type == G_FILE_TYPE_DIRECTORY)
        goto loop_out;
      
      if (g_str_has_suffix (name, ".file"))
        objtype = OSTREE_OBJECT_TYPE_RAW_FILE;
      else if (g_str_has_suffix (name, ".archive-meta"))
        objtype = OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META;
      else if (g_str_has_suffix (name, ".archive-content"))
        objtype = OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT;
      else if (g_str_has_suffix (name, ".dirtree"))
        objtype = OSTREE_OBJECT_TYPE_DIR_TREE;
      else if (g_str_has_suffix (name, ".dirmeta"))
        objtype = OSTREE_OBJECT_TYPE_DIR_META;
      else if (g_str_has_suffix (name, ".commit"))
        objtype = OSTREE_OBJECT_TYPE_COMMIT;
      else
        goto loop_out;
          
      dot = strrchr (name, '.');
      g_assert (dot);

      if ((dot - name) != 62)
        goto loop_out;
      
      checksum = g_string_new (dirname);
      g_string_append_len (checksum, name, 62);
      
      child = g_file_get_child (dir, name);
      callback (self, checksum->str, objtype, child, file_info, user_data);
      
    loop_out:
      if (checksum)
        g_string_free (checksum, TRUE);
      g_clear_object (&file_info);
      g_clear_object (&child);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  if (!g_file_enumerator_close (enumerator, NULL, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_object (&file_info);
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

  objectdir = ot_gfile_new_for_path (priv->objects_path);
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

static gboolean
checkout_file_from_input (GFile          *file,
                          OstreeRepoCheckoutMode mode,
                          GFileInfo      *finfo,
                          GVariant       *xattrs,
                          GInputStream   *input,
                          GCancellable   *cancellable,
                          GError        **error)
{
  gboolean ret = FALSE;
  GFileInfo *temp_info = NULL;

  if (mode == OSTREE_REPO_CHECKOUT_MODE_USER)
    {
      if (g_file_info_get_file_type (finfo) == G_FILE_TYPE_SPECIAL)
        return TRUE;

      temp_info = g_file_info_dup (finfo);
      
      g_file_info_set_attribute_uint32 (temp_info, "unix::uid", geteuid ());
      g_file_info_set_attribute_uint32 (temp_info, "unix::gid", getegid ());

      xattrs = NULL;
    }

  if (!ostree_create_file_from_input (file, temp_info ? temp_info : finfo,
                                      xattrs, input, OSTREE_OBJECT_TYPE_RAW_FILE,
                                      NULL, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_object (&temp_info);
  return ret;
}

static gboolean
checkout_tree (OstreeRepo               *self,
               OstreeRepoCheckoutMode    mode,
               GFile                    *destination,
               OstreeRepoFile           *source,
               GFileInfo                *source_info,
               GCancellable             *cancellable,
               GError                  **error)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  GVariant *archive_metadata = NULL;
  GFileInfo *file_info = NULL;
  GVariant *xattrs = NULL;
  GFileEnumerator *dir_enum = NULL;
  GFile *src_child = NULL;
  GFile *dest_path = NULL;
  GFile *object_path = NULL;
  GFile *content_object_path = NULL;
  GInputStream *content_input = NULL;

  if (!ostree_repo_file_get_xattrs (source, &xattrs, NULL, error))
    goto out;

  if (!checkout_file_from_input (destination, mode, source_info,
                                 xattrs, NULL,
                                 cancellable, error))
    goto out;

  ot_clear_gvariant (&xattrs);

  dir_enum = g_file_enumerate_children ((GFile*)source, OSTREE_GIO_FAST_QUERYINFO, 
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

      g_clear_object (&dest_path);
      dest_path = g_file_get_child (destination, name);

      g_clear_object (&src_child);
      src_child = g_file_get_child ((GFile*)source, name);

      if (type == G_FILE_TYPE_DIRECTORY)
        {
          if (!checkout_tree (self, mode, dest_path, (OstreeRepoFile*)src_child, file_info, cancellable, error))
            goto out;
        }
      else
        {
          const char *checksum = ostree_repo_file_get_checksum ((OstreeRepoFile*)src_child);

          if (priv->mode == OSTREE_REPO_MODE_ARCHIVE && mode == OSTREE_REPO_CHECKOUT_MODE_USER)
            {
              g_clear_object (&object_path);
              object_path = ostree_repo_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT);

              if (link (ot_gfile_get_path_cached (object_path), ot_gfile_get_path_cached (dest_path)) < 0)
                {
                  ot_util_set_error_from_errno (error, errno);
                  goto out;
                }
            }
          else if (priv->mode == OSTREE_REPO_MODE_ARCHIVE)
            {
              ot_clear_gvariant (&archive_metadata);
              if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META, checksum, &archive_metadata, error))
                goto out;
              
              ot_clear_gvariant (&xattrs);
              if (!ostree_parse_archived_file_meta (archive_metadata, NULL, &xattrs, error))
                goto out;
              
              g_clear_object (&content_object_path);
              content_object_path = ostree_repo_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT);

              g_clear_object (&content_input);
              if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
                {
                  content_input = (GInputStream*)g_file_read (content_object_path, cancellable, error);
                  if (!content_input)
                    goto out;
                }

              if (!checkout_file_from_input (dest_path, mode, file_info, xattrs, 
                                             content_input, cancellable, error))
                goto out;
            }
          else
            {
              g_clear_object (&object_path);
              object_path = ostree_repo_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_RAW_FILE);

              if (link (ot_gfile_get_path_cached (object_path), ot_gfile_get_path_cached (dest_path)) < 0)
                {
                  ot_util_set_error_from_errno (error, errno);
                  goto out;
                }
            }
        }

      g_clear_object (&file_info);
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
  ot_clear_gvariant (&xattrs);
  ot_clear_gvariant (&archive_metadata);
  g_clear_object (&src_child);
  g_clear_object (&object_path);
  g_clear_object (&content_object_path);
  g_clear_object (&content_input);
  g_clear_object (&dest_path);
  g_free (dest_path);
  return ret;
}

gboolean
ostree_repo_checkout (OstreeRepo              *self,
                      OstreeRepoCheckoutMode   mode,
                      const char              *ref,
                      GFile                   *destination,
                      GCancellable            *cancellable,
                      GError                 **error)
{
  gboolean ret = FALSE;
  char *resolved = NULL;
  OstreeRepoFile *root = NULL;
  GFileInfo *root_info = NULL;

  if (!ostree_repo_resolve_rev (self, ref, FALSE, &resolved, error))
    goto out;

  root = (OstreeRepoFile*)ostree_repo_file_new_root (self, resolved);
  if (!ostree_repo_file_ensure_resolved (root, error))
    goto out;

  root_info = g_file_query_info ((GFile*)root, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 NULL, error);
  if (!root_info)
    goto out;

  if (!checkout_tree (self, mode, destination, root, root_info, cancellable, error))
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
  char *ret_checksum = NULL;

  if (OSTREE_IS_REPO_FILE (f))
    {
      ret_checksum = g_strdup (ostree_repo_file_get_checksum ((OstreeRepoFile*)f));
    }
  else
    {
      if (!ostree_checksum_file (f, OSTREE_OBJECT_TYPE_RAW_FILE,
                                 &tmp_checksum, cancellable, error))
        goto out;
      ret_checksum = g_strdup (g_checksum_get_string (tmp_checksum));
    }

  ret = TRUE;
  ot_transfer_out_value(out_checksum, &ret_checksum);
 out:
  ot_clear_checksum (&tmp_checksum);
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
  ot_transfer_out_value(out_item, &ret_item);
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

  ret_root = ostree_repo_file_new_root (self, resolved_rev);
  if (!ostree_repo_file_ensure_resolved ((OstreeRepoFile*)ret_root, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_root, &ret_root);
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
  ot_transfer_out_value(out_modified, &ret_modified);
  ot_transfer_out_value(out_removed, &ret_removed);
  ot_transfer_out_value(out_added, &ret_added);
 out:
  if (ret_modified)
    g_ptr_array_free (ret_modified, TRUE);
  if (ret_removed)
    g_ptr_array_free (ret_removed, TRUE);
  if (ret_added)
    g_ptr_array_free (ret_added, TRUE);
  return ret;
}
