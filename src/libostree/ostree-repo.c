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

#include <glib-unix.h>
#include <gio/gunixinputstream.h>
#include "otutil.h"
#include "libgsystem.h"

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-file.h"

/**
 * SECTION:libostree-repo
 * @title: Content-addressed object store
 * @short_description: A git-like storage system for operating system binaries
 *
 * The #OstreeRepo is like git, a content-addressed object store.
 * Unlike git, it records uid, gid, and extended attributes.
 *
 * There are two possible "modes" for an #OstreeRepo;
 * %OSTREE_REPO_MODE_BARE is very simple - content files are
 * represented exactly as they are, and checkouts are just hardlinks.
 * A %OSTREE_REPO_MODE_ARCHIVE_Z2 repository in contrast stores
 * content files zlib-compressed.  It is suitable for non-root-owned
 * repositories that can be served via a static HTTP server.
 *
 * Creating an #OstreeRepo does not invoke any file I/O, and thus needs
 * to be initialized, either from an existing contents or with a new
 * repository. If you have an existing repo, use ostree_repo_open()
 * to load it from disk and check its validity. To initialize a new
 * repository in the given filepath, use ostree_repo_create() instead.
 *
 * To store content in the repo, first start a transaction with
 * ostree_repo_prepare_transaction().  Then create a
 * #OstreeMutableTree, and apply functions such as
 * ostree_repo_write_directory_to_mtree() to traverse a physical
 * filesystem and write content, possibly multiple times.
 *
 * Once the #OstreeMutableTree is complete, write all of its metadata
 * with ostree_repo_write_mtree(), and finally create a commit with
 * ostree_repo_write_commit().
 */
typedef struct {
  GObjectClass parent_class;
} OstreeRepoClass;

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
  if (self->tmp_dir_fd)
    (void) close (self->tmp_dir_fd);
  g_clear_object (&self->local_heads_dir);
  g_clear_object (&self->remote_heads_dir);
  g_clear_object (&self->objects_dir);
  if (self->objects_dir_fd != -1)
    (void) close (self->objects_dir_fd);
  g_clear_object (&self->uncompressed_objects_dir);
  if (self->uncompressed_objects_dir_fd != -1)
    (void) close (self->uncompressed_objects_dir_fd);
  g_clear_object (&self->remote_cache_dir);
  g_clear_object (&self->config_file);

  g_clear_object (&self->transaction_lock_path);

  if (self->loose_object_devino_hash)
    g_hash_table_destroy (self->loose_object_devino_hash);
  if (self->updated_uncompressed_dirs)
    g_hash_table_destroy (self->updated_uncompressed_dirs);
  if (self->config)
    g_key_file_free (self->config);
  g_clear_pointer (&self->txn_refs, g_hash_table_destroy);
  g_clear_pointer (&self->cached_meta_indexes, (GDestroyNotify) g_ptr_array_unref);
  g_clear_pointer (&self->cached_content_indexes, (GDestroyNotify) g_ptr_array_unref);
  g_mutex_clear (&self->cache_lock);
  g_mutex_clear (&self->txn_stats_lock);

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

static void
ostree_repo_constructed (GObject *object)
{
  OstreeRepo *self = OSTREE_REPO (object);

  g_assert (self->repodir != NULL);

  self->tmp_dir = g_file_resolve_relative_path (self->repodir, "tmp");
  self->local_heads_dir = g_file_resolve_relative_path (self->repodir, "refs/heads");
  self->remote_heads_dir = g_file_resolve_relative_path (self->repodir, "refs/remotes");

  self->objects_dir = g_file_get_child (self->repodir, "objects");
  self->uncompressed_objects_dir = g_file_resolve_relative_path (self->repodir, "uncompressed-objects-cache/objects");
  self->remote_cache_dir = g_file_get_child (self->repodir, "remote-cache");
  self->config_file = g_file_get_child (self->repodir, "config");

  G_OBJECT_CLASS (ostree_repo_parent_class)->constructed (object);
}

