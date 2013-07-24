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

#include "ostree-repo-private.h"
#include "ostree-mutable-tree.h"
#include "ostree-checksum-input-stream.h"
#include "otutil.h"
#include "ostree-repo-file-enumerator.h"

#include <gio/gunixoutputstream.h>
#include <gio/gunixinputstream.h>
#include <glib/gstdio.h>

#include <stdio.h>
#include <stdlib.h>

typedef struct {
  GObjectClass parent_class;
} OstreeRepoClass;

static gboolean      
repo_find_object (OstreeRepo           *self,
                  OstreeObjectType      objtype,
                  const char           *checksum,
                  GFile               **out_stored_path,
                  GCancellable         *cancellable,
                  GError             **error);

enum {
  PROP_0,

  PROP_PATH
};

G_DEFINE_TYPE (OstreeRepo, ostree_repo, G_TYPE_OBJECT)

static void
ostree_repo_finalize (GObject *object)
{
  OstreeRepo *self = OSTREE_REPO (object);

  g_clear_object (&self->parent_repo);

  g_clear_object (&self->repodir);
  g_clear_object (&self->tmp_dir);
  g_clear_object (&self->pending_dir);
  g_clear_object (&self->local_heads_dir);
  g_clear_object (&self->remote_heads_dir);
  g_clear_object (&self->objects_dir);
  g_clear_object (&self->uncompressed_objects_dir);
  g_clear_object (&self->remote_cache_dir);
  g_clear_object (&self->config_file);

  g_clear_object (&self->transaction_lock_path);

  if (self->loose_object_devino_hash)
    g_hash_table_destroy (self->loose_object_devino_hash);
  if (self->updated_uncompressed_dirs)
    g_hash_table_destroy (self->updated_uncompressed_dirs);
  if (self->config)
    g_key_file_free (self->config);
  g_clear_pointer (&self->cached_meta_indexes, (GDestroyNotify) g_ptr_array_unref);
  g_clear_pointer (&self->cached_content_indexes, (GDestroyNotify) g_ptr_array_unref);
  g_mutex_clear (&self->cache_lock);

  G_OBJECT_CLASS (ostree_repo_parent_class)->finalize (object);
}

static void
ostree_repo_set_property(GObject         *object,
			   guint            prop_id,
			   const GValue    *value,
			   GParamSpec      *pspec)
{
  OstreeRepo *self = OSTREE_REPO (object);

  switch (prop_id)
    {
    case PROP_PATH:
      /* Canonicalize */
      self->repodir = g_file_new_for_path (gs_file_get_path_cached (g_value_get_object (value)));
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

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_object (value, self->repodir);
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
  OstreeRepo *self;
  GObject *object;
  GObjectClass *parent_class;

  parent_class = G_OBJECT_CLASS (ostree_repo_parent_class);
  object = parent_class->constructor (gtype, n_properties, properties);
  self = (OstreeRepo*)object;

  g_assert (self->repodir != NULL);
  
  self->tmp_dir = g_file_resolve_relative_path (self->repodir, "tmp");
  self->pending_dir = g_file_resolve_relative_path (self->repodir, "tmp/pending");
  self->local_heads_dir = g_file_resolve_relative_path (self->repodir, "refs/heads");
  self->remote_heads_dir = g_file_resolve_relative_path (self->repodir, "refs/remotes");
  
  self->objects_dir = g_file_get_child (self->repodir, "objects");
  self->uncompressed_objects_dir = g_file_get_child (self->repodir, "uncompressed-objects-cache");
  self->remote_cache_dir = g_file_get_child (self->repodir, "remote-cache");
  self->config_file = g_file_get_child (self->repodir, "config");

  return object;
}

static void
ostree_repo_class_init (OstreeRepoClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

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
  g_mutex_init (&self->cache_lock);
}

OstreeRepo*
ostree_repo_new (GFile *path)
{
  return g_object_new (OSTREE_TYPE_REPO, "path", path, NULL);
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
  g_return_val_if_fail (self->inited, NULL);

  return self->config;
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
  GKeyFile *copy;
  char *data;
  gsize len;

  g_return_val_if_fail (self->inited, NULL);

  copy = g_key_file_new ();
  data = g_key_file_to_data (self->config, &len, NULL);
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
  gs_free char *data = NULL;
  gsize len;

  g_return_val_if_fail (self->inited, FALSE);

  data = g_key_file_to_data (new_config, &len, error);
  if (!g_file_replace_contents (self->config_file, data, len, NULL, FALSE, 0, NULL,
                                NULL, error))
    goto out;
  
  g_key_file_free (self->config);
  self->config = g_key_file_new ();
  if (!g_key_file_load_from_data (self->config, data, len, 0, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_mode_from_string (const char      *mode,
                              OstreeRepoMode  *out_mode,
                              GError         **error)
{
  gboolean ret = FALSE;
  OstreeRepoMode ret_mode;

  if (strcmp (mode, "bare") == 0)
    ret_mode = OSTREE_REPO_MODE_BARE;
  else if (strcmp (mode, "archive") == 0)
    ret_mode = OSTREE_REPO_MODE_ARCHIVE;
  else if (strcmp (mode, "archive-z2") == 0)
    ret_mode = OSTREE_REPO_MODE_ARCHIVE_Z2;
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid mode '%s' in repository configuration", mode);
      goto out;
    }

  ret = TRUE;
  *out_mode = ret_mode;
 out:
  return ret;
}

gboolean
ostree_repo_check (OstreeRepo *self, GError **error)
{
  gboolean ret = FALSE;
  gboolean is_archive;
  gs_free char *version = NULL;
  gs_free char *mode = NULL;
  gs_free char *parent_repo_path = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->inited)
    return TRUE;

  if (!g_file_test (gs_file_get_path_cached (self->objects_dir), G_FILE_TEST_IS_DIR))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't find objects directory '%s'",
                   gs_file_get_path_cached (self->objects_dir));
      goto out;
    }

  if (!gs_file_ensure_directory (self->pending_dir, FALSE, NULL, error))
    goto out;
  
  self->config = g_key_file_new ();
  if (!g_key_file_load_from_file (self->config, gs_file_get_path_cached (self->config_file), 0, error))
    {
      g_prefix_error (error, "Couldn't parse config file: ");
      goto out;
    }

  version = g_key_file_get_value (self->config, "core", "repo_version", error);
  if (!version)
    goto out;

  if (strcmp (version, "1") != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid repository version '%s'", version);
      goto out;
    }

  if (!ot_keyfile_get_boolean_with_default (self->config, "core", "archive",
                                            FALSE, &is_archive, error))
    goto out;
  
  if (is_archive)
    self->mode = OSTREE_REPO_MODE_ARCHIVE;
  else
    {
      if (!ot_keyfile_get_value_with_default (self->config, "core", "mode",
                                              "bare", &mode, error))
        goto out;

      if (!ostree_repo_mode_from_string (mode, &self->mode, error))
        goto out;
    }

  if (!ot_keyfile_get_value_with_default (self->config, "core", "parent",
                                          NULL, &parent_repo_path, error))
    goto out;

  if (parent_repo_path && parent_repo_path[0])
    {
      gs_unref_object GFile *parent_repo_f = g_file_new_for_path (parent_repo_path);

      self->parent_repo = ostree_repo_new (parent_repo_f);

      if (!ostree_repo_check (self->parent_repo, error))
        {
          g_prefix_error (error, "While checking parent repository '%s': ",
                          gs_file_get_path_cached (parent_repo_f));
          goto out;
        }
    }

  if (!ot_keyfile_get_boolean_with_default (self->config, "core", "enable-uncompressed-cache",
                                            TRUE, &self->enable_uncompressed_cache, error))
    goto out;

  self->inited = TRUE;
  
  ret = TRUE;
 out:
  return ret;
}

