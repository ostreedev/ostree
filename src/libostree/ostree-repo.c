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

#include <stdio.h>
#include <stdlib.h>

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
  GFile *repodir;
  GFile *tmp_dir;
  GFile *pending_dir;
  GFile *local_heads_dir;
  GFile *remote_heads_dir;
  GFile *objects_dir;
  GFile *pack_dir;
  GFile *remote_cache_dir;
  GFile *config_file;

  gboolean inited;
  gboolean in_transaction;

  GKeyFile *config;
  OstreeRepoMode mode;

  GHashTable *pack_index_mappings;
  GHashTable *pack_data_mappings;
};

static void
ostree_repo_finalize (GObject *object)
{
  OstreeRepo *self = OSTREE_REPO (object);
  OstreeRepoPrivate *priv = GET_PRIVATE (self);

  g_clear_object (&priv->repodir);
  g_clear_object (&priv->tmp_dir);
  g_clear_object (&priv->pending_dir);
  g_clear_object (&priv->local_heads_dir);
  g_clear_object (&priv->remote_heads_dir);
  g_clear_object (&priv->objects_dir);
  g_clear_object (&priv->pack_dir);
  g_clear_object (&priv->remote_cache_dir);
  g_clear_object (&priv->config_file);
  g_hash_table_destroy (priv->pack_index_mappings);
  g_hash_table_destroy (priv->pack_data_mappings);
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
      /* Canonicalize */
      priv->repodir = ot_gfile_new_for_path (ot_gfile_get_path_cached (g_value_get_object (value)));
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
      g_value_set_object (value, priv->repodir);
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

  g_assert (priv->repodir != NULL);
  
  priv->tmp_dir = g_file_resolve_relative_path (priv->repodir, "tmp");
  priv->pending_dir = g_file_resolve_relative_path (priv->repodir, "tmp/pending");
  priv->local_heads_dir = g_file_resolve_relative_path (priv->repodir, "refs/heads");
  priv->remote_heads_dir = g_file_resolve_relative_path (priv->repodir, "refs/remotes");
  
  priv->objects_dir = g_file_get_child (priv->repodir, "objects");
  priv->pack_dir = g_file_get_child (priv->objects_dir, "pack");
  priv->remote_cache_dir = g_file_get_child (priv->repodir, "remote-cache");
  priv->config_file = g_file_get_child (priv->repodir, "config");

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
                                   g_param_spec_object ("path",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
ostree_repo_init (OstreeRepo *self)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  
  priv->pack_index_mappings = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free,
                                                     (GDestroyNotify)g_variant_unref);
  priv->pack_data_mappings = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free,
                                                    (GDestroyNotify)g_mapped_file_unref);
}

OstreeRepo*
ostree_repo_new (GFile *path)
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
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GError *temp_error = NULL;
  ot_lfree char *rev = NULL;

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
  return ret;
}

static gboolean
find_rev_in_remotes (OstreeRepo         *self,
                     const char         *rev,
                     GFile             **out_file,
                     GError            **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GError *temp_error = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GFile *child = NULL;
  ot_lobj GFile *ret_file = NULL;

  dir_enum = g_file_enumerate_children (priv->remote_heads_dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, error);
  if (!dir_enum)
    goto out;

  while ((file_info = g_file_enumerator_next_file (dir_enum, NULL, error)) != NULL)
    {
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          g_clear_object (&child);
          child = g_file_get_child (priv->remote_heads_dir,
                                    g_file_info_get_name (file_info));
          g_clear_object (&ret_file);
          ret_file = g_file_resolve_relative_path (child, rev);
          if (!g_file_query_exists (ret_file, NULL))
            g_clear_object (&ret_file);
        }

      g_clear_object (&file_info);
      
      if (ret_file)
        break;
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_file, &ret_file);
 out:
  return ret;
}

gboolean
ostree_repo_resolve_rev (OstreeRepo     *self,
                         const char     *rev,
                         gboolean        allow_noent,
                         char          **sha256,
                         GError        **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GError *temp_error = NULL;
  ot_lfree char *tmp = NULL;
  ot_lfree char *tmp2 = NULL;
  ot_lfree char *ret_rev = NULL;
  ot_lobj GFile *child = NULL;
  ot_lobj GFile *origindir = NULL;
  ot_lvariant GVariant *commit = NULL;
  ot_lvariant GVariant *parent_csum_v = NULL;
  
  g_return_val_if_fail (rev != NULL, FALSE);

  if (!ostree_validate_rev (rev, error))
    goto out;

  /* We intentionally don't allow a ref that looks like a checksum */
  if (ostree_validate_checksum_string (rev, NULL))
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
      
      g_variant_get_child (commit, 1, "@ay", &parent_csum_v);
      if (g_variant_n_children (parent_csum_v) == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Commit %s has no parent", tmp2);
          goto out;
        }
      ret_rev = ostree_checksum_from_bytes_v (parent_csum_v);
    }
  else
    {
      child = g_file_resolve_relative_path (priv->local_heads_dir, rev);

      if (!g_file_query_exists (child, NULL))
        {
          g_clear_object (&child);

          child = g_file_resolve_relative_path (priv->remote_heads_dir, rev);

          if (!g_file_query_exists (child, NULL))
            {
              g_clear_object (&child);
              
              if (!find_rev_in_remotes (self, rev, &child, error))
                goto out;
              
              if (child == NULL)
                {
                  if (!allow_noent)
                    {
                      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Rev '%s' not found", rev);
                      goto out;
                    }
                  else
                    g_clear_object (&child);
                }
            }
        }

      if (child)
        {
          if (!ot_gfile_load_contents_utf8 (child, &ret_rev, NULL, NULL, &temp_error))
            {
              g_propagate_error (error, temp_error);
              g_prefix_error (error, "Couldn't open ref '%s': ", ot_gfile_get_path_cached (child));
              goto out;
            }

          g_strchomp (ret_rev);
          if (!ostree_validate_checksum_string (ret_rev, error))
            goto out;
        }
    }

  ot_transfer_out_value(sha256, &ret_rev);
  ret = TRUE;
 out:
  return ret;
}

static gboolean
write_checksum_file (GFile *parentdir,
                     const char *name,
                     const char *sha256,
                     GError **error)
{
  gboolean ret = FALSE;
  gsize bytes_written;
  int i;
  ot_lobj GFile *parent = NULL;
  ot_lobj GFile *child = NULL;
  ot_lobj GOutputStream *out = NULL;
  ot_lptrarray GPtrArray *components = NULL;

  if (!ostree_validate_checksum_string (sha256, error))
    goto out;

  if (ostree_validate_checksum_string (name, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Rev name '%s' looks like a checksum", name);
      goto out;
    }

  if (!ot_util_path_split_validate (name, &components, error))
    goto out;

  if (components->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid empty ref name");
      goto out;
    }

  parent = g_object_ref (parentdir);
  for (i = 0; i+1 < components->len; i++)
    {
      child = g_file_get_child (parent, (char*)components->pdata[i]);

      if (!ot_gfile_ensure_directory (child, FALSE, error))
        goto out;

      g_clear_object (&parent);
      parent = child;
      child = NULL;
    }

  child = g_file_get_child (parent, components->pdata[components->len - 1]);
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
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  ot_lfree char *data = NULL;
  gsize len;

  g_return_val_if_fail (priv->inited, FALSE);

  data = g_key_file_to_data (new_config, &len, error);
  if (!g_file_replace_contents (priv->config_file, data, len, NULL, FALSE, 0, NULL,
                                NULL, error))
    goto out;
  
  g_key_file_free (priv->config);
  priv->config = g_key_file_new ();
  if (!g_key_file_load_from_data (priv->config, data, len, 0, error))
    goto out;

  ret = TRUE;
 out:
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
  ot_lfree char *ret_value;

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
  return ret;
}
                                
gboolean
ostree_repo_check (OstreeRepo *self, GError **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gboolean is_archive;
  ot_lfree char *version = NULL;;
  ot_lfree char *mode = NULL;;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (priv->inited)
    return TRUE;

  if (!g_file_test (ot_gfile_get_path_cached (priv->objects_dir), G_FILE_TEST_IS_DIR))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't find objects directory '%s'",
                   ot_gfile_get_path_cached (priv->objects_dir));
      goto out;
    }

  if (!ot_gfile_ensure_directory (priv->pending_dir, FALSE, error))
    goto out;
  
  priv->config = g_key_file_new ();
  if (!g_key_file_load_from_file (priv->config, ot_gfile_get_path_cached (priv->config_file), 0, error))
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
  return ret;
}

GFile *
ostree_repo_get_path (OstreeRepo  *self)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  return priv->repodir;
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
                   gboolean            store_if_packed,
                   GFileInfo          *file_info,
                   GVariant           *xattrs,
                   GInputStream       *input,
                   const char         *expected_checksum,
                   guchar            **out_csum,
                   GCancellable       *cancellable,
                   GError            **error);

static gboolean
commit_tmpfile_trusted (OstreeRepo        *self,
                        const char        *checksum,
                        OstreeObjectType   objtype,
                        GFile             *tempfile_path,
                        GCancellable      *cancellable,
                        GError           **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *dest_file = NULL;
  ot_lobj GFile *checksum_dir = NULL;

  dest_file = ostree_repo_get_object_path (self, checksum, objtype);
  checksum_dir = g_file_get_parent (dest_file);

  if (!ot_gfile_ensure_directory (checksum_dir, FALSE, error))
    goto out;
  
  if (link (ot_gfile_get_path_cached (tempfile_path), ot_gfile_get_path_cached (dest_file)) < 0)
    {
      if (errno != EEXIST)
        {
          ot_util_set_error_from_errno (error, errno);
          g_prefix_error (error, "Storing file '%s': ",
                          ot_gfile_get_path_cached (dest_file));
          goto out;
        }
    }

  (void) unlink (ot_gfile_get_path_cached (tempfile_path));

  ret = TRUE;
 out:
  return ret;
}