static void
ostree_repo_class_init (OstreeRepoClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = ostree_repo_constructed;
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
  g_mutex_init (&self->txn_stats_lock);
  self->objects_dir_fd = -1;
  self->uncompressed_objects_dir_fd = -1;
}

/**
 * ostree_repo_new:
 * @path: Path to a repository
 *
 * Returns: (transfer full): An accessor object for an OSTree repository located at @path
 */
OstreeRepo*
ostree_repo_new (GFile *path)
{
  return g_object_new (OSTREE_TYPE_REPO, "path", path, NULL);
}

/**
 * ostree_repo_new_default:
 *
 * If the current working directory appears to be an OSTree
 * repository, create a new #OstreeRepo object for accessing it.
 * Otherwise, use the default system repository located at
 * /ostree/repo.
 *
 * Returns: (transfer full): An accessor object for an OSTree repository located at /ostree/repo
 */
OstreeRepo*
ostree_repo_new_default (void)
{
  if (g_file_test ("objects", G_FILE_TEST_IS_DIR)
      && g_file_test ("config", G_FILE_TEST_IS_REGULAR))
    {
      gs_unref_object GFile *cwd = g_file_new_for_path (".");
      return ostree_repo_new (cwd);
    }
  else
    {
      gs_unref_object GFile *default_repo_path = g_file_new_for_path ("/ostree/repo");
      return ostree_repo_new (default_repo_path);
    }
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
 * @self: Repo
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

static gboolean
ostree_repo_mode_to_string (OstreeRepoMode   mode,
                            const char     **out_mode,
                            GError         **error)
{
  gboolean ret = FALSE;
  const char *ret_mode;

  switch (mode)
    {
    case OSTREE_REPO_MODE_BARE:
      ret_mode = "bare";
      break;
    case OSTREE_REPO_MODE_ARCHIVE_Z2:
      ret_mode ="archive-z2";
      break;
    default:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid mode '%d'", mode);
      goto out;
    }

  ret = TRUE;
  *out_mode = ret_mode;
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

#define DEFAULT_CONFIG_CONTENTS ("[core]\n" \
                                 "repo_version=1\n")

/**
 * ostree_repo_create:
 * @self: An #OstreeRepo
 * @mode: The mode to store the repository in
 * @cancellable: Cancellable
 * @error: Error
 *
 * Create the underlying structure on disk for the
 * repository.
 */
gboolean
ostree_repo_create (OstreeRepo     *self,
                    OstreeRepoMode  mode,
                    GCancellable   *cancellable,
                    GError        **error)
{
  gboolean ret = FALSE;
  GString *config_data = NULL;
  gs_unref_object GFile *child = NULL;
  gs_unref_object GFile *grandchild = NULL;
  const char *mode_str;

  if (!ostree_repo_mode_to_string (mode, &mode_str, error))
    goto out;

  if (!gs_file_ensure_directory (self->repodir, FALSE, cancellable, error))
    goto out;

  config_data = g_string_new (DEFAULT_CONFIG_CONTENTS);
  g_string_append_printf (config_data, "mode=%s\n", mode_str);
  if (!g_file_replace_contents (self->config_file,
                                config_data->str,
                                config_data->len,
                                NULL, FALSE, 0, NULL,
                                cancellable, error))
    goto out;

  if (!g_file_make_directory (self->objects_dir, cancellable, error))
    goto out;

  if (!g_file_make_directory (self->tmp_dir, cancellable, error))
    goto out;

  if (!g_file_make_directory (self->remote_cache_dir, cancellable, error))
    goto out;

  g_clear_object (&child);
  child = g_file_get_child (self->repodir, "refs");
  if (!g_file_make_directory (child, cancellable, error))
    goto out;

  g_clear_object (&grandchild);
  grandchild = g_file_get_child (child, "heads");
  if (!g_file_make_directory (grandchild, cancellable, error))
    goto out;

  g_clear_object (&grandchild);
  grandchild = g_file_get_child (child, "remotes");
  if (!g_file_make_directory (grandchild, cancellable, error))
    goto out;

  if (!ostree_repo_open (self, cancellable, error))
    goto out;

  ret = TRUE;

 out:
  if (config_data)
    g_string_free (config_data, TRUE);
  return ret;
}

gboolean
ostree_repo_open (OstreeRepo    *self,
                  GCancellable  *cancellable,
                  GError       **error)
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
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Couldn't find objects directory '%s'",
                   gs_file_get_path_cached (self->objects_dir));
      goto out;
    }

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
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "This version of OSTree no longer supports \"archive\" repositories; use archive-z2 instead");
      goto out;
    }

  if (!ot_keyfile_get_value_with_default (self->config, "core", "mode",
                                          "bare", &mode, error))
    goto out;
  if (!ostree_repo_mode_from_string (mode, &self->mode, error))
    goto out;

  if (!ot_keyfile_get_value_with_default (self->config, "core", "parent",
                                          NULL, &parent_repo_path, error))
    goto out;

  if (parent_repo_path && parent_repo_path[0])
    {
      gs_unref_object GFile *parent_repo_f = g_file_new_for_path (parent_repo_path);

      self->parent_repo = ostree_repo_new (parent_repo_f);

      if (!ostree_repo_open (self->parent_repo, cancellable, error))
        {
          g_prefix_error (error, "While checking parent repository '%s': ",
                          gs_file_get_path_cached (parent_repo_f));
          goto out;
        }
    }

  if (!ot_keyfile_get_boolean_with_default (self->config, "core", "enable-uncompressed-cache",
                                            TRUE, &self->enable_uncompressed_cache, error))
    goto out;

  if (!gs_file_open_dir_fd (self->objects_dir, &self->objects_dir_fd, cancellable, error))
    goto out;

  if (!gs_file_open_dir_fd (self->tmp_dir, &self->tmp_dir_fd, cancellable, error))
    goto out;

  if (self->mode == OSTREE_REPO_MODE_ARCHIVE_Z2)
    {
      if (!gs_file_ensure_directory (self->uncompressed_objects_dir, TRUE, cancellable, error))
        goto out;
      if (!gs_file_open_dir_fd (self->uncompressed_objects_dir,
                                &self->uncompressed_objects_dir_fd,
                                cancellable, error))
        goto out;
    }

  self->inited = TRUE;

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_get_path:
 * @self:
 *
 * Returns: (transfer none): Path to repo
 */