GFile *
ostree_repo_get_path (OstreeRepo  *self)
{
  return self->repodir;
}

GFile *
ostree_repo_get_tmpdir (OstreeRepo  *self)
{
  return self->tmp_dir;
}

OstreeRepoMode
ostree_repo_get_mode (OstreeRepo  *self)
{
  g_return_val_if_fail (self->inited, FALSE);

  return self->mode;
}

/**
 * ostree_repo_get_parent:
 * @self:
 * 
 * Before this function can be used, ostree_repo_init() must have been
 * called.
 *
 * Returns: (transfer none): Parent repository, or %NULL if none
 */
OstreeRepo *
ostree_repo_get_parent (OstreeRepo  *self)
{
  return self->parent_repo;
}

GFile *
ostree_repo_get_file_object_path (OstreeRepo   *self,
                                  const char   *checksum)
{
  return ostree_repo_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_FILE);
}

GFile *
ostree_repo_get_archive_content_path (OstreeRepo    *self,
                                      const char    *checksum)
{
  gs_free char *path = NULL;

  g_assert (ostree_repo_get_mode (self) == OSTREE_REPO_MODE_ARCHIVE);

  path = ostree_get_relative_archive_content_path (checksum);
  return g_file_resolve_relative_path (self->repodir, path);
}