static gboolean
impl_stage_archive_file_object_from_raw (OstreeRepo         *self,
                                         GFileInfo          *file_info,
                                         GVariant           *xattrs,
                                         GInputStream       *input,
                                         const char         *expected_checksum,
                                         guchar            **out_csum,
                                         GCancellable       *cancellable,
                                         GError            **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  const char *actual_checksum;
  ot_lvariant GVariant *archive_metadata = NULL;
  ot_lobj GFileInfo *temp_info = NULL;
  ot_lobj GFile *meta_temp_file = NULL;
  ot_lobj GFile *content_temp_file = NULL;
  ot_lobj GInputStream *mem = NULL;
  ot_lobj OstreeChecksumInputStream *checksum_input = NULL;
  ot_lfree guchar *ret_csum = NULL;
  GChecksum *checksum = NULL;

  if (expected_checksum || out_csum)
    {
      checksum = g_checksum_new (G_CHECKSUM_SHA256);
      if (input)
        checksum_input = ostree_checksum_input_stream_new (input, checksum);
    }
  
  archive_metadata = ostree_create_archive_file_metadata (file_info, xattrs);
  
  mem = g_memory_input_stream_new_from_data (g_variant_get_data (archive_metadata),
                                             g_variant_get_size (archive_metadata),
                                             NULL);

  if (!ostree_create_temp_file_from_input (priv->tmp_dir,
                                           "archive-tmp-", NULL,
                                           NULL, NULL, mem,
                                           &meta_temp_file,
                                           cancellable, error))
    goto out;

  temp_info = dup_file_info_owned_by_me (file_info);
  /* Archived content files should always be readable by all and
   * read/write by owner.  If the base file is executable then
   * we're also executable.
   */
  g_file_info_set_attribute_uint32 (temp_info, "unix::mode",
                                    g_file_info_get_attribute_uint32 (file_info, "unix::mode") | 0644);
  if (!ostree_create_temp_file_from_input (priv->tmp_dir,
                                           "archive-tmp-", NULL,
                                           temp_info, NULL,
                                           checksum_input ? (GInputStream*)checksum_input : input,
                                           &content_temp_file,
                                           cancellable, error))
    goto out;

  if (checksum)
    ostree_checksum_update_meta (checksum, file_info, xattrs);

  if (expected_checksum && checksum)
    {
      if (strcmp (g_checksum_get_string (checksum), expected_checksum) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Corrupted %s object %s (actual checksum is %s)",
                       ostree_object_type_to_string (OSTREE_OBJECT_TYPE_RAW_FILE),
                       expected_checksum, g_checksum_get_string (checksum));
          goto out;
        }
      actual_checksum = expected_checksum;
    }
  else if (expected_checksum)
    actual_checksum = expected_checksum;
  else
    actual_checksum = g_checksum_get_string (checksum);

  if (!commit_tmpfile_trusted (self, actual_checksum, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT,
                               content_temp_file, cancellable, error))
    goto out;

  if (!commit_tmpfile_trusted (self, actual_checksum, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META,
                               meta_temp_file, cancellable, error))
    goto out;

  if (checksum)
    ret_csum = ot_csum_from_gchecksum (checksum);

  ret = TRUE;
  ot_transfer_out_value (out_csum, &ret_csum);
 out:
  ot_clear_checksum (&checksum);
  return ret;
}

static gboolean
stage_object_impl (OstreeRepo         *self,
                   OstreeObjectType    objtype,
                   gboolean            store_if_packed,
                   GFileInfo          *file_info,
                   GVariant           *xattrs,
                   GInputStream       *input,
                   const char         *expected_checksum,
                   guchar            **out_csum,
                   GCancellable       *cancellable,
                   GError            **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  guint64 pack_offset;
  const char *actual_checksum;
  ot_lobj GFileInfo *temp_info = NULL;
  ot_lobj GFile *temp_file = NULL;
  ot_lobj GFile *stored_path = NULL;
  ot_lfree char *pack_checksum = NULL;
  ot_lfree guchar *ret_csum = NULL;
  ot_lobj OstreeChecksumInputStream *checksum_input = NULL;
  GChecksum *checksum = NULL;

  g_return_val_if_fail (priv->in_transaction, FALSE);
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  g_assert (expected_checksum || out_csum);

  if (expected_checksum)
    {
      if (!store_if_packed)
        {
          if (!ostree_repo_find_object (self, objtype, expected_checksum,
                                        &stored_path, &pack_checksum, &pack_offset,
                                        cancellable, error))
            goto out;
        }
      else
        {
          if (!ostree_repo_find_object (self, objtype, expected_checksum,
                                        &stored_path, NULL, NULL,
                                        cancellable, error))
            goto out;
        }
    }

  g_assert (objtype != OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT);
  g_assert (objtype != OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META);

  if (stored_path == NULL && pack_checksum == NULL)
    {
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

       if (objtype == OSTREE_OBJECT_TYPE_RAW_FILE && priv->mode == OSTREE_REPO_MODE_ARCHIVE)
        {
          if (!impl_stage_archive_file_object_from_raw (self, file_info, xattrs, input,
                                                        expected_checksum,
                                                        out_csum ? &ret_csum : NULL,
                                                        cancellable, error))
            goto out;
        }
       else
        {
          if (out_csum)
            {
              checksum = g_checksum_new (G_CHECKSUM_SHA256);
              if (input)
                checksum_input = ostree_checksum_input_stream_new (input, checksum);
            }
          
          if (!ostree_create_temp_file_from_input (priv->tmp_dir,
                                                   ostree_object_type_to_string (objtype), NULL,
                                                   file_info, xattrs,
                                                   checksum_input ? (GInputStream*)checksum_input : input,
                                                   &temp_file,
                                                   cancellable, error))
            goto out;

          if (checksum && !OSTREE_OBJECT_TYPE_IS_META (objtype))
            ostree_checksum_update_meta (checksum, file_info, xattrs);
      
          if (!checksum)
            actual_checksum = expected_checksum;
          else
            {
              actual_checksum = g_checksum_get_string (checksum);
              if (expected_checksum && strcmp (actual_checksum, expected_checksum) != 0)
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Corrupted %s object %s (actual checksum is %s)",
                               ostree_object_type_to_string (objtype),
                               expected_checksum, actual_checksum);
                  goto out;
                }
            }
          
          if (!commit_tmpfile_trusted (self, actual_checksum, objtype, 
                                       temp_file, cancellable, error))
            goto out;
          g_clear_object (&temp_file);

          if (checksum)
            ret_csum = ot_csum_from_gchecksum (checksum);
        }
    }
  else
    {
      g_assert (stored_path != NULL || pack_checksum != NULL);
      /* Nothing to do */
    }

  ret = TRUE;
  ot_transfer_out_value(out_csum, &ret_csum);
 out:
  if (temp_file)
    (void) unlink (ot_gfile_get_path_cached (temp_file));
  ot_clear_checksum (&checksum);
  return ret;
}

gboolean
ostree_repo_prepare_transaction (OstreeRepo     *self,
                                 GCancellable   *cancellable,
                                 GError        **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);

  g_return_val_if_fail (priv->in_transaction == FALSE, FALSE);

  priv->in_transaction = TRUE;

  ret = TRUE;
  /* out: */
  return ret;
}

gboolean      
ostree_repo_commit_transaction (OstreeRepo     *self,
                                GCancellable   *cancellable,
                                GError        **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);

  g_return_val_if_fail (priv->in_transaction == TRUE, FALSE);

  ret = TRUE;
  /* out: */
  priv->in_transaction = FALSE;

  return ret;
}

gboolean
ostree_repo_abort_transaction (OstreeRepo     *self,
                               GCancellable   *cancellable,
                               GError        **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);

  priv->in_transaction = FALSE;

  ret = TRUE;
  return ret;
}