GFile *
ostree_repo_get_path (OstreeRepo  *self)
{
  return self->repodir;
}

OstreeRepoMode
ostree_repo_get_mode (OstreeRepo  *self)
{
  g_return_val_if_fail (self->inited, FALSE);

  return self->mode;
}

/**
 * ostree_repo_get_parent:
 * @self: Repo
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

gboolean
_ostree_repo_get_loose_object_dirs (OstreeRepo       *self,
                                    GPtrArray       **out_object_dirs,
                                    GCancellable     *cancellable,
                                    GError          **error)
{
  gboolean ret = FALSE;
  gs_unref_ptrarray GPtrArray *ret_object_dirs = NULL;

  ret_object_dirs = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  if (ostree_repo_get_mode (self) == OSTREE_REPO_MODE_ARCHIVE_Z2)
    {
      if (!append_object_dirs_from (self, self->uncompressed_objects_dir, ret_object_dirs,
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

  if (!_ostree_repo_get_loose_object_dirs (self, &object_dirs, cancellable, error))
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

static gboolean
openat_allow_noent (int                 dfd,
                    const char         *path,
                    int                *fd,
                    GCancellable       *cancellable,
                    GError            **error)
{
  GError *temp_error = NULL;

  if (!gs_file_openat_noatime (dfd, path, fd,
                               cancellable, &temp_error))
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          *fd = -1;
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          return FALSE;
        }
    }
  return TRUE;
}

static gboolean
load_metadata_internal (OstreeRepo       *self,
                        OstreeObjectType  objtype,
                        const char       *sha256,
                        gboolean          error_if_not_found,
                        GVariant        **out_variant,
                        GInputStream    **out_stream,
                        guint64          *out_size,
                        GCancellable     *cancellable,
                        GError          **error)
{
  gboolean ret = FALSE;
  char loose_path_buf[_OSTREE_LOOSE_PATH_MAX];
  int fd = -1;
  gs_unref_object GInputStream *ret_stream = NULL;
  gs_unref_variant GVariant *ret_variant = NULL;

  g_return_val_if_fail (OSTREE_OBJECT_TYPE_IS_META (objtype), FALSE);

  _ostree_loose_path (loose_path_buf, sha256, objtype, self->mode);

  if (!openat_allow_noent (self->objects_dir_fd, loose_path_buf, &fd,
                           cancellable, error))
    goto out;

  if (fd != -1)
    {
      if (out_variant)
        {
          GMappedFile *mfile;

          mfile = g_mapped_file_new_from_fd (fd, FALSE, error);
          if (!mfile)
            goto out;
          (void) close (fd); /* Ignore errors, we have it mapped */
          fd = -1;
          ret_variant = g_variant_new_from_data (ostree_metadata_variant_type (objtype),
                                                 g_mapped_file_get_contents (mfile),
                                                 g_mapped_file_get_length (mfile),
                                                 TRUE,
                                                 (GDestroyNotify) g_mapped_file_unref,
                                                 mfile);
          g_variant_ref_sink (ret_variant);

          if (out_size)
            *out_size = g_variant_get_size (ret_variant);
        }
      else if (out_stream)
        {
          ret_stream = g_unix_input_stream_new (fd, TRUE);
          if (!ret_stream)
            goto out;
          fd = -1; /* Transfer ownership */
          if (out_size)
            {
              struct stat stbuf;

              if (!gs_stream_fstat ((GFileDescriptorBased*)ret_stream, &stbuf, cancellable, error))
                goto out;
              *out_size = stbuf.st_size;
            }
        }
    }
  else if (self->parent_repo)
    {
      if (!ostree_repo_load_variant (self->parent_repo, objtype, sha256, &ret_variant, error))
        goto out;
    }
  else if (error_if_not_found)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No such metadata object %s.%s",
                   sha256, ostree_object_type_to_string (objtype));
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_variant, &ret_variant);
  ot_transfer_out_value (out_stream, &ret_stream);
 out:
  if (fd != -1)
    (void) close (fd);
  return ret;
}