static gboolean
commit_loose_object_impl (OstreeRepo        *self,
                          GFile             *tempfile_path,
                          GFile             *dest,
                          gboolean           is_regular,
                          GCancellable      *cancellable,
                          GError           **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *parent = NULL;

  parent = g_file_get_parent (dest);
  if (!gs_file_ensure_directory (parent, FALSE, cancellable, error))
    goto out;

  if (is_regular)
    {
      /* Ensure that in case of a power cut, these files have the data we
       * want.   See http://lwn.net/Articles/322823/
       */
      if (!gs_file_sync_data (tempfile_path, cancellable, error))
        goto out;
    }
  
  if (rename (gs_file_get_path_cached (tempfile_path), gs_file_get_path_cached (dest)) < 0)
    {
      if (errno != EEXIST)
        {
          ot_util_set_error_from_errno (error, errno);
          g_prefix_error (error, "Storing file '%s': ",
                          gs_file_get_path_cached (dest));
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
commit_loose_object_trusted (OstreeRepo        *self,
                             const char        *checksum,
                             OstreeObjectType   objtype,
                             GFile             *tempfile_path,
                             gboolean           is_regular,
                             GCancellable      *cancellable,
                             GError           **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *dest_file = NULL;

  dest_file = ostree_repo_get_object_path (self, checksum, objtype);

  if (!commit_loose_object_impl (self, tempfile_path, dest_file, is_regular,
                                 cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
stage_object (OstreeRepo         *self,
              OstreeObjectType    objtype,
              const char         *expected_checksum,
              GInputStream       *input,
              guint64             file_object_length,
              guchar            **out_csum,
              GCancellable       *cancellable,
              GError            **error)
{
  gboolean ret = FALSE;
  const char *actual_checksum;
  gboolean do_commit;
  OstreeRepoMode repo_mode;
  gs_unref_object GFileInfo *temp_info = NULL;
  gs_unref_object GFile *temp_file = NULL;
  gs_unref_object GFile *raw_temp_file = NULL;
  gs_unref_object GFile *stored_path = NULL;
  gs_free guchar *ret_csum = NULL;
  gs_unref_object OstreeChecksumInputStream *checksum_input = NULL;
  gboolean have_obj;
  GChecksum *checksum = NULL;
  gboolean staged_raw_file = FALSE;
  gboolean staged_archive_file = FALSE;
  gboolean temp_file_is_regular;

  g_return_val_if_fail (self->in_transaction, FALSE);
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  g_assert (expected_checksum || out_csum);

  if (expected_checksum)
    {
      if (!repo_find_object (self, objtype, expected_checksum, &stored_path,
                             cancellable, error))
        goto out;
    }

  repo_mode = ostree_repo_get_mode (self);

  if (out_csum)
    {
      checksum = g_checksum_new (G_CHECKSUM_SHA256);
      if (input)
        checksum_input = ostree_checksum_input_stream_new (input, checksum);
    }

  if (objtype == OSTREE_OBJECT_TYPE_FILE)
    {
      gs_unref_object GInputStream *file_input = NULL;
      gs_unref_object GFileInfo *file_info = NULL;
      gs_unref_variant GVariant *xattrs = NULL;

      if (!ostree_content_stream_parse (FALSE, checksum_input ? (GInputStream*)checksum_input : input,
                                        file_object_length, FALSE,
                                        &file_input, &file_info, &xattrs,
                                        cancellable, error))
        goto out;

      temp_file_is_regular = g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR;

      if (repo_mode == OSTREE_REPO_MODE_BARE)
        {
          if (!ostree_create_temp_file_from_input (self->tmp_dir,
                                                   ostree_object_type_to_string (objtype), NULL,
                                                   file_info, xattrs, file_input,
                                                   &temp_file,
                                                   cancellable, error))
            goto out;
          staged_raw_file = TRUE;
        }
      else if (repo_mode == OSTREE_REPO_MODE_ARCHIVE_Z2)
        {
          gs_unref_variant GVariant *file_meta = NULL;
          gs_unref_object GOutputStream *temp_out = NULL;
          gs_unref_object GConverter *zlib_compressor = NULL;
          gs_unref_object GOutputStream *compressed_out_stream = NULL;

          if (!ostree_create_temp_regular_file (self->tmp_dir,
                                                ostree_object_type_to_string (objtype), NULL,
                                                &temp_file, &temp_out,
                                                cancellable, error))
            goto out;
          temp_file_is_regular = TRUE;

          file_meta = ostree_zlib_file_header_new (file_info, xattrs);

          if (!ostree_write_variant_with_size (temp_out, file_meta, 0, NULL, NULL,
                                               cancellable, error))
            goto out;

          if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
            {
              zlib_compressor = (GConverter*)g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW, 9);
              compressed_out_stream = g_converter_output_stream_new (temp_out, zlib_compressor);
              
              if (g_output_stream_splice (compressed_out_stream, file_input,
                                          G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                          cancellable, error) < 0)
                goto out;
            }

          if (!g_output_stream_close (temp_out, cancellable, error))
            goto out;
        }
      else if (repo_mode == OSTREE_REPO_MODE_ARCHIVE)
        {
          gs_unref_variant GVariant *file_meta = NULL;
          gs_unref_object GInputStream *file_meta_input = NULL;
          gs_unref_object GFileInfo *archive_content_file_info = NULL;
          
          file_meta = ostree_file_header_new (file_info, xattrs);
          file_meta_input = ot_variant_read (file_meta);

          if (!ostree_create_temp_file_from_input (self->tmp_dir,
                                                   ostree_object_type_to_string (objtype), NULL,
                                                   NULL, NULL, file_meta_input,
                                                   &temp_file,
                                                   cancellable, error))
            goto out;

          if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
            {
              gs_unref_object GOutputStream *content_out = NULL;
              guint32 src_mode;
              guint32 target_mode;

              if (!ostree_create_temp_regular_file (self->tmp_dir,
                                                    ostree_object_type_to_string (objtype), NULL,
                                                    &raw_temp_file, &content_out,
                                                    cancellable, error))
                goto out;

              /* Don't make setuid files in the repository; all we want to preserve
               * is file type and permissions.
               */
              src_mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
              target_mode = src_mode & (S_IRWXU | S_IRWXG | S_IRWXO | S_IFMT);
              /* However, do ensure that archive mode files are
               * readable by all users.  This is important for serving
               * files via HTTP.
               */
              target_mode |= (S_IRUSR | S_IRGRP | S_IROTH);
              
              if (chmod (gs_file_get_path_cached (raw_temp_file), target_mode) < 0)
                {
                  ot_util_set_error_from_errno (error, errno);
                  goto out;
                }

              if (g_output_stream_splice (content_out, file_input,
                                          G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                          cancellable, error) < 0)
                goto out;

              staged_archive_file = TRUE;
            }
        }
      else
        g_assert_not_reached ();
    }
  else
    {
      if (!ostree_create_temp_file_from_input (self->tmp_dir,
                                               ostree_object_type_to_string (objtype), NULL,
                                               NULL, NULL,
                                               checksum_input ? (GInputStream*)checksum_input : input,
                                               &temp_file,
                                               cancellable, error))
        goto out;
      temp_file_is_regular = TRUE;
    }

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
          
  if (!ostree_repo_has_object (self, objtype, actual_checksum, &have_obj,
                               cancellable, error))
    goto out;
          
  do_commit = !have_obj;

  if (do_commit)
    {
      /* Only do this if we *didn't* stage a bare file above */
      if (!staged_raw_file
          && objtype == OSTREE_OBJECT_TYPE_FILE && self->mode == OSTREE_REPO_MODE_BARE)
        {
          gs_unref_object GInputStream *file_input = NULL;
          gs_unref_object GFileInfo *file_info = NULL;
          gs_unref_variant GVariant *xattrs = NULL;
          gboolean is_regular;
              
          if (!ostree_content_file_parse (FALSE, temp_file, FALSE, &file_input,
                                          &file_info, &xattrs,
                                          cancellable, error))
            goto out;
              
          if (!ostree_create_temp_file_from_input (self->tmp_dir,
                                                   ostree_object_type_to_string (objtype), NULL,
                                                   file_info, xattrs, file_input,
                                                   &raw_temp_file,
                                                   cancellable, error))
            goto out;

          is_regular = g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR;

          if (!commit_loose_object_trusted (self, actual_checksum, objtype, 
                                            raw_temp_file, is_regular, cancellable, error))
            goto out;
          g_clear_object (&raw_temp_file);
        }
      else
        {
          /* Commit content first so the process is atomic */
          if (staged_archive_file)
            {
              gs_unref_object GFile *archive_content_dest = NULL;

              archive_content_dest = ostree_repo_get_archive_content_path (self, actual_checksum);
                                                                   
              if (!commit_loose_object_impl (self, raw_temp_file, archive_content_dest, TRUE,
                                             cancellable, error))
                goto out;
              g_clear_object (&raw_temp_file);
            }
          if (!commit_loose_object_trusted (self, actual_checksum, objtype, temp_file, temp_file_is_regular,
                                            cancellable, error))
            goto out;
          g_clear_object (&temp_file);
        }
    }
      
  if (checksum)
    ret_csum = ot_csum_from_gchecksum (checksum);

  ret = TRUE;
  ot_transfer_out_value(out_csum, &ret_csum);
 out:
  if (temp_file)
    (void) unlink (gs_file_get_path_cached (temp_file));
  if (raw_temp_file)
    (void) unlink (gs_file_get_path_cached (raw_temp_file));
  g_clear_pointer (&checksum, (GDestroyNotify) g_checksum_free);
  return ret;
}

static gboolean
append_object_dirs_from (OstreeRepo          *self,
                         GFile               *dir,
                         GPtrArray           *object_dirs,
                         GCancellable        *cancellable,
                         GError             **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  gs_unref_object GFileEnumerator *enumerator = NULL;

  enumerator = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO, 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, 
                                          &temp_error);
  if (!enumerator)
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
          ret = TRUE;
        }
      else
        g_propagate_error (error, temp_error);

      goto out;
    }

  while (TRUE)
    {
      GFileInfo *file_info;
      const char *name;
      guint32 type;

      if (!gs_file_enumerator_iterate (enumerator, &file_info, NULL,
                                       NULL, error))
        goto out;
      if (file_info == NULL)
        break;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");
      
      if (strlen (name) == 2 && type == G_FILE_TYPE_DIRECTORY)
        {
          GFile *objdir = g_file_get_child (g_file_enumerator_get_container (enumerator), name);
          g_ptr_array_add (object_dirs, objdir);  /* transfer ownership */
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
get_loose_object_dirs (OstreeRepo       *self,
                       GPtrArray       **out_object_dirs,
                       GCancellable     *cancellable,
                       GError          **error)
{
  gboolean ret = FALSE;
  gs_unref_ptrarray GPtrArray *ret_object_dirs = NULL;

  ret_object_dirs = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  if (ostree_repo_get_mode (self) == OSTREE_REPO_MODE_ARCHIVE_Z2)
    {
      gs_unref_object GFile *cachedir = g_file_get_child (self->uncompressed_objects_dir, "objects");
      if (!append_object_dirs_from (self, cachedir, ret_object_dirs,
                                    cancellable, error))
        goto out;
    }

  if (!append_object_dirs_from (self, self->objects_dir, ret_object_dirs,
                                cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_object_dirs, &ret_object_dirs);
 out:
  return ret;
}

typedef struct {
  dev_t dev;
  ino_t ino;
} OstreeDevIno;

static guint
devino_hash (gconstpointer a)
{
  OstreeDevIno *a_i = (gpointer)a;
  return (guint) (a_i->dev + a_i->ino);
}

static int
devino_equal (gconstpointer   a,
              gconstpointer   b)
{
  OstreeDevIno *a_i = (gpointer)a;
  OstreeDevIno *b_i = (gpointer)b;
  return a_i->dev == b_i->dev
    && a_i->ino == b_i->ino;
}

static gboolean
scan_loose_devino (OstreeRepo                     *self,
                   GHashTable                     *devino_cache,
                   GCancellable                   *cancellable,
                   GError                        **error)
{
  gboolean ret = FALSE;
  guint i;
  OstreeRepoMode repo_mode;
  gs_unref_ptrarray GPtrArray *object_dirs = NULL;
  gs_unref_object GFile *objdir = NULL;

  if (self->parent_repo)
    {
      if (!scan_loose_devino (self->parent_repo, devino_cache, cancellable, error))
        goto out;
    }

  repo_mode = ostree_repo_get_mode (self);

  if (!get_loose_object_dirs (self, &object_dirs, cancellable, error))
    goto out;

  for (i = 0; i < object_dirs->len; i++)
    {
      GFile *objdir = object_dirs->pdata[i];
      gs_unref_object GFileEnumerator *enumerator = NULL;
      gs_unref_object GFileInfo *file_info = NULL;
      const char *dirname;

      enumerator = g_file_enumerate_children (objdir, OSTREE_GIO_FAST_QUERYINFO, 
                                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                              cancellable, 
                                              error);
      if (!enumerator)
        goto out;

      dirname = gs_file_get_basename_cached (objdir);

      while (TRUE)
        {
          const char *name;
          const char *dot;
          guint32 type;
          OstreeDevIno *key;
          GString *checksum;
          gboolean skip;

          if (!gs_file_enumerator_iterate (enumerator, &file_info, NULL,
                                           NULL, error))
            goto out;
          if (file_info == NULL)
            break;

          name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
          type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

          if (type == G_FILE_TYPE_DIRECTORY)
            continue;
      
          switch (repo_mode)
            {
            case OSTREE_REPO_MODE_ARCHIVE:
              skip = !g_str_has_suffix (name, ".filecontent");
              break;
            case OSTREE_REPO_MODE_ARCHIVE_Z2:
            case OSTREE_REPO_MODE_BARE:
              skip = !g_str_has_suffix (name, ".file");
              break;
            default:
              g_assert_not_reached ();
            }
          if (skip)
            continue;

          dot = strrchr (name, '.');
          g_assert (dot);

          if ((dot - name) != 62)
            continue;
                  
          checksum = g_string_new (dirname);
          g_string_append_len (checksum, name, 62);
          
          key = g_new (OstreeDevIno, 1);
          key->dev = g_file_info_get_attribute_uint32 (file_info, "unix::device");
          key->ino = g_file_info_get_attribute_uint64 (file_info, "unix::inode");
          
          g_hash_table_replace (devino_cache, key, g_string_free (checksum, FALSE));
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static const char *
devino_cache_lookup (OstreeRepo           *self,
                     GFileInfo            *finfo)
{
  OstreeDevIno dev_ino;

  if (!self->loose_object_devino_hash)
    return NULL;

  dev_ino.dev = g_file_info_get_attribute_uint32 (finfo, "unix::device");
  dev_ino.ino = g_file_info_get_attribute_uint64 (finfo, "unix::inode");
  return g_hash_table_lookup (self->loose_object_devino_hash, &dev_ino);
}

gboolean
ostree_repo_prepare_transaction (OstreeRepo     *self,
                                 gboolean        enable_commit_hardlink_scan,
                                 gboolean       *out_transaction_resume,
                                 GCancellable   *cancellable,
                                 GError        **error)
{
  gboolean ret = FALSE;
  gboolean ret_transaction_resume = FALSE;
  gs_free char *transaction_str = NULL;

  g_return_val_if_fail (self->in_transaction == FALSE, FALSE);

  if (self->transaction_lock_path == NULL)
    self->transaction_lock_path = g_file_resolve_relative_path (self->repodir, "transaction");

  if (g_file_query_file_type (self->transaction_lock_path, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_SYMBOLIC_LINK)
    ret_transaction_resume = TRUE;
  else
    ret_transaction_resume = FALSE;

  self->in_transaction = TRUE;
  if (ret_transaction_resume)
    {
      if (!ot_gfile_ensure_unlinked (self->transaction_lock_path, cancellable, error))
        goto out;
    }
  transaction_str = g_strdup_printf ("pid=%llu", (unsigned long long) getpid ());
  if (!g_file_make_symbolic_link (self->transaction_lock_path, transaction_str,
                                  cancellable, error))
    goto out;

  if (enable_commit_hardlink_scan)
    {
      if (!self->loose_object_devino_hash)
        self->loose_object_devino_hash = g_hash_table_new_full (devino_hash, devino_equal, g_free, g_free);
      g_hash_table_remove_all (self->loose_object_devino_hash);
      if (!scan_loose_devino (self, self->loose_object_devino_hash, cancellable, error))
        goto out;
    }

  ret = TRUE;
  if (out_transaction_resume)
    *out_transaction_resume = ret_transaction_resume;
 out:
  return ret;
}

gboolean      
ostree_repo_commit_transaction (OstreeRepo     *self,
                                GCancellable   *cancellable,
                                GError        **error)
{
  gboolean ret = FALSE;

  g_return_val_if_fail (self->in_transaction == TRUE, FALSE);

  if (!ot_gfile_ensure_unlinked (self->transaction_lock_path, cancellable, error))
    goto out;

  if (self->loose_object_devino_hash)
    g_hash_table_remove_all (self->loose_object_devino_hash);

  self->in_transaction = FALSE;
  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_abort_transaction (OstreeRepo     *self,
                               GCancellable   *cancellable,
                               GError        **error)
{
  gboolean ret = FALSE;

  self->in_transaction = FALSE;
  if (self->loose_object_devino_hash)
    g_hash_table_remove_all (self->loose_object_devino_hash);

  ret = TRUE;
  return ret;
}

/**
 * ostree_repo_stage_metadata:
 * 
 * Store the metadata object @variant.  Return the checksum
 * as @out_csum.
 *
 * If @expected_checksum is not %NULL, verify it against the
 * computed checksum.
 */
gboolean
ostree_repo_stage_metadata (OstreeRepo         *self,
                            OstreeObjectType    type,
                            const char         *expected_checksum,
                            GVariant           *variant,
                            guchar            **out_csum,
                            GCancellable       *cancellable,
                            GError            **error)
{
  gs_unref_object GInputStream *input = NULL;
  gs_unref_variant GVariant *normalized = NULL;

  normalized = g_variant_get_normal_form (variant);
  input = ot_variant_read (normalized);
  
  return stage_object (self, type, expected_checksum, input, 0, out_csum,
                       cancellable, error);
}

/**
 * ostree_repo_stage_metadata_trusted:
 * 
 * Store the metadata object @variant; the provided @checksum
 * is trusted.
 */
gboolean
ostree_repo_stage_metadata_trusted (OstreeRepo         *self,
                                    OstreeObjectType    type,
                                    const char         *checksum,
                                    GVariant           *variant,
                                    GCancellable       *cancellable,
                                    GError            **error)
{
  gs_unref_object GInputStream *input = NULL;
  gs_unref_variant GVariant *normalized = NULL;

  normalized = g_variant_get_normal_form (variant);
  input = ot_variant_read (normalized);
  
  return stage_object (self, type, checksum, input, 0, NULL,
                       cancellable, error);
}

typedef struct {
  OstreeRepo *repo;
  OstreeObjectType objtype;
  char *expected_checksum;
  GVariant *object;
  GCancellable *cancellable;
  GSimpleAsyncResult *result;
  
  guchar *result_csum;
} StageMetadataAsyncData;

static void
stage_metadata_async_data_free (gpointer user_data)
{
  StageMetadataAsyncData *data = user_data;

  g_clear_object (&data->repo);
  g_clear_object (&data->cancellable);
  g_variant_unref (data->object);
  g_free (data->result_csum);
  g_free (data->expected_checksum);
  g_free (data);
}

static void
stage_metadata_thread (GSimpleAsyncResult  *res,
                       GObject             *object,
                       GCancellable        *cancellable)
{
  GError *error = NULL;
  StageMetadataAsyncData *data;

  data = g_simple_async_result_get_op_res_gpointer (res);
  if (!ostree_repo_stage_metadata (data->repo, data->objtype, data->expected_checksum,
                                   data->object,
                                   &data->result_csum,
                                   cancellable, &error))
    g_simple_async_result_take_error (res, error);
}

/**
 * ostree_repo_stage_metadata_async:
 * 
 * Asynchronously store the metadata object @variant.  If provided,
 * the checksum @expected_checksum will be verified.
 */
void          
ostree_repo_stage_metadata_async (OstreeRepo               *self,
                                  OstreeObjectType          objtype,
                                  const char               *expected_checksum,
                                  GVariant                 *object,
                                  GCancellable             *cancellable,
                                  GAsyncReadyCallback       callback,
                                  gpointer                  user_data)
{
  StageMetadataAsyncData *asyncdata;

  asyncdata = g_new0 (StageMetadataAsyncData, 1);
  asyncdata->repo = g_object_ref (self);
  asyncdata->objtype = objtype;
  asyncdata->expected_checksum = g_strdup (expected_checksum);
  asyncdata->object = g_variant_ref (object);
  asyncdata->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  asyncdata->result = g_simple_async_result_new ((GObject*) self,
                                                 callback, user_data,
                                                 ostree_repo_stage_metadata_async);

  g_simple_async_result_set_op_res_gpointer (asyncdata->result, asyncdata,
                                             stage_metadata_async_data_free);
  g_simple_async_result_run_in_thread (asyncdata->result, stage_metadata_thread, G_PRIORITY_DEFAULT, cancellable);
  g_object_unref (asyncdata->result);
}

gboolean
ostree_repo_stage_metadata_finish (OstreeRepo        *self,
                                   GAsyncResult      *result,
                                   guchar           **out_csum,
                                   GError           **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  StageMetadataAsyncData *data;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == ostree_repo_stage_metadata_async);

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  data = g_simple_async_result_get_op_res_gpointer (simple);
  /* Transfer ownership */
  *out_csum = data->result_csum;
  data->result_csum = NULL;
  return TRUE;
}

gboolean
_ostree_repo_stage_directory_meta (OstreeRepo   *self,
                                   GFileInfo    *file_info,
                                   GVariant     *xattrs,
                                   guchar      **out_csum,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  gs_unref_variant GVariant *dirmeta = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  dirmeta = ostree_create_directory_metadata (file_info, xattrs);
  
  return ostree_repo_stage_metadata (self, OSTREE_OBJECT_TYPE_DIR_META, NULL,
                                     dirmeta, out_csum, cancellable, error);
}

GFile *
ostree_repo_get_object_path (OstreeRepo       *self,
                             const char       *checksum,
                             OstreeObjectType  type)
{
  char *relpath;
  GFile *ret;
  gboolean compressed;

  compressed = (type == OSTREE_OBJECT_TYPE_FILE
                && ostree_repo_get_mode (self) == OSTREE_REPO_MODE_ARCHIVE_Z2);
  relpath = ostree_get_relative_object_path (checksum, type, compressed);
  ret = g_file_resolve_relative_path (self->repodir, relpath);
  g_free (relpath);
 
  return ret;
}

GFile *
_ostree_repo_get_uncompressed_object_cache_path (OstreeRepo       *self,
                                                 const char       *checksum)
{
  char *relpath;
  GFile *ret;

  relpath = ostree_get_relative_object_path (checksum, OSTREE_OBJECT_TYPE_FILE, FALSE);
  ret = g_file_resolve_relative_path (self->uncompressed_objects_dir, relpath);
  g_free (relpath);
 
  return ret;
}

/**
 * ostree_repo_stage_content_trusted:
 *
 * Store the content object streamed as @object_input, with total
 * length @length.  The given @checksum will be treated as trusted.
 *
 * This function should be used when importing file objects from local
 * disk, for example.
 */
gboolean      
ostree_repo_stage_content_trusted (OstreeRepo       *self,
                                   const char       *checksum,
                                   GInputStream     *object_input,
                                   guint64           length,
                                   GCancellable     *cancellable,
                                   GError          **error)
{
  return stage_object (self, OSTREE_OBJECT_TYPE_FILE, checksum,
                       object_input, length, NULL,
                       cancellable, error);
}

/**
 * ostree_repo_stage_content:
 *
 * Store the content object streamed as @object_input,
 * with total length @length.  The actual checksum will
 * be returned as @out_csum.
 */
gboolean
ostree_repo_stage_content (OstreeRepo       *self,
                           const char       *expected_checksum,
                           GInputStream     *object_input,
                           guint64           length,
                           guchar          **out_csum,
                           GCancellable     *cancellable,
                           GError          **error)
{
  return stage_object (self, OSTREE_OBJECT_TYPE_FILE, expected_checksum,
                       object_input, length, out_csum,
                       cancellable, error);
}

typedef struct {
  OstreeRepo *repo;
  char *expected_checksum;
  GInputStream *object;
  guint64 file_object_length;
  GCancellable *cancellable;
  GSimpleAsyncResult *result;
  
  guchar *result_csum;
} StageContentAsyncData;

static void
stage_content_async_data_free (gpointer user_data)
{
  StageContentAsyncData *data = user_data;

  g_clear_object (&data->repo);
  g_clear_object (&data->cancellable);
  g_clear_object (&data->object);
  g_free (data->result_csum);
  g_free (data->expected_checksum);
  g_free (data);
}

static void
stage_content_thread (GSimpleAsyncResult  *res,
                      GObject             *object,
                      GCancellable        *cancellable)
{
  GError *error = NULL;
  StageContentAsyncData *data;

  data = g_simple_async_result_get_op_res_gpointer (res);
  if (!ostree_repo_stage_content (data->repo, data->expected_checksum,
                                  data->object, data->file_object_length,
                                  &data->result_csum,
                                  cancellable, &error))
    g_simple_async_result_take_error (res, error);
}

/**
 * ostree_repo_stage_content_async:
 * 
 * Asynchronously store the content object @object.  If provided,
 * the checksum @expected_checksum will be verified.
 */
void          
ostree_repo_stage_content_async (OstreeRepo               *self,
                                 const char               *expected_checksum,
                                 GInputStream             *object,
                                 guint64                   file_object_length,
                                 GCancellable             *cancellable,
                                 GAsyncReadyCallback       callback,
                                 gpointer                  user_data)
{
  StageContentAsyncData *asyncdata;

  asyncdata = g_new0 (StageContentAsyncData, 1);
  asyncdata->repo = g_object_ref (self);
  asyncdata->expected_checksum = g_strdup (expected_checksum);
  asyncdata->object = g_object_ref (object);
  asyncdata->file_object_length = file_object_length;
  asyncdata->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  asyncdata->result = g_simple_async_result_new ((GObject*) self,
                                                 callback, user_data,
                                                 ostree_repo_stage_content_async);

  g_simple_async_result_set_op_res_gpointer (asyncdata->result, asyncdata,
                                             stage_content_async_data_free);
  g_simple_async_result_run_in_thread (asyncdata->result, stage_content_thread, G_PRIORITY_DEFAULT, cancellable);
  g_object_unref (asyncdata->result);
}

gboolean
ostree_repo_stage_content_finish (OstreeRepo        *self,
                                  GAsyncResult      *result,
                                  guchar           **out_csum,
                                  GError           **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  StageContentAsyncData *data;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == ostree_repo_stage_content_async);

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  data = g_simple_async_result_get_op_res_gpointer (simple);
  /* Transfer ownership */
  *out_csum = data->result_csum;
  data->result_csum = NULL;
  return TRUE;
}

static GVariant *
create_empty_gvariant_dict (void)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE("a{sv}"));
  return g_variant_builder_end (&builder);
}

gboolean
ostree_repo_stage_commit (OstreeRepo *self,
                          const char   *branch,
                          const char   *parent,
                          const char   *subject,
                          const char   *body,
                          const char   *root_contents_checksum,
                          const char   *root_metadata_checksum,
                          char        **out_commit,
                          GCancellable *cancellable,
                          GError      **error)
{
  gboolean ret = FALSE;
  gs_free char *ret_commit = NULL;
  gs_unref_variant GVariant *commit = NULL;
  gs_free guchar *commit_csum = NULL;
  GDateTime *now = NULL;

  g_return_val_if_fail (branch != NULL, FALSE);
  g_return_val_if_fail (subject != NULL, FALSE);
  g_return_val_if_fail (root_contents_checksum != NULL, FALSE);
  g_return_val_if_fail (root_metadata_checksum != NULL, FALSE);

  now = g_date_time_new_now_utc ();
  commit = g_variant_new ("(@a{sv}@ay@a(say)sst@ay@ay)",
                          create_empty_gvariant_dict (),
                          parent ? ostree_checksum_to_bytes_v (parent) : ot_gvariant_new_bytearray (NULL, 0),
                          g_variant_new_array (G_VARIANT_TYPE ("(say)"), NULL, 0),
                          subject, body ? body : "",
                          GUINT64_TO_BE (g_date_time_to_unix (now)),
                          ostree_checksum_to_bytes_v (root_contents_checksum),
                          ostree_checksum_to_bytes_v (root_metadata_checksum));
  g_variant_ref_sink (commit);
  if (!ostree_repo_stage_metadata (self, OSTREE_OBJECT_TYPE_COMMIT, NULL,
                                   commit, &commit_csum,
                                   cancellable, error))
    goto out;

  ret_commit = ostree_checksum_from_bytes (commit_csum);

  ret = TRUE;
  ot_transfer_out_value(out_commit, &ret_commit);
 out:
  if (now)
    g_date_time_unref (now);
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
  gboolean repo_dir_was_empty = FALSE;
  OstreeRepoCommitFilterResult filter_result;
  gs_unref_object OstreeRepoFile *repo_dir = NULL;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFileInfo *child_info = NULL;

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
      gs_unref_object GFileInfo *modified_info = NULL;
      gs_unref_variant GVariant *xattrs = NULL;
      gs_free guchar *child_file_csum = NULL;
      gs_free char *tmp_checksum = NULL;

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
          
          if (!_ostree_repo_stage_directory_meta (self, modified_info, xattrs, &child_file_csum,
                                                  cancellable, error))
            goto out;
          
          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
          ostree_mutable_tree_set_metadata_checksum (mtree, tmp_checksum);
        }

      g_clear_object (&child_info);
    }

  if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
    {
      dir_enum = g_file_enumerate_children ((GFile*)dir, OSTREE_GIO_FAST_QUERYINFO, 
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            cancellable, 
                                            error);
      if (!dir_enum)
        goto out;

      while (TRUE)
        {
          GFileInfo *child_info;
          gs_unref_object GFile *child = NULL;
          gs_unref_object GFileInfo *modified_info = NULL;
          gs_unref_object OstreeMutableTree *child_mtree = NULL;
          const char *name;
          
          if (!gs_file_enumerator_iterate (dir_enum, &child_info, NULL,
                                           cancellable, error))
            goto out;
          if (child_info == NULL)
            break;

          name = g_file_info_get_name (child_info);
          g_ptr_array_add (path, (char*)name);
          filter_result = apply_commit_filter (self, modifier, path, child_info, &modified_info);

          if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
            {
              child = g_file_get_child (dir, name);

              if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
                {
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
                  guint64 file_obj_length;
                  const char *loose_checksum;
                  gs_unref_object GInputStream *file_input = NULL;
                  gs_unref_variant GVariant *xattrs = NULL;
                  gs_unref_object GInputStream *file_object_input = NULL;
                  gs_free guchar *child_file_csum = NULL;
                  gs_free char *tmp_checksum = NULL;

                  loose_checksum = devino_cache_lookup (self, child_info);

                  if (loose_checksum)
                    {
                      if (!ostree_mutable_tree_replace_file (mtree, name, loose_checksum,
                                                             error))
                        goto out;
                    }
                  else
                    {
                     if (g_file_info_get_file_type (modified_info) == G_FILE_TYPE_REGULAR)
                        {
                          file_input = (GInputStream*)g_file_read (child, cancellable, error);
                          if (!file_input)
                            goto out;
                        }

                      if (!(modifier && modifier->skip_xattrs))
                        {
                          g_clear_pointer (&xattrs, (GDestroyNotify) g_variant_unref);
                          if (!ostree_get_xattrs_for_file (child, &xattrs, cancellable, error))
                            goto out;
                        }

                      if (!ostree_raw_file_to_content_stream (file_input,
                                                              modified_info, xattrs,
                                                              &file_object_input, &file_obj_length,
                                                              cancellable, error))
                        goto out;
                      if (!ostree_repo_stage_content (self, NULL, file_object_input, file_obj_length,
                                                      &child_file_csum, cancellable, error))
                        goto out;

                      g_free (tmp_checksum);
                      tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
                      if (!ostree_mutable_tree_replace_file (mtree, name, tmp_checksum,
                                                             error))
                        goto out;
                    }
                }

              g_ptr_array_remove_index (path, path->len - 1);
            }
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
  gs_free char *ret_contents_checksum = NULL;
  gs_unref_hashtable GHashTable *dir_metadata_checksums = NULL;
  gs_unref_hashtable GHashTable *dir_contents_checksums = NULL;
  gs_unref_variant GVariant *serialized_tree = NULL;
  gs_free guchar *contents_csum = NULL;

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
      
      if (!ostree_repo_stage_metadata (self, OSTREE_OBJECT_TYPE_DIR_TREE, NULL,
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
  const char *dirname = NULL;
  const char *dot = NULL;
  gs_unref_object GFileEnumerator *enumerator = NULL;
  GString *checksum = NULL;

  dirname = gs_file_get_basename_cached (dir);

  /* We're only querying name */
  enumerator = g_file_enumerate_children (dir, "standard::name,standard::type", 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, 
                                          error);
  if (!enumerator)
    goto out;
  
  while (TRUE)
    {
      GFileInfo *file_info;
      const char *name;
      guint32 type;
      OstreeObjectType objtype;

      if (!gs_file_enumerator_iterate (enumerator, &file_info, NULL,
                                       NULL, error))
        goto out;
      if (file_info == NULL)
        break;

      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

      if (type == G_FILE_TYPE_DIRECTORY)
        continue;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      
      if (g_str_has_suffix (name, ".file"))
        objtype = OSTREE_OBJECT_TYPE_FILE;
      else if (g_str_has_suffix (name, ".dirtree"))
        objtype = OSTREE_OBJECT_TYPE_DIR_TREE;
      else if (g_str_has_suffix (name, ".dirmeta"))
        objtype = OSTREE_OBJECT_TYPE_DIR_META;
      else if (g_str_has_suffix (name, ".commit"))
        objtype = OSTREE_OBJECT_TYPE_COMMIT;
      else
        continue;
          
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
          g_hash_table_replace (inout_objects, key,
                                g_variant_ref_sink (value));
        }
    }

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
  guint i;
  gs_unref_ptrarray GPtrArray *object_dirs = NULL;
  gs_unref_object GFile *objdir = NULL;

  if (!get_loose_object_dirs (self, &object_dirs, cancellable, error))
    goto out;

  for (i = 0; i < object_dirs->len; i++)
    {
      GFile *objdir = object_dirs->pdata[i];
      if (!list_loose_object_dir (self, objdir, inout_objects, cancellable, error))
        goto out;
    }

  ret = TRUE;
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
  OstreeRepoMode repo_mode;
  gs_unref_variant GVariant *file_data = NULL;
  gs_unref_object GFile *loose_path = NULL;
  gs_unref_object GFileInfo *content_loose_info = NULL;
  gs_unref_object GInputStream *ret_input = NULL;
  gs_unref_object GFileInfo *ret_file_info = NULL;
  gs_unref_variant GVariant *ret_xattrs = NULL;

  if (!repo_find_object (self, OSTREE_OBJECT_TYPE_FILE, checksum, &loose_path,
                         cancellable, error))
    goto out;

  repo_mode = ostree_repo_get_mode (self);

  if (loose_path)
    {
      switch (repo_mode)
        {
        case OSTREE_REPO_MODE_ARCHIVE:
          {
            gs_unref_variant GVariant *archive_meta = NULL;

            if (!ot_util_variant_map (loose_path, OSTREE_FILE_HEADER_GVARIANT_FORMAT,
                                      TRUE, &archive_meta, error))
              goto out;

            if (!ostree_file_header_parse (archive_meta, &ret_file_info, &ret_xattrs,
                                           error))
              goto out;

            if (g_file_info_get_file_type (ret_file_info) == G_FILE_TYPE_REGULAR)
              {
                gs_unref_object GFile *archive_content_path = NULL;
                gs_unref_object GFileInfo *content_info = NULL;

                archive_content_path = ostree_repo_get_archive_content_path (self, checksum);
                content_info = g_file_query_info (archive_content_path, OSTREE_GIO_FAST_QUERYINFO,
                                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                  cancellable, error);
                if (!content_info)
                  goto out;

                if (out_input)
                  {
                    ret_input = (GInputStream*)gs_file_read_noatime (archive_content_path, cancellable, error);
                    if (!ret_input)
                      goto out;
                  }
                g_file_info_set_size (ret_file_info, g_file_info_get_size (content_info));
              }
          }
          break;
        case OSTREE_REPO_MODE_ARCHIVE_Z2:
          {
            if (!ostree_content_file_parse (TRUE, loose_path, TRUE,
                                            out_input ? &ret_input : NULL,
                                            &ret_file_info, &ret_xattrs,
                                            cancellable, error))
              goto out;
          }
          break;
        case OSTREE_REPO_MODE_BARE:
          {
            ret_file_info = g_file_query_info (loose_path, OSTREE_GIO_FAST_QUERYINFO,
                                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                               cancellable, error);
            if (!ret_file_info)
              goto out;

            if (out_xattrs)
              {
                if (!ostree_get_xattrs_for_file (loose_path, &ret_xattrs,
                                                 cancellable, error))
                  goto out;
              }

            if (out_input && g_file_info_get_file_type (ret_file_info) == G_FILE_TYPE_REGULAR)
              {
                ret_input = (GInputStream*) gs_file_read_noatime (loose_path, cancellable, error);
                if (!ret_input)
                  {
                    g_prefix_error (error, "Error opening loose file object %s: ", gs_file_get_path_cached (loose_path));
                    goto out;
                  }
              }
          }
          break;
        }
    }
  else if (self->parent_repo)
    {
      if (!ostree_repo_load_file (self->parent_repo, checksum, 
                                  out_input ? &ret_input : NULL,
                                  out_file_info ? &ret_file_info : NULL,
                                  out_xattrs ? &ret_xattrs : NULL,
                                  cancellable, error))
        goto out;
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Couldn't find file object '%s'", checksum);
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_input, &ret_input);
  ot_transfer_out_value (out_file_info, &ret_file_info);
  ot_transfer_out_value (out_xattrs, &ret_xattrs);
 out:
  return ret;
}

static gboolean      
repo_find_object (OstreeRepo           *self,
                  OstreeObjectType      objtype,
                  const char           *checksum,
                  GFile               **out_stored_path,
                  GCancellable         *cancellable,
                  GError             **error)
{
  gboolean ret = FALSE;
  struct stat stbuf;
  gs_unref_object GFile *object_path = NULL;
  gs_unref_object GFile *ret_stored_path = NULL;

  object_path = ostree_repo_get_object_path (self, checksum, objtype);
  
  if (lstat (gs_file_get_path_cached (object_path), &stbuf) == 0)
    {
      ret_stored_path = object_path;
      object_path = NULL; /* Transfer ownership */
    }
  else if (errno != ENOENT)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }
      
  ret = TRUE;
  ot_transfer_out_value (out_stored_path, &ret_stored_path);
out:
  return ret;
}

gboolean
ostree_repo_has_object (OstreeRepo           *self,
                        OstreeObjectType      objtype,
                        const char           *checksum,
                        gboolean             *out_have_object,
                        GCancellable         *cancellable,
                        GError              **error)
{
  gboolean ret = FALSE;
  gboolean ret_have_object;
  gs_unref_object GFile *loose_path = NULL;

  if (!repo_find_object (self, objtype, checksum, &loose_path,
                         cancellable, error))
    goto out;

  ret_have_object = (loose_path != NULL);

  if (!ret_have_object && self->parent_repo)
    {
      if (!ostree_repo_has_object (self->parent_repo, objtype, checksum,
                                   &ret_have_object, cancellable, error))
        goto out;
    }
                                
  ret = TRUE;
  if (out_have_object)
    *out_have_object = ret_have_object;
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
  gs_free char *checksum = NULL;

  checksum = ostree_checksum_from_bytes (csum);

  if (!ostree_repo_load_variant (self, objtype, checksum, out_variant, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
load_variant_internal (OstreeRepo       *self,
                       OstreeObjectType  objtype,
                       const char       *sha256, 
                       gboolean          error_if_not_found,
                       GVariant        **out_variant,
                       GError          **error)
{
  gboolean ret = FALSE;
  GCancellable *cancellable = NULL;
  gs_unref_object GFile *object_path = NULL;
  gs_unref_variant GVariant *ret_variant = NULL;

  g_return_val_if_fail (OSTREE_OBJECT_TYPE_IS_META (objtype), FALSE);

  if (!repo_find_object (self, objtype, sha256, &object_path,
                         cancellable, error))
    goto out;

  if (object_path != NULL)
    {
      if (!ot_util_variant_map (object_path, ostree_metadata_variant_type (objtype),
                                TRUE, &ret_variant, error))
        goto out;
    }
  else if (self->parent_repo)
    {
      if (!ostree_repo_load_variant (self->parent_repo, objtype, sha256, &ret_variant, error))
        goto out;
    }
  else if (error_if_not_found)
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
 * ostree_repo_load_variant_if_exists:
 * 
 * Attempt to load the metadata object @sha256 of type @objtype if it
 * exists, storing the result in @out_variant.  If it doesn't exist,
 * %NULL is returned.
 */
gboolean
ostree_repo_load_variant_if_exists (OstreeRepo       *self,
                                    OstreeObjectType  objtype,
                                    const char       *sha256, 
                                    GVariant        **out_variant,
                                    GError          **error)
{
  return load_variant_internal (self, objtype, sha256, FALSE,
                                out_variant, error);
}

/**
 * ostree_repo_load_variant:
 * 
 * Load the metadata object @sha256 of type @objtype, storing the
 * result in @out_variant.
 */
gboolean
ostree_repo_load_variant (OstreeRepo       *self,
                          OstreeObjectType  objtype,
                          const char       *sha256, 
                          GVariant        **out_variant,
                          GError          **error)
{
  return load_variant_internal (self, objtype, sha256, TRUE,
                                out_variant, error);
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
  gs_unref_hashtable GHashTable *ret_objects = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (self->inited, FALSE);
  
  ret_objects = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                       (GDestroyNotify) g_variant_unref,
                                       (GDestroyNotify) g_variant_unref);

  if (flags & OSTREE_REPO_LIST_OBJECTS_ALL)
    flags |= (OSTREE_REPO_LIST_OBJECTS_LOOSE | OSTREE_REPO_LIST_OBJECTS_PACKED);

  if (flags & OSTREE_REPO_LIST_OBJECTS_LOOSE)
    {
      if (!list_loose_objects (self, ret_objects, cancellable, error))
        goto out;
      if (self->parent_repo)
        {
          if (!list_loose_objects (self->parent_repo, ret_objects, cancellable, error))
            goto out;
        }
    }

  if (flags & OSTREE_REPO_LIST_OBJECTS_PACKED)
    {
      /* Nothing for now... */
    }

  ret = TRUE;
  ot_transfer_out_value (out_objects, &ret_objects);
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
  gs_unref_object GFile *ret_root = NULL;
  gs_free char *resolved_rev = NULL;

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

#ifndef HAVE_LIBSOUP
gboolean
ostree_repo_pull (OstreeRepo               *repo,
                  const char               *remote_name,
                  char                    **refs_to_fetch,
                  OstreeRepoPullFlags       flags,
                  GCancellable             *cancellable,
                  GError                  **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "This version of ostree was built without libsoup, and cannot fetch over HTTP");
  return FALSE;
}
#endif