static gboolean
stage_gvariant_object (OstreeRepo         *self,
                       OstreeObjectType    type,
                       GVariant           *variant,
                       guchar            **out_csum,
                       GCancellable       *cancellable,
                       GError            **error)
{
  gboolean ret = FALSE;
  ot_lobj GInputStream *mem = NULL;

  mem = g_memory_input_stream_new_from_data (g_variant_get_data (variant),
                                             g_variant_get_size (variant),
                                             NULL);
  
  if (!stage_object_impl (self, type, FALSE,
                          NULL, NULL, mem,
                          NULL, out_csum, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
stage_directory_meta (OstreeRepo   *self,
                      GFileInfo    *file_info,
                      GVariant     *xattrs,
                      guchar      **out_csum,
                      GCancellable *cancellable,
                      GError      **error)
{
  gboolean ret = FALSE;
  ot_lvariant GVariant *dirmeta = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  dirmeta = ostree_create_directory_metadata (file_info, xattrs);
  
  if (!stage_gvariant_object (self, OSTREE_OBJECT_TYPE_DIR_META, 
                              dirmeta, out_csum, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

GFile *
ostree_repo_get_object_path (OstreeRepo  *self,
                             const char    *checksum,
                             OstreeObjectType type)
{
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  char *relpath;
  GFile *ret;

  relpath = ostree_get_relative_object_path (checksum, type);
  ret = g_file_resolve_relative_path (priv->repodir, relpath);
  g_free (relpath);
 
  return ret;
}

gboolean      
ostree_repo_stage_object_trusted (OstreeRepo   *self,
                                  OstreeObjectType objtype,
                                  const char   *checksum,
                                  gboolean          store_if_packed,
                                  GFileInfo        *file_info,
                                  GVariant         *xattrs,
                                  GInputStream     *input,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  return stage_object_impl (self, objtype, store_if_packed,
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
  ot_lfree guchar *actual_csum = NULL;
  
  if (!stage_object_impl (self, objtype, FALSE,
                          file_info, xattrs, input,
                          expected_checksum, &actual_csum,
                          cancellable, error))
    goto out;

  ret = TRUE;
 out:
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
enumerate_refs_recurse (OstreeRepo    *repo,
                        GFile         *base,
                        GFile         *dir,
                        GHashTable    *refs,
                        GCancellable  *cancellable,
                        GError       **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GFileEnumerator *enumerator = NULL;
  ot_lobj GFile *child = NULL;

  enumerator = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO,
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, error);
  if (!enumerator)
    goto out;

  while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
    {
      g_clear_object (&child);
      child = g_file_get_child (dir, g_file_info_get_name (file_info));
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!enumerate_refs_recurse (repo, base, child, refs, cancellable, error))
            goto out;
        }
      else if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
        {
          char *contents;
          gsize len;

          if (!g_file_load_contents (child, cancellable, &contents, &len, NULL, error))
            goto out;

          g_strchomp (contents);

          g_hash_table_insert (refs, g_file_get_relative_path (base, child), contents);
        }

      g_clear_object (&file_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_list_all_refs (OstreeRepo       *repo,
                           GHashTable      **out_all_refs,
                           GCancellable     *cancellable,
                           GError          **error)
{
  gboolean ret = FALSE;
  ot_lhash GHashTable *ret_all_refs = NULL;
  ot_lobj GFile *heads_dir = NULL;

  ret_all_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  heads_dir = g_file_resolve_relative_path (ostree_repo_get_path (repo), "refs/heads");
  if (!enumerate_refs_recurse (repo, heads_dir, heads_dir, ret_all_refs, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_all_refs, &ret_all_refs);
 out:
  return ret;
}

static gboolean
write_ref_summary (OstreeRepo      *self,
                   GCancellable    *cancellable,
                   GError         **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  gsize bytes_written;
  ot_lhash GHashTable *all_refs = NULL;
  ot_lobj GFile *summary_path = NULL;
  ot_lobj GOutputStream *out = NULL;
  ot_lfree char *buf = NULL;

  if (!ostree_repo_list_all_refs (self, &all_refs, cancellable, error))
    goto out;

  summary_path = g_file_resolve_relative_path (ostree_repo_get_path (self),
                                               "refs/summary");

  out = (GOutputStream*) g_file_replace (summary_path, NULL, FALSE, 0, cancellable, error);
  if (!out)
    goto out;
  
  g_hash_table_iter_init (&hash_iter, all_refs);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      const char *sha256 = value;

      g_free (buf);
      buf = g_strdup_printf ("%s %s\n", sha256, name);
      if (!g_output_stream_write_all (out, buf, strlen (buf), &bytes_written, cancellable, error))
        goto out;
    }

  if (!g_output_stream_close (out, cancellable, error))
    goto out;

  ret = TRUE;
 out:
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
  ot_lobj GFile *dir = NULL;

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

  if (priv->mode == OSTREE_REPO_MODE_ARCHIVE)
    {
      if (!write_ref_summary (self, NULL, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_stage_commit (OstreeRepo *self,
                          const char   *branch,
                          const char   *parent,
                          const char   *subject,
                          const char   *body,
                          GVariant     *metadata,
                          const char   *root_contents_checksum,
                          const char   *root_metadata_checksum,
                          char        **out_commit,
                          GCancellable *cancellable,
                          GError      **error)
{
  gboolean ret = FALSE;
  ot_lfree char *ret_commit = NULL;
  ot_lvariant GVariant *commit = NULL;
  ot_lfree guchar *commit_csum = NULL;
  GDateTime *now = NULL;

  g_return_val_if_fail (branch != NULL, FALSE);
  g_return_val_if_fail (subject != NULL, FALSE);
  g_return_val_if_fail (root_contents_checksum != NULL, FALSE);
  g_return_val_if_fail (root_metadata_checksum != NULL, FALSE);

  now = g_date_time_new_now_utc ();
  commit = g_variant_new ("(@a{sv}@ay@a(say)sst@ay@ay)",
                          metadata ? metadata : create_empty_gvariant_dict (),
                          parent ? ostree_checksum_to_bytes_v (parent) : ot_gvariant_new_bytearray (NULL, 0),
                          g_variant_new_array (G_VARIANT_TYPE ("(say)"),
                                               NULL, 0),
                          subject, body ? body : "",
                          GUINT64_TO_BE (g_date_time_to_unix (now)),
                          ostree_checksum_to_bytes_v (root_contents_checksum),
                          ostree_checksum_to_bytes_v (root_metadata_checksum));
  g_variant_ref_sink (commit);
  if (!stage_gvariant_object (self, OSTREE_OBJECT_TYPE_COMMIT,
                              commit, &commit_csum, cancellable, error))
    goto out;

  ret_commit = ostree_checksum_from_bytes (commit_csum);

  ret = TRUE;
  ot_transfer_out_value(out_commit, &ret_commit);
 out:
  if (now)
    g_date_time_unref (now);
  return ret;
}

static gboolean
list_files_in_dir_matching (GFile                  *dir,
                            const char             *prefix,
                            const char             *suffix,
                            GPtrArray             **out_files,
                            GCancellable           *cancellable,
                            GError                **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFileEnumerator *enumerator = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lptrarray GPtrArray *ret_files = NULL;

  g_return_val_if_fail (prefix != NULL || suffix != NULL, FALSE);

  ret_files = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  enumerator = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO, 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, 
                                          error);
  if (!enumerator)
    goto out;
  
  while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

      if (type != G_FILE_TYPE_REGULAR)
        goto loop_next;

      if (prefix)
        {
          if (!g_str_has_prefix (name, prefix))
            goto loop_next;
        }
      if (suffix)
        {
          if (!g_str_has_suffix (name, suffix))
            goto loop_next;
        }

      g_ptr_array_add (ret_files, g_file_get_child (dir, name));
      
    loop_next:
      g_clear_object (&file_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  if (!g_file_enumerator_close (enumerator, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_files, &ret_files);
 out:
  return ret;
}

static gboolean
map_variant_file_check_header_string (GFile         *path,
                                      const GVariantType  *variant_type,
                                      const char    *expected_header,
                                      GVariant     **out_variant,
                                      GCancellable  *cancellable,
                                      GError       **error)
{
  gboolean ret = FALSE;
  const char *header;
  ot_lvariant GVariant *ret_variant = NULL;

  if (!ot_util_variant_map (path, variant_type, &ret_variant, error))
    goto out;

  g_variant_get_child (ret_variant, 0, "&s", &header);

  if (strcmp (header, expected_header) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid variant file '%s', expected header '%s'",
                   ot_gfile_get_path_cached (path),
                   expected_header);
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_variant, &ret_variant);
 out:
  return ret;
}


static char *
get_checksum_from_pack_name (const char *name)
{
  const char *dash;
  const char *dot;

  dash = strchr (name, '-');
  g_assert (dash);
  dot = strrchr (name, '.');
  g_assert (dot);

  g_assert_cmpint (dot - (dash + 1), ==, 64);
  
  return g_strndup (dash + 1, 64);
}

static gboolean
list_pack_indexes_from_dir (OstreeRepo              *self,
                            gboolean                 is_meta,
                            GPtrArray              **out_indexes,
                            GCancellable            *cancellable,
                            GError                 **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  guint i;
  ot_lptrarray GPtrArray *index_files = NULL;
  ot_lptrarray GPtrArray *ret_indexes = NULL;

  if (!list_files_in_dir_matching (priv->pack_dir,
                                   is_meta ? "ostmetapack-" : "ostdatapack-", ".index",
                                   &index_files, 
                                   cancellable, error))
    goto out;

  ret_indexes = g_ptr_array_new_with_free_func ((GDestroyNotify)g_free);
  for (i = 0; i < index_files->len; i++)
    {
      GFile *index_path = index_files->pdata[i];
      const char *basename = ot_gfile_get_basename_cached (index_path);
      g_ptr_array_add (ret_indexes, get_checksum_from_pack_name (basename));
    }

  ret = TRUE;
  ot_transfer_out_value (out_indexes, &ret_indexes);
 out:
  return ret;
}

static gboolean
list_pack_checksums_from_superindex_file (GFile         *superindex_path,
                                          GPtrArray    **out_meta_indexes,
                                          GPtrArray    **out_data_indexes,
                                          GCancellable  *cancellable,
                                          GError       **error)
{
  gboolean ret = FALSE;
  const char *magic;
  ot_lptrarray GPtrArray *ret_meta_indexes = NULL;
  ot_lptrarray GPtrArray *ret_data_indexes = NULL;
  ot_lvariant GVariant *superindex_variant = NULL;
  ot_lvariant GVariant *checksum = NULL;
  ot_lvariant GVariant *bloom = NULL;
  GVariantIter *meta_variant_iter = NULL;
  GVariantIter *data_variant_iter = NULL;

  if (!ot_util_variant_map (superindex_path, OSTREE_PACK_SUPER_INDEX_VARIANT_FORMAT,
                            &superindex_variant, error))
    goto out;
  
  g_variant_get (superindex_variant, "(&s@a{sv}a(ayay)a(ayay))",
                 &magic, NULL, &meta_variant_iter, &data_variant_iter);
  
  if (strcmp (magic, "OSTv0SUPERPACKINDEX") != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Invalid header in super pack index");
      goto out;
    }

  ret_meta_indexes = g_ptr_array_new_with_free_func ((GDestroyNotify)g_free); 
  while (g_variant_iter_loop (meta_variant_iter, "(@ay@ay)",
                              &checksum, &bloom))
    g_ptr_array_add (ret_meta_indexes, ostree_checksum_from_bytes_v (checksum));
  checksum = NULL;
  bloom = NULL;

  ret_data_indexes = g_ptr_array_new_with_free_func ((GDestroyNotify)g_free); 
  while (g_variant_iter_loop (data_variant_iter, "(@ay@ay)",
                              &checksum, &bloom))
    g_ptr_array_add (ret_data_indexes, ostree_checksum_from_bytes_v (checksum));
  checksum = NULL;
  bloom = NULL;

  ret = TRUE;
  ot_transfer_out_value (out_meta_indexes, &ret_meta_indexes);
  ot_transfer_out_value (out_data_indexes, &ret_data_indexes);
 out:
  if (meta_variant_iter)
    g_variant_iter_free (meta_variant_iter);
  if (data_variant_iter)
    g_variant_iter_free (data_variant_iter);
  return ret;
}

gboolean
ostree_repo_list_pack_indexes (OstreeRepo              *self,
                               GPtrArray              **out_meta_indexes,
                               GPtrArray              **out_data_indexes,
                               GCancellable            *cancellable,
                               GError                 **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  ot_lobj GFile *superindex_path = NULL;
  ot_lptrarray GPtrArray *ret_meta_indexes = NULL;
  ot_lptrarray GPtrArray *ret_data_indexes = NULL;

  superindex_path = g_file_get_child (priv->pack_dir, "index");

  if (g_file_query_exists (superindex_path, cancellable))
    {
      if (!list_pack_checksums_from_superindex_file (superindex_path, &ret_meta_indexes,
                                                     &ret_data_indexes,
                                                     cancellable, error))
        goto out;
    }
  else
    {
      ret_meta_indexes = g_ptr_array_new_with_free_func ((GDestroyNotify)g_free); 
      ret_data_indexes = g_ptr_array_new_with_free_func ((GDestroyNotify)g_free); 
    }

  ret = TRUE;
  ot_transfer_out_value (out_meta_indexes, &ret_meta_indexes);
  ot_transfer_out_value (out_data_indexes, &ret_data_indexes);
 out:
  return ret;
}

static gboolean
create_index_bloom (OstreeRepo          *self,
                    const char          *pack_checksum,
                    GVariant           **out_bloom,
                    GCancellable        *cancellable,
                    GError             **error)
{
  gboolean ret = FALSE;
  ot_lvariant GVariant *ret_bloom;

  /* TODO - define and compute bloom filter */

  ret_bloom = ot_gvariant_new_bytearray (NULL, 0);
  g_variant_ref_sink (ret_bloom);

  ret = TRUE;
  ot_transfer_out_value (out_bloom, &ret_bloom);
  /* out: */
  return ret;
}

static gboolean
append_index_builder (OstreeRepo           *self,
                      GPtrArray            *indexes,
                      GVariantBuilder      *builder,
                      GCancellable         *cancellable,
                      GError              **error)
{
  gboolean ret = FALSE;
  guint i;

  for (i = 0; i < indexes->len; i++)
    {
      const char *pack_checksum = indexes->pdata[i];
      ot_lvariant GVariant *bloom = NULL;

      if (!create_index_bloom (self, pack_checksum, &bloom, cancellable, error))
        goto out;

      g_variant_builder_add (builder,
                             "(@ay@ay)",
                             ostree_checksum_to_bytes_v (pack_checksum),
                             bloom);
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * Regenerate the pack superindex file based on the set of pack
 * indexes currently in the filesystem.
 */
gboolean
ostree_repo_regenerate_pack_index (OstreeRepo       *self,
                                   GCancellable     *cancellable,
                                   GError          **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  ot_lobj GFile *superindex_path = NULL;
  ot_lptrarray GPtrArray *pack_indexes = NULL;
  ot_lvariant GVariant *superindex_variant = NULL;
  GVariantBuilder *meta_index_content_builder = NULL;
  GVariantBuilder *data_index_content_builder = NULL;

  superindex_path = g_file_get_child (priv->pack_dir, "index");

  ot_clear_ptrarray (&pack_indexes);
  if (!list_pack_indexes_from_dir (self, TRUE, &pack_indexes,
                                   cancellable, error))
    goto out;
  meta_index_content_builder = g_variant_builder_new (G_VARIANT_TYPE ("a(ayay)"));
  if (!append_index_builder (self, pack_indexes, meta_index_content_builder,
                             cancellable, error))
    goto out;

  ot_clear_ptrarray (&pack_indexes);
  if (!list_pack_indexes_from_dir (self, FALSE, &pack_indexes,
                                   cancellable, error))
    goto out;
  data_index_content_builder = g_variant_builder_new (G_VARIANT_TYPE ("a(ayay)"));
  if (!append_index_builder (self, pack_indexes, data_index_content_builder,
                             cancellable, error))
    goto out;

  superindex_variant = g_variant_new ("(s@a{sv}@a(ayay)@a(ayay))",
                                      "OSTv0SUPERPACKINDEX",
                                      g_variant_new_array (G_VARIANT_TYPE ("{sv}"),
                                                           NULL, 0),
                                      g_variant_builder_end (meta_index_content_builder),
                                      g_variant_builder_end (data_index_content_builder));
  g_variant_ref_sink (superindex_variant);

  if (!ot_util_variant_save (superindex_path, superindex_variant,
                             cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (meta_index_content_builder)
    g_variant_builder_unref (meta_index_content_builder);
  if (data_index_content_builder)
    g_variant_builder_unref (data_index_content_builder);
  return ret;
}

static GFile *
get_pack_index_path (GFile            *parent,
                     gboolean          is_meta,
                     const char       *checksum)
{
  char *path = ostree_get_pack_index_name (is_meta, checksum);
  GFile *ret = g_file_resolve_relative_path (parent, path);
  g_free (path);
  return ret;
}

static GFile *
get_pack_data_path (GFile            *parent,
                    gboolean          is_meta,
                    const char       *checksum)
{
  char *path = ostree_get_pack_data_name (is_meta, checksum);
  GFile *ret = g_file_resolve_relative_path (parent, path);
  g_free (path);
  return ret;
}

gboolean
ostree_repo_add_pack_file (OstreeRepo       *self,
                           const char       *pack_checksum,
                           gboolean          is_meta,
                           GFile            *index_path,
                           GFile            *data_path,
                           GCancellable     *cancellable,
                           GError          **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  ot_lobj GFile *pack_index_path = NULL;
  ot_lobj GFile *pack_data_path = NULL;

  if (!ot_gfile_ensure_directory (priv->pack_dir, FALSE, error))
    goto out;

  pack_data_path = get_pack_data_path (priv->pack_dir, is_meta, pack_checksum);
  if (!ot_gfile_rename (data_path, pack_data_path, cancellable, error))
    goto out;

  pack_index_path = get_pack_index_path (priv->pack_dir, is_meta, pack_checksum);
  if (!ot_gfile_rename (index_path, pack_index_path, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
ensure_remote_cache_dir (OstreeRepo       *self,
                         const char       *remote_name,
                         GFile           **out_cache_dir,
                         GCancellable     *cancellable,
                         GError          **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  ot_lobj GFile *ret_cache_dir = NULL;

  ret_cache_dir = g_file_get_child (priv->remote_cache_dir, remote_name);
  
  if (!ot_gfile_ensure_directory (ret_cache_dir, FALSE, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_cache_dir, &ret_cache_dir);
 out:
  return ret;
}

static gboolean
delete_no_longer_referenced (OstreeRepo                   *self,
                             GFile                        *cache_path,
                             const char                   *prefix,
                             const char                   *suffix,
                             GHashTable                   *new_files,
                             GPtrArray                    *inout_cached,
                             GCancellable                 *cancellable,
                             GError                      **error)
{
  gboolean ret = FALSE;
  guint i;
  ot_lptrarray GPtrArray *current_files = NULL;
  ot_lfree char *pack_checksum = NULL;

  if (!list_files_in_dir_matching (cache_path,
                                   prefix, suffix,
                                   &current_files, 
                                   cancellable, error))
    goto out;
  for (i = 0; i < current_files->len; i++)
    {
      GFile *file = current_files->pdata[i];
      
      g_free (pack_checksum);
      pack_checksum = get_checksum_from_pack_name (ot_gfile_get_basename_cached (file));
      
      if (!g_hash_table_lookup (new_files, pack_checksum))
        {
          if (!ot_gfile_unlink (file, cancellable, error))
            goto out;
        }
      
      if (inout_cached)
        {
          g_ptr_array_add (inout_cached, pack_checksum);
          pack_checksum = NULL; /* transfer ownership */
        }
    }
  ret = TRUE;
 out:
  return ret;
}

static void
gather_uncached (GHashTable   *new_files,
                 GPtrArray    *cached,
                 GPtrArray    *inout_uncached)
{
  guint i;
  GHashTableIter hash_iter;
  gpointer key, value;

  g_hash_table_iter_init (&hash_iter, new_files);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *cur_pack_checksum = key;
      gboolean found = FALSE;

      for (i = 0; i < cached->len; i++)
        {
          const char *checksum = cached->pdata[i];
          if (strcmp (cur_pack_checksum, checksum) == 0)
            {
              found = TRUE;
              break;
            }
        }
      
      if (!found)
        g_ptr_array_add (inout_uncached, g_strdup (cur_pack_checksum));
    }
}

/**
 * Take a pack superindex file @superindex_path, and clean up any
 * no-longer-referenced pack files in the lookaside cache for
 * @remote_name.  The updated index file will also be saved into the
 * cache.
 *
 * Upon successful return, @out_cached_indexes will hold checksum
 * strings for indexes which are already in the cache, and
 * @out_uncached_indexes will hold strings for those which are not.
 */
gboolean
ostree_repo_resync_cached_remote_pack_indexes (OstreeRepo       *self,
                                               const char       *remote_name,
                                               GFile            *superindex_path,
                                               GPtrArray       **out_cached_meta_indexes,
                                               GPtrArray       **out_cached_data_indexes,
                                               GPtrArray       **out_uncached_meta_indexes,
                                               GPtrArray       **out_uncached_data_indexes,
                                               GCancellable     *cancellable,
                                               GError          **error)
{
  gboolean ret = FALSE;
  ot_lvariant GVariant *superindex_variant = NULL;
  ot_lobj GFile *cache_path = NULL;
  ot_lobj GFile *superindex_cache_path = NULL;
  ot_lptrarray GPtrArray *meta_index_files = NULL;
  ot_lptrarray GPtrArray *data_index_files = NULL;
  ot_lptrarray GPtrArray *meta_data_files = NULL;
  ot_lptrarray GPtrArray *data_data_files = NULL;
  ot_lhash GHashTable *new_pack_meta_indexes = NULL;
  ot_lhash GHashTable *new_pack_data_indexes = NULL;
  ot_lptrarray GPtrArray *ret_cached_meta_indexes = NULL;
  ot_lptrarray GPtrArray *ret_cached_data_indexes = NULL;
  ot_lptrarray GPtrArray *ret_uncached_meta_indexes = NULL;
  ot_lptrarray GPtrArray *ret_uncached_data_indexes = NULL;
  ot_lvariant GVariant *csum_bytes = NULL;
  ot_lvariant GVariant *bloom = NULL;
  ot_lfree char *pack_checksum = NULL;
  GVariantIter *superindex_contents_iter = NULL;

  if (!ensure_remote_cache_dir (self, remote_name, &cache_path, cancellable, error))
    goto out;

  ret_cached_meta_indexes = g_ptr_array_new_with_free_func (g_free);
  ret_cached_data_indexes = g_ptr_array_new_with_free_func (g_free);
  ret_uncached_meta_indexes = g_ptr_array_new_with_free_func (g_free);
  ret_uncached_data_indexes = g_ptr_array_new_with_free_func (g_free);

  if (!ot_util_variant_map (superindex_path, OSTREE_PACK_SUPER_INDEX_VARIANT_FORMAT,
                            &superindex_variant, error))
    goto out;

  if (!ostree_validate_structureof_pack_superindex (superindex_variant, error))
    goto out;

  new_pack_meta_indexes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  new_pack_data_indexes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  g_variant_get_child (superindex_variant, 2, "a(ayay)",
                       &superindex_contents_iter);
  while (g_variant_iter_loop (superindex_contents_iter,
                              "(@ay@ay)", &csum_bytes, &bloom))
    {
      pack_checksum = ostree_checksum_from_bytes_v (csum_bytes);
      g_hash_table_insert (new_pack_meta_indexes, pack_checksum, pack_checksum);
      pack_checksum = NULL; /* transfer ownership */
    }

  g_variant_get_child (superindex_variant, 3, "a(ayay)",
                       &superindex_contents_iter);
  while (g_variant_iter_loop (superindex_contents_iter,
                              "(@ay@ay)", &csum_bytes, &bloom))
    {
      pack_checksum = ostree_checksum_from_bytes_v (csum_bytes);
      g_hash_table_insert (new_pack_data_indexes, pack_checksum, pack_checksum);
      pack_checksum = NULL; /* transfer ownership */
    }

  if (!delete_no_longer_referenced (self, cache_path,
                                    "ostmetapack-", ".index",
                                    new_pack_meta_indexes,
                                    ret_cached_meta_indexes,
                                    cancellable, error))
    goto out;

  if (!delete_no_longer_referenced (self, cache_path,
                                    "ostdatapack-", ".index",
                                    new_pack_data_indexes,
                                    ret_cached_data_indexes,
                                    cancellable, error))
    goto out;

  gather_uncached (new_pack_meta_indexes, ret_cached_meta_indexes, ret_uncached_meta_indexes);
  gather_uncached (new_pack_data_indexes, ret_cached_data_indexes, ret_uncached_data_indexes);
  
  superindex_cache_path = g_file_get_child (cache_path, "index");
  if (!ot_util_variant_save (superindex_cache_path, superindex_variant, cancellable, error))
    goto out;

  /* Now also delete stale pack files */

  if (!delete_no_longer_referenced (self, cache_path,
                                    "ostmetapack-", ".data",
                                    new_pack_meta_indexes, NULL,
                                    cancellable, error))
    goto out;
  if (!delete_no_longer_referenced (self, cache_path,
                                    "ostdatapack-", ".data",
                                    new_pack_data_indexes, NULL,
                                    cancellable, error))
    goto out;
      
  ret = TRUE;
  ot_transfer_out_value (out_cached_meta_indexes, &ret_cached_meta_indexes);
  ot_transfer_out_value (out_cached_data_indexes, &ret_cached_data_indexes);
  ot_transfer_out_value (out_uncached_meta_indexes, &ret_uncached_data_indexes);
  ot_transfer_out_value (out_uncached_data_indexes, &ret_uncached_data_indexes);
 out:
  if (superindex_contents_iter)
    g_variant_iter_free (superindex_contents_iter);
  return ret;
}

gboolean
ostree_repo_clean_cached_remote_pack_data (OstreeRepo       *self,
                                           const char       *remote_name,
                                           GCancellable     *cancellable,
                                           GError          **error)
{
  gboolean ret = FALSE;
  guint i;
  ot_lobj GFile *cache_path = NULL;
  ot_lptrarray GPtrArray *data_files = NULL;

  if (!ensure_remote_cache_dir (self, remote_name, &cache_path, cancellable, error))
    goto out;

  if (!list_files_in_dir_matching (cache_path,
                                   "ostmetapack-", ".data",
                                   &data_files, 
                                   cancellable, error))
    goto out;
  for (i = 0; i < data_files->len; i++)
    {
      GFile *data_file = data_files->pdata[i];
      
      if (!ot_gfile_unlink (data_file, cancellable, error))
        goto out;
    }

  ot_clear_ptrarray (&data_files);
  if (!list_files_in_dir_matching (cache_path,
                                   "ostdatapack-", ".data",
                                   &data_files, 
                                   cancellable, error))
    goto out;
  for (i = 0; i < data_files->len; i++)
    {
      GFile *data_file = data_files->pdata[i];
      
      if (!ot_gfile_unlink (data_file, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * Load the index for pack @pack_checksum from cache directory for
 * @remote_name.
 */
gboolean
ostree_repo_map_cached_remote_pack_index (OstreeRepo       *self,
                                          const char       *remote_name,
                                          const char       *pack_checksum,
                                          gboolean          is_meta,
                                          GVariant        **out_variant,
                                          GCancellable     *cancellable,
                                          GError          **error)
{
  gboolean ret = FALSE;
  ot_lvariant GVariant *ret_variant = NULL;
  ot_lobj GFile *cache_dir = NULL;
  ot_lobj GFile *cached_pack_path = NULL;

  if (!ensure_remote_cache_dir (self, remote_name, &cache_dir,
                                cancellable, error))
    goto out;

  cached_pack_path = get_pack_index_path (cache_dir, is_meta, pack_checksum);
  if (!ot_util_variant_map (cached_pack_path, OSTREE_PACK_INDEX_VARIANT_FORMAT,
                            &ret_variant, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_variant, &ret_variant);
 out:
  return ret;
}

/**
 * The variable @cached_path should refer to a file containing a pack
 * index.  It will be validated and added to the cache directory for
 * @remote_name.
 */
gboolean
ostree_repo_add_cached_remote_pack_index (OstreeRepo       *self,
                                          const char       *remote_name,
                                          const char       *pack_checksum,
                                          gboolean          is_meta,
                                          GFile            *cached_path,
                                          GCancellable     *cancellable,
                                          GError          **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *cachedir = NULL;
  ot_lobj GFile *target_path = NULL;
  ot_lvariant GVariant *input_index_variant = NULL;
  ot_lvariant GVariant *output_index_variant = NULL;

  if (!map_variant_file_check_header_string (cached_path,
                                             OSTREE_PACK_INDEX_VARIANT_FORMAT,
                                             "OSTv0PACKINDEX",
                                             &input_index_variant,
                                             cancellable, error))
    goto out;

  if (!ostree_validate_structureof_pack_index (input_index_variant, error))
    goto out;

  output_index_variant = g_variant_get_normal_form (input_index_variant);
  
  if (!ensure_remote_cache_dir (self, remote_name, &cachedir, cancellable, error))
    goto out;
  
  target_path = get_pack_index_path (cachedir, is_meta, pack_checksum);
  if (!ot_util_variant_save (target_path, output_index_variant, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

/**
 * Check for availability of the pack index pointing to @pack_checksum
 * in the lookaside cache for @remote_name.  If not found, then the
 * output parameter @out_cached_path will be %NULL.
 */
gboolean
ostree_repo_get_cached_remote_pack_data (OstreeRepo       *self,
                                         const char       *remote_name,
                                         const char       *pack_checksum,
                                         gboolean          is_meta,
                                         GFile           **out_cached_path,
                                         GCancellable     *cancellable,
                                         GError          **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *cache_dir = NULL;
  ot_lobj GFile *cached_pack_path = NULL;
  ot_lobj GFile *ret_cached_path = NULL;

  if (!ensure_remote_cache_dir (self, remote_name, &cache_dir,
                                cancellable, error))
    goto out;

  cached_pack_path = get_pack_data_path (cache_dir, is_meta, pack_checksum);
  if (g_file_query_exists (cached_pack_path, cancellable))
    {
      ret_cached_path = cached_pack_path;
      cached_pack_path = NULL;
    }

  ret = TRUE;
  ot_transfer_out_value (out_cached_path, &ret_cached_path);
 out:
  return ret;
}

/**
 * Add file @cached_path into the cache for given @remote_name.
 *
 * <note>
 *   This unlinks @cached_path.
 * </note>
 */
gboolean
ostree_repo_take_cached_remote_pack_data (OstreeRepo       *self,
                                          const char       *remote_name,
                                          const char       *pack_checksum,
                                          gboolean          is_meta,
                                          GFile            *cached_path,
                                          GCancellable     *cancellable,
                                          GError          **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *cachedir = NULL;
  ot_lobj GFile *target_path = NULL;

  if (!ensure_remote_cache_dir (self, remote_name, &cachedir, cancellable, error))
    goto out;

  target_path = get_pack_data_path (cachedir, is_meta, pack_checksum);
  if (!ot_gfile_rename (cached_path, target_path, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static GVariant *
create_tree_variant_from_hashes (GHashTable            *file_checksums,
                                 GHashTable            *dir_contents_checksums,
                                 GHashTable            *dir_metadata_checksums)
{
  GHashTableIter hash_iter;
  gpointer key, value;
  GVariantBuilder files_builder;
  GVariantBuilder dirs_builder;
  GSList *sorted_filenames = NULL;
  GSList *iter;
  GVariant *serialized_tree;

  g_variant_builder_init (&files_builder, G_VARIANT_TYPE ("a(say)"));
  g_variant_builder_init (&dirs_builder, G_VARIANT_TYPE ("a(sayay)"));

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
      g_variant_builder_add (&files_builder, "(s@ay)", name,
                             ostree_checksum_to_bytes_v (value));
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
      const char *content_checksum;
      const char *meta_checksum;

      content_checksum = g_hash_table_lookup (dir_contents_checksums, name);
      meta_checksum = g_hash_table_lookup (dir_metadata_checksums, name);

      g_variant_builder_add (&dirs_builder, "(s@ay@ay)",
                             name,
                             ostree_checksum_to_bytes_v (content_checksum),
                             ostree_checksum_to_bytes_v (meta_checksum));
    }

  g_slist_free (sorted_filenames);
  sorted_filenames = NULL;

  serialized_tree = g_variant_new ("(@a(say)@a(sayay))",
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
  
  return ret;
}

static OstreeRepoCommitFilterResult
apply_commit_filter (OstreeRepo            *self,
                     OstreeRepoCommitModifier *modifier,
                     GPtrArray                *path,
                     GFileInfo                *file_info,
                     GFileInfo               **out_modified_info)
{
  GString *path_buf;
  guint i;
  OstreeRepoCommitFilterResult result;
  GFileInfo *modified_info;
  
  if (modifier == NULL || modifier->filter == NULL)
    {
      *out_modified_info = g_object_ref (file_info);
      return OSTREE_REPO_COMMIT_FILTER_ALLOW;
    }

  path_buf = g_string_new ("");

  if (path->len == 0)
    g_string_append_c (path_buf, '/');
  else
    {
      for (i = 0; i < path->len; i++)
        {
          const char *elt = path->pdata[i];
          
          g_string_append_c (path_buf, '/');
          g_string_append (path_buf, elt);
        }
    }

  modified_info = g_file_info_dup (file_info);
  result = modifier->filter (self, path_buf->str, modified_info, modifier->user_data);
  *out_modified_info = modified_info;

  g_string_free (path_buf, TRUE);
  return result;
}

static gboolean
stage_directory_to_mtree_internal (OstreeRepo           *self,
                                   GFile                *dir,
                                   OstreeMutableTree    *mtree,
                                   OstreeRepoCommitModifier *modifier,
                                   GPtrArray             *path,
                                   GCancellable         *cancellable,
                                   GError              **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  gboolean repo_dir_was_empty = FALSE;
  OstreeRepoCommitFilterResult filter_result;
  ot_lobj OstreeRepoFile *repo_dir = NULL;
  ot_lobj GFileInfo *child_info = NULL;
  ot_lobj OstreeMutableTree *child_mtree = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFileInfo *modified_info = NULL;
  ot_lobj GFile *child = NULL;
  ot_lvariant GVariant *xattrs = NULL;
  ot_lobj GInputStream *file_input = NULL;
  ot_lfree guchar *child_file_csum = NULL;
  ot_lfree char *tmp_checksum = NULL;

  /* We can only reuse checksums directly if there's no modifier */
  if (OSTREE_IS_REPO_FILE (dir) && modifier == NULL)
    repo_dir = (OstreeRepoFile*)g_object_ref (dir);

  if (repo_dir)
    {
      if (!ostree_repo_file_ensure_resolved (repo_dir, error))
        goto out;

      ostree_mutable_tree_set_metadata_checksum (mtree, ostree_repo_file_get_checksum (repo_dir));
      repo_dir_was_empty = 
        g_hash_table_size (ostree_mutable_tree_get_files (mtree)) == 0
        && g_hash_table_size (ostree_mutable_tree_get_subdirs (mtree)) == 0;

      filter_result = OSTREE_REPO_COMMIT_FILTER_ALLOW;
    }
  else
    {
      child_info = g_file_query_info (dir, OSTREE_GIO_FAST_QUERYINFO,
                                      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                      cancellable, error);
      if (!child_info)
        goto out;
      
      filter_result = apply_commit_filter (self, modifier, path, child_info, &modified_info);

      if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
        {
          if (!(modifier && modifier->skip_xattrs))
            {
              if (!ostree_get_xattrs_for_file (dir, &xattrs, cancellable, error))
                goto out;
            }
          
          if (!stage_directory_meta (self, modified_info, xattrs, &child_file_csum,
                                     cancellable, error))
            goto out;
          
          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
          ostree_mutable_tree_set_metadata_checksum (mtree, tmp_checksum);
          
          g_clear_object (&child_info);
          g_clear_object (&modified_info);
        }
    }

  if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
    {
      dir_enum = g_file_enumerate_children ((GFile*)dir, OSTREE_GIO_FAST_QUERYINFO, 
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            cancellable, 
                                            error);
      if (!dir_enum)
        goto out;

      while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
        {
          const char *name = g_file_info_get_name (child_info);

          g_clear_object (&modified_info);
          g_ptr_array_add (path, (char*)name);
          filter_result = apply_commit_filter (self, modifier, path, child_info, &modified_info);

          if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
            {
              g_clear_object (&child);
              child = g_file_get_child (dir, name);

              if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
                {
                  g_clear_object (&child_mtree);
                  if (!ostree_mutable_tree_ensure_dir (mtree, name, &child_mtree, error))
                    goto out;

                  if (!stage_directory_to_mtree_internal (self, child, child_mtree,
                                                          modifier, path, cancellable, error))
                    goto out;
                }
              else if (repo_dir)
                {
                  if (!ostree_mutable_tree_replace_file (mtree, name, 
                                                         ostree_repo_file_get_checksum ((OstreeRepoFile*) child),
                                                         error))
                    goto out;
                }
              else
                {
                  g_clear_object (&file_input);
                  if (g_file_info_get_file_type (modified_info) == G_FILE_TYPE_REGULAR)
                    {
                      file_input = (GInputStream*)g_file_read (child, cancellable, error);
                      if (!file_input)
                        goto out;
                    }

                  if (!(modifier && modifier->skip_xattrs))
                    {
                      ot_clear_gvariant (&xattrs);
                      if (!ostree_get_xattrs_for_file (child, &xattrs, cancellable, error))
                        goto out;
                    }

                  g_free (child_file_csum);
                  child_file_csum = NULL;
                  if (!stage_object_impl (self, OSTREE_OBJECT_TYPE_RAW_FILE, FALSE,
                                          modified_info, xattrs, file_input, NULL,
                                          &child_file_csum, cancellable, error))
                    goto out;

                  g_free (tmp_checksum);
                  tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
                  if (!ostree_mutable_tree_replace_file (mtree, name, tmp_checksum,
                                                         error))
                    goto out;
                }

              g_ptr_array_remove_index (path, path->len - 1);
            }

          g_clear_object (&child_info);
        }
      if (temp_error != NULL)
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  if (repo_dir && repo_dir_was_empty)
    ostree_mutable_tree_set_contents_checksum (mtree, ostree_repo_file_tree_get_content_checksum (repo_dir));

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_stage_directory_to_mtree (OstreeRepo           *self,
                                      GFile                *dir,
                                      OstreeMutableTree    *mtree,
                                      OstreeRepoCommitModifier *modifier,
                                      GCancellable         *cancellable,
                                      GError              **error)
{
  gboolean ret = FALSE;
  GPtrArray *path = NULL;

  path = g_ptr_array_new ();
  if (!stage_directory_to_mtree_internal (self, dir, mtree, modifier, path, cancellable, error))
    goto out;
  
  ret = TRUE;
 out:
  if (path)
    g_ptr_array_free (path, TRUE);
  return ret;
}

gboolean
ostree_repo_stage_mtree (OstreeRepo           *self,
                         OstreeMutableTree    *mtree,
                         char                **out_contents_checksum,
                         GCancellable         *cancellable,
                         GError              **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  const char *existing_checksum;
  ot_lfree char *ret_contents_checksum = NULL;
  ot_lhash GHashTable *dir_metadata_checksums = NULL;
  ot_lhash GHashTable *dir_contents_checksums = NULL;
  ot_lvariant GVariant *serialized_tree = NULL;
  ot_lfree guchar *contents_csum = NULL;

  existing_checksum = ostree_mutable_tree_get_contents_checksum (mtree);
  if (existing_checksum)
    {
      ret_contents_checksum = g_strdup (existing_checksum);
    }
  else
    {
      dir_contents_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free, (GDestroyNotify)g_free);
      dir_metadata_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free, (GDestroyNotify)g_free);
      
      g_hash_table_iter_init (&hash_iter, ostree_mutable_tree_get_subdirs (mtree));
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *name = key;
          const char *metadata_checksum;
          OstreeMutableTree *child_dir = value;
          char *child_dir_contents_checksum;

          if (!ostree_repo_stage_mtree (self, child_dir, &child_dir_contents_checksum,
                                        cancellable, error))
            goto out;
      
          g_assert (child_dir_contents_checksum);
          g_hash_table_replace (dir_contents_checksums, g_strdup (name),
                                child_dir_contents_checksum); /* Transfer ownership */
          metadata_checksum = ostree_mutable_tree_get_metadata_checksum (child_dir);
          g_assert (metadata_checksum);
          g_hash_table_replace (dir_metadata_checksums, g_strdup (name),
                                g_strdup (metadata_checksum));
        }
    
      serialized_tree = create_tree_variant_from_hashes (ostree_mutable_tree_get_files (mtree),
                                                         dir_contents_checksums,
                                                         dir_metadata_checksums);
      
      if (!stage_gvariant_object (self, OSTREE_OBJECT_TYPE_DIR_TREE,
                                  serialized_tree, &contents_csum,
                                  cancellable, error))
        goto out;
      ret_contents_checksum = ostree_checksum_from_bytes (contents_csum);
    }

  ret = TRUE;
  ot_transfer_out_value(out_contents_checksum, &ret_contents_checksum);
 out:
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
                              guchar              **out_csum,
                              GCancellable         *cancellable,
                              GError              **error)
{
  gboolean ret = FALSE;
  ot_lobj GInputStream *archive_stream = NULL;
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    archive_stream = ostree_libarchive_input_stream_new (a);
  
  if (!stage_object_impl (self, OSTREE_OBJECT_TYPE_RAW_FILE, FALSE,
                          file_info, NULL, archive_stream,
                          NULL, out_csum,
                          cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
stage_libarchive_entry_to_mtree (OstreeRepo           *self,
                                 OstreeMutableTree    *root,
                                 struct archive       *a,
                                 struct archive_entry *entry,
                                 OstreeRepoCommitModifier *modifier,
                                 const guchar         *tmp_dir_csum,
                                 GCancellable         *cancellable,
                                 GError              **error)
{
  gboolean ret = FALSE;
  const char *pathname;
  const char *hardlink;
  const char *basename;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lptrarray GPtrArray *split_path = NULL;
  ot_lptrarray GPtrArray *hardlink_split_path = NULL;
  ot_lobj OstreeMutableTree *subdir = NULL;
  ot_lobj OstreeMutableTree *parent = NULL;
  ot_lobj OstreeMutableTree *hardlink_source_parent = NULL;
  ot_lfree char *hardlink_source_checksum = NULL;
  ot_lobj OstreeMutableTree *hardlink_source_subdir = NULL;
  ot_lfree guchar *tmp_csum = NULL;
  ot_lfree char *tmp_checksum = NULL;

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
      if (tmp_dir_csum)
        {
          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes (tmp_dir_csum);
          if (!ostree_mutable_tree_ensure_parent_dirs (root, split_path,
                                                       tmp_checksum,
                                                       &parent,
                                                       error))
            goto out;
        }
      else
        {
          if (!ostree_mutable_tree_walk (root, split_path, 0, &parent, error))
            goto out;
        }
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

          if (!stage_directory_meta (self, file_info, NULL, &tmp_csum, cancellable, error))
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

          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes (tmp_csum);
          ostree_mutable_tree_set_metadata_checksum (subdir, tmp_checksum);
        }
      else 
        {
          if (parent == NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Can't import file as root");
              goto out;
            }

          if (!import_libarchive_entry_file (self, a, entry, file_info, &tmp_csum, cancellable, error))
            goto out;
          
          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes (tmp_csum);
          if (!ostree_mutable_tree_replace_file (parent, basename,
                                                 tmp_checksum,
                                                 error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}
#endif
                          
gboolean
ostree_repo_stage_archive_to_mtree (OstreeRepo                *self,
                                    GFile                     *archive_f,
                                    OstreeMutableTree         *root,
                                    OstreeRepoCommitModifier  *modifier,
                                    gboolean                   autocreate_parents,
                                    GCancellable             *cancellable,
                                    GError                  **error)
{
#ifdef HAVE_LIBARCHIVE
  gboolean ret = FALSE;
  struct archive *a = NULL;
  struct archive_entry *entry;
  int r;
  ot_lobj GFileInfo *tmp_dir_info = NULL;
  ot_lfree guchar *tmp_csum = NULL;

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

      if (autocreate_parents && !tmp_csum)
        {
          tmp_dir_info = g_file_info_new ();
          
          g_file_info_set_attribute_uint32 (tmp_dir_info, "unix::uid", archive_entry_uid (entry));
          g_file_info_set_attribute_uint32 (tmp_dir_info, "unix::gid", archive_entry_gid (entry));
          g_file_info_set_attribute_uint32 (tmp_dir_info, "unix::mode", 0755 | S_IFDIR);
          
          if (!stage_directory_meta (self, tmp_dir_info, NULL, &tmp_csum, cancellable, error))
            goto out;
        }

      if (!stage_libarchive_entry_to_mtree (self, root, a,
                                            entry, modifier,
                                            autocreate_parents ? tmp_csum : NULL,
                                            cancellable, error))
        goto out;
    }
  if (archive_read_close (a) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }

  ret = TRUE;
 out:
  if (a)
    (void)archive_read_close (a);
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
list_loose_object_dir (OstreeRepo             *self,
                       GFile                  *dir,
                       GHashTable             *inout_objects,
                       GCancellable           *cancellable,
                       GError                **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  const char *dirname = NULL;
  const char *dot = NULL;
  ot_lobj GFileEnumerator *enumerator = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  GString *checksum = NULL;

  dirname = ot_gfile_get_basename_cached (dir);

  /* We're only querying name */
  enumerator = g_file_enumerate_children (dir, "standard::name,standard::type", 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, 
                                          error);
  if (!enumerator)
    goto out;
  
  while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;
      OstreeObjectType objtype;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

      if (type == G_FILE_TYPE_DIRECTORY)
        goto loop_next;
      
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
        goto loop_next;
          
      dot = strrchr (name, '.');
      g_assert (dot);

      if ((dot - name) == 62)
        {
          GVariant *key, *value;

          if (checksum)
            g_string_free (checksum, TRUE);
          checksum = g_string_new (dirname);
          g_string_append_len (checksum, name, 62);
          
          key = ostree_object_name_serialize (checksum->str, objtype);
          value = g_variant_new ("(b@as)",
                                 TRUE, g_variant_new_strv (NULL, 0));
          /* transfer ownership */
          g_hash_table_replace (inout_objects, g_variant_ref_sink (key),
                                g_variant_ref_sink (value));
        }
    loop_next:
      g_clear_object (&file_info);
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
  if (checksum)
    g_string_free (checksum, TRUE);
  return ret;
}

static gboolean
list_loose_objects (OstreeRepo                     *self,
                    GHashTable                     *inout_objects,
                    GCancellable                   *cancellable,
                    GError                        **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  GError *temp_error = NULL;
  ot_lobj GFileEnumerator *enumerator = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GFile *objdir = NULL;

  enumerator = g_file_enumerate_children (priv->objects_dir, OSTREE_GIO_FAST_QUERYINFO, 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, 
                                          error);
  if (!enumerator)
    goto out;

  while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");
      
      if (strlen (name) == 2 && type == G_FILE_TYPE_DIRECTORY)
        {
          g_clear_object (&objdir);
          objdir = g_file_get_child (priv->objects_dir, name);
          if (!list_loose_object_dir (self, objdir, inout_objects, cancellable, error))
            goto out;
        }
      g_clear_object (&file_info);
    }
  if (file_info == NULL && temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  if (!g_file_enumerator_close (enumerator, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_load_pack_index (OstreeRepo    *self,
                             const char    *pack_checksum, 
                             gboolean       is_meta,
                             GVariant     **out_variant,
                             GCancellable  *cancellable,
                             GError       **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  ot_lvariant GVariant *ret_variant = NULL;
  ot_lobj GFile *path = NULL;
  
  ret_variant = g_hash_table_lookup (priv->pack_index_mappings, pack_checksum);
  if (ret_variant)
    {
      g_variant_ref (ret_variant);
    }
  else
    {
      path = get_pack_index_path (priv->pack_dir, is_meta, pack_checksum);
      if (!map_variant_file_check_header_string (path,
                                                 OSTREE_PACK_INDEX_VARIANT_FORMAT,
                                                 "OSTv0PACKINDEX",
                                                 &ret_variant,
                                                 cancellable, error))
        goto out;
      g_hash_table_insert (priv->pack_index_mappings, g_strdup (pack_checksum),
                           g_variant_ref (ret_variant));
    }

  ret = TRUE;
  ot_transfer_out_value (out_variant, &ret_variant);
 out:
  return ret;
}

/**
 * @sha256: Checksum of pack file
 * @out_data: (out): Pointer to pack file data
 *
 * Ensure that the given pack file is mapped into
 * memory.
 */
gboolean
ostree_repo_map_pack_file (OstreeRepo    *self,
                           const char    *pack_checksum,
                           gboolean       is_meta,
                           guchar       **out_data,
                           guint64       *out_len,
                           GCancellable  *cancellable,
                           GError       **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  gpointer ret_data;
  guint64 ret_len;
  GMappedFile *map = NULL;
  ot_lobj GFile *path = NULL;

  map = g_hash_table_lookup (priv->pack_data_mappings, pack_checksum);
  if (map == NULL)
    {
      path = get_pack_data_path (priv->pack_dir, is_meta, pack_checksum);

      map = g_mapped_file_new (ot_gfile_get_path_cached (path), FALSE, error);
      if (!map)
        goto out;

      g_hash_table_insert (priv->pack_data_mappings, g_strdup (pack_checksum), map);
      ret_data = g_mapped_file_get_contents (map);
    }

  ret_data = g_mapped_file_get_contents (map);
  ret_len = (guint64)g_mapped_file_get_length (map);

  ret = TRUE;
  if (out_data)
    *out_data = ret_data;
  if (out_len)
    *out_len = ret_len;
 out:
  return ret;
}


gboolean
ostree_repo_load_file (OstreeRepo         *self,
                       const char         *checksum,
                       GInputStream      **out_input,
                       GFileInfo         **out_file_info,
                       GVariant          **out_xattrs,
                       GCancellable       *cancellable,
                       GError            **error)
{
  gboolean ret = FALSE;
  guchar *content_pack_data;
  guint64 content_pack_len;
  guint64 content_pack_offset;
  ot_lvariant GVariant *packed_object = NULL;
  ot_lvariant GVariant *archive_meta = NULL;
  ot_lobj GFile *content_loose_path = NULL;
  ot_lobj GFileInfo *content_loose_info = NULL;
  ot_lfree char *content_pack_checksum = NULL;
  ot_lobj GInputStream *ret_input = NULL;
  ot_lobj GFileInfo *ret_file_info = NULL;
  ot_lvariant GVariant *ret_xattrs = NULL;

  if (ostree_repo_get_mode (self) == OSTREE_REPO_MODE_ARCHIVE)
    {
      /* First, read the metadata */
      if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META, checksum,
                                     &archive_meta, error))
        goto out;
      if (!ostree_parse_archived_file_meta (archive_meta, 
                                            &ret_file_info,
                                            &ret_xattrs,
                                            error))
        goto out;

      if (g_file_info_get_file_type (ret_file_info) == G_FILE_TYPE_REGULAR)
        {
          /* Blah, right now we need to look up the content too to get the file size */
          if (!ostree_repo_find_object (self, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT,
                                        checksum, &content_loose_path,
                                        &content_pack_checksum, &content_pack_offset,
                                        cancellable, error))
            goto out;
          
          if (content_loose_path)
            {
              content_loose_info = g_file_query_info (content_loose_path, OSTREE_GIO_FAST_QUERYINFO,
                                                      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
              if (!content_loose_info)
                goto out;
              
              g_file_info_set_attribute_uint64 (ret_file_info,
                                                "standard::size",
                                                g_file_info_get_attribute_uint64 (content_loose_info, "standard::size"));
            }
          /* fixme - don't have file size for packed =/ */
      
      /* Now, look for the content */
          if (out_input)
            {
              if (content_pack_checksum != NULL)
                {
                  if (!ostree_repo_map_pack_file (self, content_pack_checksum, FALSE,
                                                  &content_pack_data, &content_pack_len,
                                                  cancellable, error))
                    goto out;
                  if (!ostree_read_pack_entry_raw (content_pack_data, content_pack_len,
                                                   content_pack_offset, TRUE, FALSE,
                                                   &packed_object, cancellable, error))
                    goto out;
                  ret_input = ostree_read_pack_entry_as_stream (packed_object);
                }
              else if (content_loose_path != NULL)
                {
                  ret_input = (GInputStream*)g_file_read (content_loose_path, cancellable, error);
                  if (!ret_input)
                    goto out;
                }
              else
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "Couldn't find object '%s'", checksum);
                  goto out;
                }
            }
        }
    }
  else
    {
      content_loose_path = ostree_repo_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_RAW_FILE);
      ret_file_info = g_file_query_info (content_loose_path, OSTREE_GIO_FAST_QUERYINFO,
                                         G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
      if (!ret_file_info)
        goto out;

      /* Now, look for the content */
      if (g_file_info_get_file_type (ret_file_info) == G_FILE_TYPE_REGULAR
          && out_input)
        {
          ret_input = (GInputStream*)g_file_read (content_loose_path, cancellable, error);
          if (!ret_input)
            goto out;
        }
      if (out_xattrs)
        {
          if (!ostree_get_xattrs_for_file (content_loose_path, &ret_xattrs, cancellable,error))
            goto out;
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_input, &ret_input);
  ot_transfer_out_value (out_file_info, &ret_file_info);
  ot_transfer_out_value (out_xattrs, &ret_xattrs);
 out:
  return ret;
}

static gboolean
list_objects_in_index (OstreeRepo                     *self,
                       const char                     *pack_checksum,
                       gboolean                        is_meta,
                       GHashTable                     *inout_objects,
                       GCancellable                   *cancellable,
                       GError                        **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  guint32 objtype_u8;
  guint64 offset;
  ot_lobj GFile *index_path = NULL;
  ot_lvariant GVariant *index_variant = NULL;
  ot_lvariant GVariant *contents = NULL;
  ot_lvariant GVariant *csum_bytes = NULL;
  ot_lfree char *checksum = NULL;
  GVariantIter content_iter;

  index_path = get_pack_index_path (priv->pack_dir, is_meta, pack_checksum);

  if (!ostree_repo_load_pack_index (self, pack_checksum, is_meta, 
                                    &index_variant, cancellable, error))
    goto out;

  contents = g_variant_get_child_value (index_variant, 2);
  g_variant_iter_init (&content_iter, contents);

  while (g_variant_iter_loop (&content_iter, "(y@ayt)", &objtype_u8, &csum_bytes, &offset))
    {
      GVariant *obj_key;
      GVariant *objdata;
      OstreeObjectType objtype;
      GVariantBuilder pack_contents_builder;
      gboolean is_loose;

      objtype = (OstreeObjectType) objtype_u8;
      offset = GUINT64_FROM_BE (offset);

      g_variant_builder_init (&pack_contents_builder,
                              G_VARIANT_TYPE_STRING_ARRAY);
      
      g_free (checksum);
      checksum = ostree_checksum_from_bytes_v (csum_bytes);
      obj_key = ostree_object_name_serialize (checksum, objtype);
      ot_util_variant_take_ref (obj_key);

      objdata = g_hash_table_lookup (inout_objects, obj_key);
      if (!objdata)
        {
          is_loose = FALSE;
        }
      else
        {
          GVariantIter *current_packs_iter;
          const char *current_pack_checksum;

          g_variant_get (objdata, "(bas)", &is_loose, &current_packs_iter);

          while (g_variant_iter_loop (current_packs_iter, "&s", &current_pack_checksum))
            {
              g_variant_builder_add (&pack_contents_builder, "s", current_pack_checksum);
            }
          g_variant_iter_free (current_packs_iter);
        }
      g_variant_builder_add (&pack_contents_builder, "s", pack_checksum);
      objdata = g_variant_new ("(b@as)", is_loose,
                               g_variant_builder_end (&pack_contents_builder));
      g_variant_ref_sink (objdata);
      g_hash_table_replace (inout_objects, obj_key, objdata);
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
list_packed_objects (OstreeRepo                     *self,
                     GHashTable                     *inout_objects,
                     GCancellable                   *cancellable,
                     GError                        **error)
{
  gboolean ret = FALSE;
  guint i;
  ot_lptrarray GPtrArray *meta_index_checksums = NULL;
  ot_lptrarray GPtrArray *data_index_checksums = NULL;

  if (!ostree_repo_list_pack_indexes (self, &meta_index_checksums, &data_index_checksums,
                                      cancellable, error))
    goto out;

  for (i = 0; i < meta_index_checksums->len; i++)
    {
      const char *checksum = meta_index_checksums->pdata[i];
      if (!list_objects_in_index (self, checksum, TRUE, inout_objects, cancellable, error))
        goto out;
    }
  
  for (i = 0; i < data_index_checksums->len; i++)
    {
      const char *checksum = data_index_checksums->pdata[i];
      if (!list_objects_in_index (self, checksum, FALSE, inout_objects, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
find_object_in_packs (OstreeRepo        *self,
                      const char        *checksum,
                      OstreeObjectType   objtype,
                      char             **out_pack_checksum,
                      guint64           *out_pack_offset,
                      GCancellable      *cancellable,
                      GError           **error)
{
  gboolean ret = FALSE;
  guint i;
  guint64 ret_pack_offset;
  gboolean is_meta;
  ot_lptrarray GPtrArray *index_checksums = NULL;
  ot_lfree char *ret_pack_checksum = NULL;
  ot_lvariant GVariant *csum_bytes = NULL;
  ot_lvariant GVariant *index_variant = NULL;

  csum_bytes = ostree_checksum_to_bytes_v (checksum);

  is_meta = OSTREE_OBJECT_TYPE_IS_META (objtype);

  if (is_meta)
    {
      if (!ostree_repo_list_pack_indexes (self, &index_checksums, NULL,
                                          cancellable, error))
        goto out;
    }
  else
    {
      if (!ostree_repo_list_pack_indexes (self, NULL, &index_checksums,
                                          cancellable, error))
        goto out;
    }

  for (i = 0; i < index_checksums->len; i++)
    {
      const char *pack_checksum = index_checksums->pdata[i];
      guint64 offset;

      ot_clear_gvariant (&index_variant);
      if (!ostree_repo_load_pack_index (self, pack_checksum, is_meta, &index_variant,
                                        cancellable, error))
        goto out;

      if (!ostree_pack_index_search (index_variant, csum_bytes, objtype, &offset))
        continue;

      ret_pack_checksum = g_strdup (pack_checksum);
      ret_pack_offset = offset;
      break;
    }

  ret = TRUE;
  ot_transfer_out_value (out_pack_checksum, &ret_pack_checksum);
  if (out_pack_offset)
    *out_pack_offset = ret_pack_offset;
 out:
  return ret;
}

gboolean      
ostree_repo_find_object (OstreeRepo           *self,
                         OstreeObjectType      objtype,
                         const char           *checksum,
                         GFile               **out_stored_path,
                         char                **out_pack_checksum,
                         guint64              *out_pack_offset,
                         GCancellable         *cancellable,
                         GError             **error)
{
  gboolean ret = FALSE;
  guint64 ret_pack_offset = 0;
  struct stat stbuf;
  ot_lobj GFile *object_path = NULL;
  ot_lobj GFile *ret_stored_path = NULL;
  ot_lfree char *ret_pack_checksum = NULL;

  object_path = ostree_repo_get_object_path (self, checksum, objtype);
  
  if (lstat (ot_gfile_get_path_cached (object_path), &stbuf) == 0)
    {
      ret_stored_path = object_path;
      object_path = NULL;
    }
  else
    {
      g_clear_object (&object_path);
    }

  if (out_pack_checksum)
    {
      if (!find_object_in_packs (self, checksum, objtype,
                                 &ret_pack_checksum, &ret_pack_offset,
                                 cancellable, error))
        goto out;
    }
  
  ret = TRUE;
  ot_transfer_out_value (out_stored_path, &ret_stored_path);
  ot_transfer_out_value (out_pack_checksum, &ret_pack_checksum);
  if (out_pack_offset)
    *out_pack_offset = ret_pack_offset;
out:
  return ret;
}

gboolean
ostree_repo_load_variant_c (OstreeRepo          *self,
                            OstreeObjectType     objtype,
                            const guchar        *csum, 
                            GVariant           **out_variant,
                            GError             **error)
{
  gboolean ret = FALSE;
  ot_lfree char *checksum = NULL;

  checksum = ostree_checksum_from_bytes (csum);

  if (!ostree_repo_load_variant (self, objtype, checksum, out_variant, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_load_variant (OstreeRepo  *self,
                          OstreeObjectType  objtype,
                          const char    *sha256, 
                          GVariant     **out_variant,
                          GError       **error)
{
  gboolean ret = FALSE;
  guchar *pack_data;
  guint64 pack_len;
  guint64 object_offset;
  GCancellable *cancellable = NULL;
  ot_lobj GFile *object_path = NULL;
  ot_lvariant GVariant *packed_object = NULL;
  ot_lvariant GVariant *ret_variant = NULL;
  ot_lfree char *pack_checksum = NULL;

  g_return_val_if_fail (OSTREE_OBJECT_TYPE_IS_META (objtype), FALSE);

  if (!ostree_repo_find_object (self, objtype, sha256, &object_path,
                                &pack_checksum, &object_offset,
                                cancellable, error))
    goto out;

  /* Prefer loose metadata for now */
  if (object_path != NULL)
    {
      if (!ot_util_variant_map (object_path, ostree_metadata_variant_type (objtype),
                                &ret_variant, error))
        goto out;
    }
  else if (pack_checksum != NULL)
    {
      if (!ostree_repo_map_pack_file (self, pack_checksum, TRUE, &pack_data, &pack_len,
                                      cancellable, error))
        goto out;
      
      if (!ostree_read_pack_entry_raw (pack_data, pack_len, object_offset,
                                       TRUE, TRUE, &packed_object, cancellable, error))
        goto out;

      if (!ostree_read_pack_entry_variant (packed_object, objtype, TRUE,
                                           &ret_variant, cancellable, error))
        goto out;
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No such metadata object %s.%s",
                   sha256, ostree_object_type_to_string (objtype));
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_variant, &ret_variant);
 out:
  return ret;
}

/**
 * ostree_repo_list_objects:
 * @self:
 * @flags:
 * @out_objects: (out): Map of serialized object name to variant data
 * @cancellable:
 * @error:
 *
 * This function synchronously enumerates all objects in the
 * repository, returning data in @out_objects.  @out_objects
 * maps from keys returned by ostree_object_name_serialize()
 * to #GVariant values of type %OSTREE_REPO_LIST_OBJECTS_VARIANT_TYPE.
 *
 * Returns: %TRUE on success, %FALSE on error, and @error will be set
 */ 
gboolean
ostree_repo_list_objects (OstreeRepo                  *self,
                          OstreeRepoListObjectsFlags   flags,
                          GHashTable                 **out_objects,
                          GCancellable                *cancellable,
                          GError                     **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  ot_lhash GHashTable *ret_objects = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (priv->inited, FALSE);
  
  ret_objects = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                       (GDestroyNotify) g_variant_unref,
                                       (GDestroyNotify) g_variant_unref);

  if (flags & OSTREE_REPO_LIST_OBJECTS_ALL)
    flags |= (OSTREE_REPO_LIST_OBJECTS_LOOSE | OSTREE_REPO_LIST_OBJECTS_PACKED);

  if (flags & OSTREE_REPO_LIST_OBJECTS_LOOSE)
    {
      if (!list_loose_objects (self, ret_objects, cancellable, error))
        goto out;
    }

  if (flags & OSTREE_REPO_LIST_OBJECTS_PACKED)
    {
      if (!list_packed_objects (self, ret_objects, cancellable, error))
        goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_objects, &ret_objects);
 out:
  return ret;
}

static gboolean
checkout_file_from_input (GFile          *file,
                          OstreeRepoCheckoutMode mode,
                          OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                          GFileInfo      *finfo,
                          GVariant       *xattrs,
                          GInputStream   *input,
                          GCancellable   *cancellable,
                          GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFile *dir = NULL;
  ot_lobj GFile *temp_file = NULL;
  ot_lobj GFileInfo *temp_info = NULL;

  if (mode == OSTREE_REPO_CHECKOUT_MODE_USER)
    {
      if (g_file_info_get_file_type (finfo) == G_FILE_TYPE_SPECIAL)
        return TRUE;

      temp_info = g_file_info_dup (finfo);
      
      g_file_info_set_attribute_uint32 (temp_info, "unix::uid", geteuid ());
      g_file_info_set_attribute_uint32 (temp_info, "unix::gid", getegid ());

      xattrs = NULL;
    }

  if (overwrite_mode == OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES)
    {
      if (g_file_info_get_file_type (temp_info ? temp_info : finfo) == G_FILE_TYPE_DIRECTORY)
        {
          if (!ostree_create_file_from_input (file, temp_info ? temp_info : finfo,
                                              xattrs, input,
                                              cancellable, &temp_error))
            {
              if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
                {
                  g_clear_error (&temp_error);
                }
              else
                {
                  g_propagate_error (error, temp_error);
                  goto out;
                }
            }
        }
      else
        {
          dir = g_file_get_parent (file);
          if (!ostree_create_temp_file_from_input (dir, NULL, "checkout",
                                                   temp_info ? temp_info : finfo,
                                                   xattrs, input, &temp_file, 
                                                   cancellable, error))
            goto out;
          
          if (rename (ot_gfile_get_path_cached (temp_file), ot_gfile_get_path_cached (file)) < 0)
            {
              ot_util_set_error_from_errno (error, errno);
              goto out;
            }
        }
    }
  else
    {
      if (!ostree_create_file_from_input (file, temp_info ? temp_info : finfo,
                                          xattrs, input, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
checkout_file_hardlink (OstreeRepo                  *self,
                        OstreeRepoCheckoutMode    mode,
                        OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                        GFile                    *source,
                        GFile                    *destination,
                        GCancellable             *cancellable,
                        GError                  **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *dir = NULL;
  ot_lobj GFile *temp_file = NULL;

  if (overwrite_mode == OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES)
    {
      dir = g_file_get_parent (destination);
      if (!ostree_create_temp_hardlink (dir, (GFile*)source, NULL, "link",
                                        &temp_file, cancellable, error))
        goto out;

      /* Idiocy, from man rename(2)
       *
       * "If oldpath and newpath are existing hard links referring to
       * the same file, then rename() does nothing, and returns a
       * success status."
       *
       * So we can't make this atomic.  
       */

      (void) unlink (ot_gfile_get_path_cached (destination));

      if (rename (ot_gfile_get_path_cached (temp_file),
                  ot_gfile_get_path_cached (destination)) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
      g_clear_object (&temp_file);
    }
  else
    {
      if (link (ot_gfile_get_path_cached (source), ot_gfile_get_path_cached (destination)) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  ret = TRUE;
 out:
  if (temp_file)
    (void) unlink (ot_gfile_get_path_cached (temp_file));
  return ret;
}

static gboolean
checkout_one_file (OstreeRepo                  *self,
                   OstreeRepoCheckoutMode    mode,
                   OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                   OstreeRepoFile           *src,
                   GFileInfo                *file_info,
                   GFile                    *destination,
                   GCancellable             *cancellable,
                   GError                  **error)
{
  gboolean ret = FALSE;
  OstreeRepoPrivate *priv = GET_PRIVATE (self);
  const char *checksum;
  struct stat stbuf;
  ot_lobj GFile *possible_loose_path = NULL;
  ot_lobj GInputStream *input = NULL;
  ot_lvariant GVariant *xattrs = NULL;

  checksum = ostree_repo_file_get_checksum ((OstreeRepoFile*)src);

  /* First check for a loose object */
  if (priv->mode == OSTREE_REPO_MODE_ARCHIVE && mode == OSTREE_REPO_CHECKOUT_MODE_USER)
    {
      possible_loose_path = ostree_repo_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT);
    }
  else if (priv->mode == OSTREE_REPO_MODE_BARE && mode == OSTREE_REPO_CHECKOUT_MODE_NONE)
    {
      possible_loose_path = ostree_repo_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_RAW_FILE);
    }

  if (possible_loose_path && lstat (ot_gfile_get_path_cached (possible_loose_path), &stbuf) >= 0)
    {
      /* If we found one, we can just hardlink */
      if (!checkout_file_hardlink (self, mode, overwrite_mode, possible_loose_path, destination,
                                   cancellable, error) < 0)
        goto out;
    }
  else
    {
      if (!ostree_repo_load_file (self, checksum, &input, NULL, &xattrs, cancellable, error))
        goto out;

      if (!checkout_file_from_input (destination, mode, overwrite_mode, file_info, xattrs, 
                                     input, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_checkout_tree (OstreeRepo               *self,
                           OstreeRepoCheckoutMode    mode,
                           OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                           GFile                    *destination,
                           OstreeRepoFile           *source,
                           GFileInfo                *source_info,
                           GCancellable             *cancellable,
                           GError                  **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lvariant GVariant *xattrs = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFile *src_child = NULL;
  ot_lobj GFile *dest_path = NULL;

  if (!ostree_repo_file_get_xattrs (source, &xattrs, NULL, error))
    goto out;

  if (!checkout_file_from_input (destination, mode, overwrite_mode, source_info,
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
          if (!ostree_repo_checkout_tree (self, mode, overwrite_mode,
                                          dest_path, (OstreeRepoFile*)src_child, file_info,
                                          cancellable, error))
            goto out;
        }
      else
        {
          if (!checkout_one_file (self, mode, overwrite_mode,
                                  (OstreeRepoFile*)src_child, file_info, 
                                  dest_path, cancellable, error))
            goto out;
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
  ot_lobj GFile *ret_root = NULL;
  ot_lfree char *resolved_rev = NULL;

  if (!ostree_repo_resolve_rev (self, rev, FALSE, &resolved_rev, error))
    goto out;

  ret_root = ostree_repo_file_new_root (self, resolved_rev);
  if (!ostree_repo_file_ensure_resolved ((OstreeRepoFile*)ret_root, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_root, &ret_root);
 out:
  return ret;
}