static gboolean
query_info_for_bare_content_object (OstreeRepo      *self,
                                    const char      *loose_path_buf,
                                    GFileInfo      **out_info,
                                    GCancellable    *cancellable,
                                    GError         **error)
{
  gboolean ret = FALSE;
  struct stat stbuf;
  int res;
  gs_unref_object GFileInfo *ret_info = NULL;

  do
    res = fstatat (self->objects_dir_fd, loose_path_buf, &stbuf, AT_SYMLINK_NOFOLLOW);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      if (errno == ENOENT)
        {
          *out_info = NULL;
          ret = TRUE;
          goto out;
        }
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  ret_info = g_file_info_new ();

  if (S_ISREG (stbuf.st_mode))
    {
      g_file_info_set_file_type (ret_info, G_FILE_TYPE_REGULAR);
      g_file_info_set_size (ret_info, stbuf.st_size);
    }
  else if (S_ISLNK (stbuf.st_mode))
    {
      char targetbuf[PATH_MAX+1];
      ssize_t len;

      g_file_info_set_file_type (ret_info, G_FILE_TYPE_SYMBOLIC_LINK);
      
      do
        len = readlinkat (self->objects_dir_fd, loose_path_buf, targetbuf, sizeof (targetbuf) - 1);
      while (G_UNLIKELY (len == -1 && errno == EINTR));
      if (len == -1)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
      targetbuf[len] = '\0';
      g_file_info_set_symlink_target (ret_info, targetbuf);
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Not a regular file or symlink: %s", loose_path_buf);
      goto out;
    }

  g_file_info_set_attribute_boolean (ret_info, "standard::is-symlink", S_ISLNK (stbuf.st_mode));
  g_file_info_set_attribute_uint32 (ret_info, "unix::uid", stbuf.st_uid);
  g_file_info_set_attribute_uint32 (ret_info, "unix::gid", stbuf.st_gid);
  g_file_info_set_attribute_uint32 (ret_info, "unix::mode", stbuf.st_mode);

  ret = TRUE;
  gs_transfer_out_value (out_info, &ret_info);
 out:
  return ret;
}

/**
 * ostree_repo_load_file:
 * @self: Repo
 * @checksum: ASCII SHA256 checksum
 * @out_input: (out) (allow-none): File content
 * @out_file_info: (out) (allow-none): File information
 * @out_xattrs: (out) (allow-none): Extended attributes
 * @cancellable: Cancellable
 * @error: Error
 *
 * Load content object, decomposing it into three parts: the actual
 * content (for regular files), the metadata, and extended attributes.
 */
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
  gboolean found = FALSE;
  OstreeRepoMode repo_mode;
  gs_unref_object GInputStream *ret_input = NULL;
  gs_unref_object GFileInfo *ret_file_info = NULL;
  gs_unref_variant GVariant *ret_xattrs = NULL;

  repo_mode = ostree_repo_get_mode (self);

  if (repo_mode == OSTREE_REPO_MODE_ARCHIVE_Z2)
    {
      char loose_path_buf[_OSTREE_LOOSE_PATH_MAX];
      int fd = -1;
      struct stat stbuf;
      gs_unref_object GInputStream *tmp_stream = NULL;

      _ostree_loose_path (loose_path_buf, checksum, OSTREE_OBJECT_TYPE_FILE, self->mode);

      if (!openat_allow_noent (self->objects_dir_fd, loose_path_buf, &fd,
                               cancellable, error))
        goto out;

      if (fd != -1)
        {
          tmp_stream = g_unix_input_stream_new (fd, TRUE);
          fd = -1; /* Transfer ownership */
          
          if (!gs_stream_fstat ((GFileDescriptorBased*) tmp_stream, &stbuf,
                                cancellable, error))
            goto out;
          
          if (!ostree_content_stream_parse (TRUE, tmp_stream, stbuf.st_size, TRUE,
                                            out_input ? &ret_input : NULL,
                                            &ret_file_info, &ret_xattrs,
                                            cancellable, error))
            goto out;

          found = TRUE;
        }
    }
  else
    {
      char loose_path_buf[_OSTREE_LOOSE_PATH_MAX];

      _ostree_loose_path (loose_path_buf, checksum, OSTREE_OBJECT_TYPE_FILE, self->mode);

      if (!query_info_for_bare_content_object (self, loose_path_buf,
                                               &ret_file_info,
                                               cancellable, error))
        goto out;

      if (ret_file_info)
        {
          if (out_xattrs)
            {
              gs_unref_object GFile *full_path =
                    _ostree_repo_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_FILE);

              if (!ostree_get_xattrs_for_file (full_path, &ret_xattrs,
                                               cancellable, error))
                goto out;
            }

          if (out_input && g_file_info_get_file_type (ret_file_info) == G_FILE_TYPE_REGULAR)
            {
              int fd = -1;
              if (!gs_file_openat_noatime (self->objects_dir_fd, loose_path_buf, &fd,
                                           cancellable, error))
                goto out;
              ret_input = g_unix_input_stream_new (fd, TRUE);
            }
          
          found = TRUE;
        }
    }
  
  if (!found)
    {
      if (self->parent_repo)
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
    }

  ret = TRUE;
  ot_transfer_out_value (out_input, &ret_input);
  ot_transfer_out_value (out_file_info, &ret_file_info);
  ot_transfer_out_value (out_xattrs, &ret_xattrs);
 out:
  return ret;
}

/**
 * ostree_repo_load_object_stream:
 * @self: Repo
 * @objtype: Object type
 * @checksum: ASCII SHA256 checksum
 * @out_input: (out): Stream for object
 * @out_size: (out): Length of @out_input
 * @cancellable: Cancellable
 * @error: Error
 *
 * Load object as a stream; useful when copying objects between
 * repositories.
 */
gboolean
ostree_repo_load_object_stream (OstreeRepo         *self,
                                OstreeObjectType    objtype,
                                const char         *checksum,
                                GInputStream      **out_input,
                                guint64            *out_size,
                                GCancellable       *cancellable,
                                GError            **error)
{
  gboolean ret = FALSE;
  guint64 size;
  gs_unref_object GInputStream *ret_input = NULL;

  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    {
      if (!load_metadata_internal (self, objtype, checksum, TRUE, NULL,
                                   &ret_input, &size,
                                   cancellable, error))
        goto out;
    }
  else
    {
      gs_unref_object GInputStream *input = NULL;
      gs_unref_object GFileInfo *finfo = NULL;
      gs_unref_variant GVariant *xattrs = NULL;

      if (!ostree_repo_load_file (self, checksum, &input, &finfo, &xattrs,
                                  cancellable, error))
        goto out;

      if (!ostree_raw_file_to_content_stream (input, finfo, xattrs,
                                              &ret_input, &size,
                                              cancellable, error))
        goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_input, &ret_input);
  *out_size = size;
 out:
  return ret;
}

/*
 * _ostree_repo_has_loose_object:
 * @loose_path_buf: Buffer of size _OSTREE_LOOSE_PATH_MAX
 *
 * Locate object in repository; if it exists, @out_is_stored will be
 * set to TRUE.  @loose_path_buf is always set to the loose path.
 */
gboolean
_ostree_repo_has_loose_object (OstreeRepo           *self,
                               const char           *checksum,
                               OstreeObjectType      objtype,
                               gboolean             *out_is_stored,
                               char                 *loose_path_buf,
                               GCancellable         *cancellable,
                               GError             **error)
{
  gboolean ret = FALSE;
  struct stat stbuf;
  int res;

  _ostree_loose_path (loose_path_buf, checksum, objtype, self->mode);

  do
    res = fstatat (self->objects_dir_fd, loose_path_buf, &stbuf, AT_SYMLINK_NOFOLLOW);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1 && errno != ENOENT)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  ret = TRUE;
  *out_is_stored = (res != -1);
 out:
  return ret;
}

gboolean
_ostree_repo_find_object (OstreeRepo           *self,
                          OstreeObjectType      objtype,
                          const char           *checksum,
                          GFile               **out_stored_path,
                          GCancellable         *cancellable,
                          GError             **error)
{
  gboolean ret = FALSE;
  gboolean has_object;
  char loose_path[_OSTREE_LOOSE_PATH_MAX];

  if (!_ostree_repo_has_loose_object (self, checksum, objtype, &has_object, loose_path, 
                                      cancellable, error))
    goto out;

  ret = TRUE;
  if (has_object)
    *out_stored_path = g_file_resolve_relative_path (self->objects_dir, loose_path);
  else
    *out_stored_path = NULL;
out:
  return ret;
}

/**
 * ostree_repo_has_object:
 * @self: Repo
 * @objtype: Object type
 * @checksum: ASCII SHA256 checksum
 * @out_have_object: (out): %TRUE if repository contains object
 * @cancellable: Cancellable
 * @error: Error
 *
 * Set @out_have_object to %TRUE if @self contains the given object;
 * %FALSE otherwise.
 *
 * Returns: %FALSE if an unexpected error occurred, %TRUE otherwise
 */
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

  if (!_ostree_repo_find_object (self, objtype, checksum, &loose_path,
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

/**
 * ostree_repo_delete_object:
 * @self: Repo
 * @objtype: Object type
 * @sha256: Checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Remove the object of type @objtype with checksum @sha256
 * from the repository.  An error of type %G_IO_ERROR_NOT_FOUND
 * is thrown if the object does not exist.
 */
gboolean
ostree_repo_delete_object (OstreeRepo           *self,
                           OstreeObjectType      objtype,
                           const char           *sha256,
                           GCancellable         *cancellable,
                           GError              **error)
{
  gs_unref_object GFile *objpath = _ostree_repo_get_object_path (self, sha256, objtype);
  return gs_file_unlink (objpath, cancellable, error);
}

/**
 * ostree_repo_query_object_storage_size:
 * @self: Repo
 * @objtype: Object type
 * @sha256: Checksum
 * @out_size: (out): Size in bytes object occupies physically
 * @cancellable: Cancellable
 * @error: Error
 *
 * Return the size in bytes of object with checksum @sha256, after any
 * compression has been applied.
 */
gboolean
ostree_repo_query_object_storage_size (OstreeRepo           *self,
                                       OstreeObjectType      objtype,
                                       const char           *sha256,
                                       guint64              *out_size,
                                       GCancellable         *cancellable,
                                       GError              **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *objpath = _ostree_repo_get_object_path (self, sha256, objtype);
  gs_unref_object GFileInfo *finfo = g_file_query_info (objpath, OSTREE_GIO_FAST_QUERYINFO,
                                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                        cancellable, error);
  if (!finfo)
    goto out;

  *out_size = g_file_info_get_size (finfo);
  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_load_variant_if_exists:
 * @self: Repo
 * @objtype: Object type
 * @sha256: ASCII checksum
 * @out_variant: (out) (transfer full): Metadata
 * @error: Error
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
  return load_metadata_internal (self, objtype, sha256, FALSE,
                                 out_variant, NULL, NULL, NULL, error);
}

/**
 * ostree_repo_load_variant:
 * @self: Repo
 * @objtype: Expected object type
 * @sha256: Checksum string
 * @out_variant: (out): (transfer full): Metadata object
 * @error: Error
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
  return load_metadata_internal (self, objtype, sha256, TRUE,
                                 out_variant, NULL, NULL, NULL, error);
}

/**
 * ostree_repo_list_objects:
 * @self: Repo
 * @flags: Flags controlling enumeration
 * @out_objects: (out): Map of serialized object name to variant data
 * @cancellable: Cancellable
 * @error: Error
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

/**
 * ostree_repo_read_commit:
 * @self: Repo
 * @rev: Revision (ref or ASCII checksum)
 * @out_root: (out): An #OstreeRepoFile corresponding to the root
 * @cancellable: Cancellable
 * @error: Error
 *
 * Load the content for @rev into @out_root.
 */
gboolean
ostree_repo_read_commit (OstreeRepo *self,
                         const char *rev,
                         GFile       **out_root,
                         GCancellable *cancellable,
                         GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object OstreeRepoFile *ret_root = NULL;
  gs_free char *resolved_rev = NULL;

  if (!ostree_repo_resolve_rev (self, rev, FALSE, &resolved_rev, error))
    goto out;

  ret_root = _ostree_repo_file_new_for_commit (self, resolved_rev, error);
  if (!ret_root)
    goto out;

  if (!ostree_repo_file_ensure_resolved (ret_root, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_root, (GFile **) &ret_root);
 out:
  return ret;
}

#ifndef HAVE_LIBSOUP
/**
 * ostree_repo_pull:
 * @self: Repo
 * @remote_name: Name of remote
 * @refs_to_fetch: (array zero-terminated=1) (element-type utf8) (allow-none): Optional list of refs; if %NULL, fetch all configured refs
 * @flags: Options controlling fetch behavior
 * @cancellable: Cancellable
 * @error: Error
 *
 * Connect to the remote repository, fetching the specified set of
 * refs @refs_to_fetch.  For each ref that is changed, download the
 * commit, all metadata, and all content objects, storing them safely
 * on disk in @self.
 */
gboolean
ostree_repo_pull (OstreeRepo               *self,
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
