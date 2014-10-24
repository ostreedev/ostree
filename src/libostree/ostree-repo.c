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
#include <gio/gfiledescriptorbased.h>
#include "otutil.h"
#include "libgsystem.h"

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-file.h"
#include "ostree-repo-file-enumerator.h"
#include "ostree-gpg-verifier.h"

#ifdef HAVE_GPGME
#include <locale.h>
#include <gpgme.h>
#include <glib/gstdio.h>
#endif

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
  g_clear_object (&self->deltas_dir);
  g_clear_object (&self->state_dir);
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
  g_clear_pointer (&self->object_sizes, (GDestroyNotify) g_hash_table_unref);
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
  self->deltas_dir = g_file_get_child (self->repodir, "deltas");
  self->uncompressed_objects_dir = g_file_get_child (self->repodir, "uncompressed-objects-cache");
  self->state_dir = g_file_get_child (self->repodir, "state");
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

static GFile *
get_default_repo_path (void)
{
  return g_file_new_for_path ("/ostree/repo");
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
      gs_unref_object GFile *default_repo_path = get_default_repo_path ();
      return ostree_repo_new (default_repo_path);
    }
}

/**
 * ostree_repo_is_system:
 * @repo: Repository
 *
 * Returns: %TRUE if this repository is the root-owned system global repository
 */
gboolean
ostree_repo_is_system (OstreeRepo   *repo)
{
  gs_unref_object GFile *default_repo_path = get_default_repo_path ();
  return g_file_equal (repo->repodir, default_repo_path);
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

/* Bind a subset of an a{sv} to options in a given GKeyfile section */
static void
keyfile_set_from_vardict (GKeyFile     *keyfile,
                          const char   *section,
                          GVariant     *vardict)
{
  GVariantIter viter;
  const char *key;
  GVariant *val;

  g_variant_iter_init (&viter, vardict);
  while (g_variant_iter_loop (&viter, "{&s@v}", &key, &val))
    {
      gs_unref_variant GVariant *child = g_variant_get_variant (val);
      if (g_variant_is_of_type (child, G_VARIANT_TYPE_STRING))
        g_key_file_set_string (keyfile, section, key, g_variant_get_string (child, NULL));
      else if (g_variant_is_of_type (child, G_VARIANT_TYPE_BOOLEAN))
        g_key_file_set_boolean (keyfile, section, key, g_variant_get_boolean (child));
      else if (g_variant_is_of_type (child, G_VARIANT_TYPE_STRING_ARRAY))
        {
          gsize len;
          const char *const*strv_child = g_variant_get_strv (child, &len);
          g_key_file_set_string_list (keyfile, section, key, strv_child, len);
        }
      else
        g_critical ("Unhandled type '%s' in " G_GNUC_FUNCTION,
                    (char*)g_variant_get_type (child));
    }
}

GS_DEFINE_CLEANUP_FUNCTION0(GKeyFile*, local_keyfile_unref, g_key_file_unref)
#define local_cleanup_keyfile __attribute__ ((cleanup(local_keyfile_unref)))

/**
 * ostree_repo_remote_add:
 * @self: Repo
 * @name: Name of remote
 * @url: URL for remote (if URL begins with metalink=, it will be used as such)
 * @options: (allow-none): GVariant of type a{sv}
 * @cancellable: Cancellable
 * @error: Error
 *
 * Create a new remote named @name pointing to @url.  If @options is
 * provided, then it will be mapped to #GKeyFile entries, where the
 * GVariant dictionary key is an option string, and the value is
 * mapped as follows:
 *   * s: g_key_file_set_string()
 *   * b: g_key_file_set_boolean()
 *   * as: g_key_file_set_string_list()
 *
 */
gboolean
ostree_repo_remote_add (OstreeRepo     *self,
                        const char     *name,
                        const char     *url,
                        GVariant       *options,
                        GCancellable   *cancellable,
                        GError        **error)
{
  gboolean ret = FALSE;
  gboolean is_system;
  gs_free char *section = NULL;
  gs_unref_object GFile *etc_ostree_remotes_d = g_file_new_for_path (SYSCONFDIR "/ostree/remotes.d");
  local_cleanup_keyfile GKeyFile *target_keyfile = NULL;
  gs_free char *target_name = NULL;
  gs_unref_object GFile *target_conf = NULL;

  g_return_val_if_fail (name != NULL, FALSE);
  g_return_val_if_fail (url != NULL, FALSE);
  g_return_val_if_fail (g_variant_is_of_type (options, G_VARIANT_TYPE ("a{sv}")), FALSE);

  if (strchr (name, '/') != NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid character '/' in remote name: %s",
                   name);
      goto out;
    }

  section = g_strdup_printf ("remote \"%s\"", name);

  is_system = ostree_repo_is_system (self);
  if (is_system)
    {
      target_keyfile = g_key_file_new ();

      target_name = g_strconcat (name, ".conf", NULL);
      target_conf = g_file_get_child (etc_ostree_remotes_d, target_name);
          
      if (g_file_query_exists (target_conf, NULL))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Remote configuration already exists: %s",
                       gs_file_get_path_cached (target_conf));
          goto out;
        }
    }
  else
    {
      target_keyfile = ostree_repo_copy_config (self);
    }

  if (g_str_has_prefix (url, "metalink="))
    g_key_file_set_string (target_keyfile, section, "metalink", url + strlen ("metalink="));
  else
    g_key_file_set_string (target_keyfile, section, "url", url);

  if (options)
    keyfile_set_from_vardict (target_keyfile, section, options);

  if (is_system)
    {
      gsize len;
      gs_free char *data = g_key_file_to_data (target_keyfile, &len, error);
      if (!g_file_replace_contents (target_conf, data, len,
                                    NULL, FALSE, 0, NULL,
                                    cancellable, error))
        goto out;
    }
  else
    {
      if (!ostree_repo_write_config (self, target_keyfile, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_remote_delete:
 * @self: Repo
 * @name: Name of remote
 * @cancellable: Cancellable
 * @error: Error
 *
 * Delete the remote named @name.  It is an error if the provided
 * remote does not exist.
 *
 */
gboolean
ostree_repo_remote_delete (OstreeRepo     *self,
                           const char     *name,
                           GCancellable   *cancellable,
                           GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *etc_ostree_remotes_d = g_file_new_for_path (SYSCONFDIR "/ostree/remotes.d");
  local_cleanup_keyfile GKeyFile *target_keyfile = NULL;
  gs_free char *section = NULL;
  gs_unref_object GFile *target_conf = NULL;
  gboolean is_system;

  g_return_val_if_fail (name != NULL, FALSE);

  if (strchr (name, '/') != NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid character '/' in remote name: %s",
                   name);
      goto out;
    }

  section = g_strdup_printf ("remote \"%s\"", name);

  /* Note we prefer deleting from the config if it exists there */
  if (g_key_file_has_group (self->config, section))
    is_system = FALSE;
  else
    is_system = ostree_repo_is_system (self);

  if (is_system)
    {
      gs_free char *target_name = NULL;

      target_name = g_strconcat (name, ".conf", NULL);
      target_conf = g_file_get_child (etc_ostree_remotes_d, target_name);

      if (!gs_file_unlink (target_conf, cancellable, error))
        goto out;
    }
  else
    {
      gsize len;
      gs_free char *data = NULL;

      target_conf = g_object_ref (self->config_file);

      target_keyfile = ostree_repo_copy_config (self);

      if (!g_key_file_remove_group (target_keyfile, section, error))
        goto out;

      data = g_key_file_to_data (target_keyfile, &len, NULL);
      if (!g_file_replace_contents (target_conf, data, len,
                                    NULL, FALSE, 0, NULL,
                                    cancellable, error))
        goto out;
    }

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

  g_clear_object (&child);
  child = g_file_get_child (self->repodir, "state");
  if (!g_file_make_directory (child, cancellable, error))
    goto out;

  if (!ostree_repo_open (self, cancellable, error))
    goto out;

  ret = TRUE;

 out:
  if (config_data)
    g_string_free (config_data, TRUE);
  return ret;
}

static gboolean
enumerate_directory_allow_noent (GFile               *dirpath,
                                 const char          *queryargs,
                                 GFileQueryInfoFlags  queryflags,
                                 GFileEnumerator    **out_direnum,
                                 GCancellable        *cancellable,
                                 GError             **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  gs_unref_object GFileEnumerator *ret_direnum = NULL;

  ret_direnum = g_file_enumerate_children (dirpath, queryargs, queryflags,
                                           cancellable, &temp_error);
  if (!ret_direnum)
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

  ret = TRUE;
  gs_transfer_out_value (out_direnum, &ret_direnum);
 out:
  return ret;
}

static gboolean
append_one_remote_config (OstreeRepo      *self,
                          GFile           *path,
                          GCancellable    *cancellable,
                          GError         **error)
{
  gboolean ret = FALSE;
  local_cleanup_keyfile GKeyFile *remotedata = g_key_file_new ();
  gs_strfreev char **groups = NULL;
  char **iter;

  if (!g_key_file_load_from_file (remotedata, gs_file_get_path_cached (path),
                                  0, error))
    goto out;

  groups = g_key_file_get_groups (remotedata, NULL);
  for (iter = groups; iter && *iter; iter++)
    {
      const char *group = *iter;
      char **subiter;
      gs_strfreev char **keys = NULL;
              
      /* Whitelist of allowed groups for now */
      if (!g_str_has_prefix (group, "remote \""))
        continue;

      if (g_key_file_has_group (self->config, group))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Multiple specifications found for %s", group);
          goto out;
        }
              
      keys = g_key_file_get_keys (remotedata, group, NULL, NULL);
      g_assert (keys);
      for (subiter = keys; subiter && *subiter; subiter++)
        {
          const char *key = *subiter;
          gs_free char *value = g_key_file_get_value (remotedata, group, key, NULL);
          g_assert (value);
          g_key_file_set_value (self->config, group, key, value);
        }
    }

  ret = TRUE;
 out:
  return ret;
}
                                 
static gboolean
append_remotes_d (OstreeRepo          *self,
                  GCancellable        *cancellable,
                  GError             **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *etc_ostree_remotes_d = NULL;
  gs_unref_object GFileEnumerator *direnum = NULL;

  etc_ostree_remotes_d = g_file_new_for_path (SYSCONFDIR "/ostree/remotes.d");
  if (!enumerate_directory_allow_noent (etc_ostree_remotes_d, OSTREE_GIO_FAST_QUERYINFO, 0,
                                        &direnum,
                                        cancellable, error))
    goto out;
  if (direnum)
    {
      while (TRUE)
        {
          GFileInfo *file_info;
          GFile *path;
          const char *name;
          guint32 type;

          if (!gs_file_enumerator_iterate (direnum, &file_info, &path,
                                           NULL, error))
            goto out;
          if (file_info == NULL)
            break;

          name = g_file_info_get_attribute_byte_string (file_info, "standard::name");
          type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

          if (type == G_FILE_TYPE_REGULAR &&
              g_str_has_suffix (name, ".conf"))
            {
              if (!append_one_remote_config (self, path, cancellable, error))
                goto out;
            }
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_repo_open (OstreeRepo    *self,
                  GCancellable  *cancellable,
                  GError       **error)
{
  gboolean ret = FALSE;
  gboolean is_archive;
  struct stat stbuf;
  gs_free char *version = NULL;
  gs_free char *mode = NULL;
  gs_free char *parent_repo_path = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->inited)
    return TRUE;

  if (!gs_file_open_dir_fd (self->objects_dir, &self->objects_dir_fd, cancellable, error))
    {
      g_prefix_error (error, "Reading objects/ directory: ");
      goto out;
    }

  self->writable = faccessat (AT_FDCWD, gs_file_get_path_cached (self->objects_dir), W_OK, 0) == 0;

  if (fstat (self->objects_dir_fd, &stbuf) != 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  if (stbuf.st_uid != getuid () || stbuf.st_gid != getgid ())
    {
      self->target_owner_uid = stbuf.st_uid;
      self->target_owner_gid = stbuf.st_gid;
    }
  else
    {
      self->target_owner_uid = self->target_owner_gid = -1;
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

  if (self->writable)
    {
      if (!ot_keyfile_get_boolean_with_default (self->config, "core", "enable-uncompressed-cache",
                                                TRUE, &self->enable_uncompressed_cache, error))
        goto out;
    }
  else
    self->enable_uncompressed_cache = FALSE;

  {
    gboolean do_fsync;
    
    if (!ot_keyfile_get_boolean_with_default (self->config, "core", "fsync",
                                              TRUE, &do_fsync, error))
      goto out;
    
    if (!do_fsync)
      ostree_repo_set_disable_fsync (self, TRUE);
  }

  {
    gs_unref_object GFile *default_repo_path = get_default_repo_path ();
    
    if (g_file_equal (self->repodir, default_repo_path))
      {
        if (!append_remotes_d (self, cancellable, error))
          goto out;
      }
  }

  if (!gs_file_open_dir_fd (self->tmp_dir, &self->tmp_dir_fd, cancellable, error))
    goto out;

  if (self->mode == OSTREE_REPO_MODE_ARCHIVE_Z2 && self->enable_uncompressed_cache)
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
 * ostree_repo_set_disable_fsync:
 * @self: An #OstreeRepo
 * @disable_fsync: If %TRUE, do not fsync
 *
 * Disable requests to fsync() to stable storage during commits.  This
 * option should only be used by build system tools which are creating
 * disposable virtual machines, or have higher level mechanisms for
 * ensuring data consistency.
 */
void
ostree_repo_set_disable_fsync (OstreeRepo    *self,
                               gboolean       disable_fsync)
{
  self->disable_fsync = disable_fsync;
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
list_loose_objects_at (OstreeRepo             *self,
                       GHashTable             *inout_objects,
                       const char             *prefix,
                       int                     dfd,
                       const char             *commit_starting_with,
                       GCancellable           *cancellable,
                       GError                **error)
{
  gboolean ret = FALSE;
  DIR *d = NULL;
  struct dirent *dent;
  GVariant *key, *value;

  d = fdopendir (dfd);
  if (!d)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  while ((dent = readdir (d)) != NULL)
    {
      const char *name = dent->d_name;
      const char *dot;
      OstreeObjectType objtype;
      char buf[65];

      if (strcmp (name, ".") == 0 ||
          strcmp (name, "..") == 0)
        continue;

      dot = strrchr (name, '.');
      if (!dot)
        continue;

      if ((self->mode == OSTREE_REPO_MODE_ARCHIVE_Z2
           && strcmp (dot, ".filez") == 0) ||
          (self->mode == OSTREE_REPO_MODE_BARE
           && strcmp (dot, ".file") == 0))
        objtype = OSTREE_OBJECT_TYPE_FILE;
      else if (strcmp (dot, ".dirtree") == 0)
        objtype = OSTREE_OBJECT_TYPE_DIR_TREE;
      else if (strcmp (dot, ".dirmeta") == 0)
        objtype = OSTREE_OBJECT_TYPE_DIR_META;
      else if (strcmp (dot, ".commit") == 0)
        objtype = OSTREE_OBJECT_TYPE_COMMIT;
      else
        continue;

      if ((dot - name) != 62)
        continue;

      memcpy (buf, prefix, 2);
      memcpy (buf + 2, name, 62);
      buf[sizeof(buf)-1] = '\0';

      /* if we passed in a "starting with" argument, then
         we only want to return .commit objects with a checksum
         that matches the commit_starting_with argument */
      if (commit_starting_with)
        {
          /* object is not a commit, do not add to array */
          if (objtype != OSTREE_OBJECT_TYPE_COMMIT)
              continue;

          /* commit checksum does not match "starting with", do not add to array */     
          if (!g_str_has_prefix (buf, commit_starting_with))
            continue;
        }

        key = ostree_object_name_serialize (buf, objtype);
        value = g_variant_new ("(b@as)",
                               TRUE, g_variant_new_strv (NULL, 0));
        /* transfer ownership */
        g_hash_table_replace (inout_objects, key,
                              g_variant_ref_sink (value));
    }

  ret = TRUE;
 out:
  if (d)
    (void) closedir (d);
  return ret;
}

static gboolean
list_loose_objects (OstreeRepo                     *self,
                    GHashTable                     *inout_objects,
                    const char                     *commit_starting_with,
                    GCancellable                   *cancellable,
                    GError                        **error)
{
  gboolean ret = FALSE;
  guint c;
  int dfd = -1;
  static const gchar hexchars[] = "0123456789abcdef";

  for (c = 0; c < 256; c++)
    {
      char buf[3];
      buf[0] = hexchars[c >> 4];
      buf[1] = hexchars[c & 0xF];
      buf[2] = '\0';
      dfd = ot_opendirat (self->objects_dir_fd, buf, FALSE);
      if (dfd == -1)
        {
          if (errno == ENOENT)
            continue;
          else
            {
              ot_util_set_error_from_errno (error, errno);
              goto out;
            }
        }
      /* Takes ownership of dfd */
      if (!list_loose_objects_at (self, inout_objects, buf, dfd,
                                       commit_starting_with,
                                       cancellable, error))
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

              if (!gs_file_get_all_xattrs (full_path, &ret_xattrs,
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
  gboolean ret = FALSE;
  gs_unref_object GFile *objpath = NULL;

  if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
    {
      gs_unref_object GFile *detached_metadata =
        _ostree_repo_get_commit_metadata_loose_path (self, sha256);
      if (!ot_gfile_ensure_unlinked (detached_metadata, cancellable, error))
        goto out;
    }

  objpath = _ostree_repo_get_object_path (self, sha256, objtype);
  if (!gs_file_unlink (objpath, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
copy_detached_metadata (OstreeRepo    *self,
                        OstreeRepo    *source,
                        const char   *checksum,
                        GCancellable  *cancellable,
                        GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *detached_meta = NULL;
          
  if (!ostree_repo_read_commit_detached_metadata (source,
                                                  checksum, &detached_meta,
                                                  cancellable, error))
    goto out;

  if (detached_meta)
    {
      if (!ostree_repo_write_commit_detached_metadata (self,
                                                       checksum, detached_meta,
                                                       cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
import_one_object_copy (OstreeRepo    *self,
                        OstreeRepo    *source,
                        const char   *checksum,
                        OstreeObjectType objtype,
                        GCancellable  *cancellable,
                        GError        **error)
{
  gboolean ret = FALSE;
  guint64 length;
  gs_unref_object GInputStream *object = NULL;

  if (!ostree_repo_load_object_stream (source, objtype, checksum,
                                       &object, &length,
                                       cancellable, error))
    goto out;

  if (objtype == OSTREE_OBJECT_TYPE_FILE)
    {
      if (!ostree_repo_write_content_trusted (self, checksum,
                                              object, length,
                                              cancellable, error))
        goto out;
    }
  else
    {
      if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
        {
          if (!copy_detached_metadata (self, source, checksum, cancellable, error))
            goto out;
        }
      if (!ostree_repo_write_metadata_stream_trusted (self, objtype,
                                                      checksum, object, length,
                                                      cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
import_one_object_link (OstreeRepo    *self,
                        OstreeRepo    *source,
                        const char   *checksum,
                        OstreeObjectType objtype,
                        gboolean       *out_was_supported,
                        GCancellable  *cancellable,
                        GError        **error)
{
  gboolean ret = FALSE;
  char loose_path_buf[_OSTREE_LOOSE_PATH_MAX];

  _ostree_loose_path (loose_path_buf, checksum, objtype, self->mode);

  if (!_ostree_repo_ensure_loose_objdir_at (self->objects_dir_fd, loose_path_buf, cancellable, error))
    goto out;

  *out_was_supported = TRUE;
  if (linkat (source->objects_dir_fd, loose_path_buf, self->objects_dir_fd, loose_path_buf, 0) != 0)
    {
      if (errno == EEXIST)
        {
          ret = TRUE;
        }
      else if (errno == EMLINK || errno == EXDEV || errno == EPERM)
        {
          /* EMLINK, EXDEV and EPERM shouldn't be fatal; we just can't do the
           * optimization of hardlinking instead of copying.
           */
          *out_was_supported = FALSE;
          ret = TRUE;
        }
      else
        ot_util_set_error_from_errno (error, errno);
      
      goto out;
    }

  if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
    {
      if (!copy_detached_metadata (self, source, checksum, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_import_object_from:
 * @self: Destination repo
 * @source: Source repo
 * @objtype: Object type
 * @checksum: checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Copy object named by @objtype and @checksum into @self from the
 * source repository @source.  If both repositories are of the same
 * type and on the same filesystem, this will simply be a fast Unix
 * hard link operation.
 *
 * Otherwise, a copy will be performed.
 */
gboolean
ostree_repo_import_object_from (OstreeRepo           *self,
                                OstreeRepo           *source,
                                OstreeObjectType      objtype,
                                const char           *checksum, 
                                GCancellable         *cancellable,
                                GError              **error)
{
  gboolean ret = FALSE;
  gboolean hardlink_was_supported = FALSE;
      
  if (self->mode == source->mode)
    {
      if (!import_one_object_link (self, source, checksum, objtype,
                                   &hardlink_was_supported,
                                   cancellable, error))
        goto out;
    }

  if (!hardlink_was_supported)
    {
      gboolean has_object;

      if (!ostree_repo_has_object (self, objtype, checksum, &has_object,
                                   cancellable, error))
        goto out;
  
      if (!has_object)
        {
          if (!import_one_object_copy (self, source, checksum, objtype,
                                       cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
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
 * @out_variant: (out) (transfer full): Metadata object
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
      if (!list_loose_objects (self, ret_objects, NULL, cancellable, error))
        goto out;
      if (self->parent_repo)
        {
          if (!list_loose_objects (self->parent_repo, ret_objects, NULL, cancellable, error))
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
 * ostree_repo_list_commit_objects_starting_with:
 * @self: Repo
 * @start: List commits starting with this checksum
 * @out_commits: Array of GVariants
 * @cancellable: Cancellable
 * @error: Error
 *
 * This function synchronously enumerates all commit objects starting
 * with @start, returning data in @out_commits.
 *
 * Returns: %TRUE on success, %FALSE on error, and @error will be set
 */
gboolean
ostree_repo_list_commit_objects_starting_with (OstreeRepo                  *self,
                                               const char                  *start,
                                               GHashTable                 **out_commits,
                                               GCancellable                *cancellable,
                                               GError                     **error)
{
  gboolean ret = FALSE;
  gs_unref_hashtable GHashTable *ret_commits = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (self->inited, FALSE);

  ret_commits = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                       (GDestroyNotify) g_variant_unref,
                                       (GDestroyNotify) g_variant_unref);

  if (!list_loose_objects (self, ret_commits, start, cancellable, error))
        goto out;


  if (self->parent_repo)
    {
      if (!list_loose_objects (self->parent_repo, ret_commits, start,
                               cancellable, error))
        goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_commits, &ret_commits);
 out:
  return ret;
}

/**
 * ostree_repo_read_commit:
 * @self: Repo
 * @ref: Ref or ASCII checksum
 * @out_root: (out): An #OstreeRepoFile corresponding to the root
 * @out_commit: (out): The resolved commit checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Load the content for @rev into @out_root.
 */
gboolean
ostree_repo_read_commit (OstreeRepo   *self,
                         const char   *ref,
                         GFile       **out_root,
                         char        **out_commit,
                         GCancellable *cancellable,
                         GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *ret_root = NULL;
  gs_free char *resolved_commit = NULL;

  if (!ostree_repo_resolve_rev (self, ref, FALSE, &resolved_commit, error))
    goto out;

  ret_root = (GFile*) _ostree_repo_file_new_for_commit (self, resolved_commit, error);
  if (!ret_root)
    goto out;

  if (!ostree_repo_file_ensure_resolved ((OstreeRepoFile*)ret_root, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value(out_root, &ret_root);
  ot_transfer_out_value(out_commit, &resolved_commit);
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
 * @progress: (allow-none): Progress
 * @cancellable: Cancellable
 * @error: Error
 *
 * Connect to the remote repository, fetching the specified set of
 * refs @refs_to_fetch.  For each ref that is changed, download the
 * commit, all metadata, and all content objects, storing them safely
 * on disk in @self.
 *
 * If @flags contains %OSTREE_REPO_PULL_FLAGS_MIRROR, and
 * the @refs_to_fetch is %NULL, and the remote repository contains a
 * summary file, then all refs will be fetched.
 *
 * Warning: This API will iterate the thread default main context,
 * which is a bug, but kept for compatibility reasons.  If you want to
 * avoid this, use g_main_context_push_thread_default() to push a new
 * one around this call.
 */
gboolean
ostree_repo_pull (OstreeRepo               *self,
                  const char               *remote_name,
                  char                    **refs_to_fetch,
                  OstreeRepoPullFlags       flags,
                  OstreeAsyncProgress      *progress,
                  GCancellable             *cancellable,
                  GError                  **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "This version of ostree was built without libsoup, and cannot fetch over HTTP");
  return FALSE;
}

/**
 * ostree_repo_pull_one_dir:
 * @self: Repo
 * @remote_name: Name of remote
 * @dir_to_pull: Subdirectory path
 * @refs_to_fetch: (array zero-terminated=1) (element-type utf8) (allow-none): Optional list of refs; if %NULL, fetch all configured refs
 * @flags: Options controlling fetch behavior
 * @progress: (allow-none): Progress
 * @cancellable: Cancellable
 * @error: Error
 *
 * This is similar to ostree_repo_pull(), but only fetches a single
 * subpath.
 */
gboolean
ostree_repo_pull_one_dir (OstreeRepo               *self,
                          const char               *remote_name,
                          char                    **refs_to_fetch,
                          OstreeRepoPullFlags       flags,
                          OstreeAsyncProgress      *progress,
                          GCancellable             *cancellable,
                          GError                  **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "This version of ostree was built without libsoup, and cannot fetch over HTTP");
  return FALSE;
}

/**
 * ostree_repo_pull_with_options:
 * @self: Repo
 * @remote_name: Name of remote
 * @options: A GVariant a{sv} with an extensible set of flags.
 * @progress: (allow-none): Progress
 * @cancellable: Cancellable
 * @error: Error
 *
 * Like ostree_repo_pull(), but supports an extensible set of flags.
 * The following are currently defined:
 *
 *   * subdir (s): Pull just this subdirectory
 *   * flags (i): An instance of #OstreeRepoPullFlags
 *   * refs: (as): Array of string refs
 */
gboolean
ostree_repo_pull_with_options (OstreeRepo             *self,
                               const char             *remote_name,
                               GVariant               *options,
                               OstreeAsyncProgress    *progress,
                               GCancellable           *cancellable,
                               GError                **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "This version of ostree was built without libsoup, and cannot fetch over HTTP");
  return FALSE;
}

#endif

/**
 * ostree_repo_append_gpg_signature:
 * @self: Self
 * @commit_checksum: SHA256 of given commit to sign
 * @signature_bytes: Signature data
 * @cancellable: A #GCancellable
 * @error: a #GError
 *
 * Append a GPG signature to a commit.
 */
gboolean
ostree_repo_append_gpg_signature (OstreeRepo     *self,
                                  const gchar    *commit_checksum,
                                  GBytes         *signature_bytes,
                                  GCancellable   *cancellable,
                                  GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *metadata = NULL;
  gs_unref_variant_builder GVariantBuilder *builder = NULL;
  gs_unref_variant_builder GVariantBuilder *signature_builder = NULL;
  gs_unref_variant GVariant *signaturedata = NULL;

  if (!ostree_repo_read_commit_detached_metadata (self,
                                                  commit_checksum,
                                                  &metadata,
                                                  cancellable,
                                                  error))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to read existing detached metadata");
      goto out;
    }

  if (metadata)
    {
      builder = ot_util_variant_builder_from_variant (metadata, G_VARIANT_TYPE ("a{sv}"));
      signaturedata = g_variant_lookup_value (metadata, "ostree.gpgsigs", G_VARIANT_TYPE ("aay"));
      if (signaturedata)
        signature_builder = ot_util_variant_builder_from_variant (signaturedata, G_VARIANT_TYPE ("aay"));
    }
  if (!builder)
    builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
  if (!signature_builder)
    signature_builder = g_variant_builder_new (G_VARIANT_TYPE ("aay"));

  g_variant_builder_add (signature_builder, "@ay", ot_gvariant_new_ay_bytes (signature_bytes));

  g_variant_builder_add (builder, "{sv}", "ostree.gpgsigs", g_variant_builder_end (signature_builder));
  
  metadata = g_variant_builder_end (builder);

  if (!ostree_repo_write_commit_detached_metadata (self,
                                                   commit_checksum,
                                                   metadata,
                                                   cancellable,
                                                   error))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to read existing detached metadata");
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_sign_commit:
 * @self: Self
 * @commit_checksum: SHA256 of given commit to sign
 * @key_id: Use this GPG key id
 * @homedir: (allow-none): GPG home directory, or %NULL
 * @cancellable: A #GCancellable
 * @error: a #GError
 *
 * Add a GPG signature to a commit.
 */
gboolean
ostree_repo_sign_commit (OstreeRepo     *self,
                         const gchar    *commit_checksum,
                         const gchar    *key_id,
                         const gchar    *homedir,
                         GCancellable   *cancellable,
                         GError        **error)
{
#ifdef HAVE_GPGME
  gboolean ret = FALSE;
  gs_unref_object GFile *commit_path = NULL;
  gs_free gchar *commit_filename = NULL;
  gs_unref_object GFile *tmp_signature_file = NULL;
  gs_unref_object GOutputStream *tmp_signature_output = NULL;
  gs_unref_variant GVariant *commit_variant = NULL;
  gs_unref_bytes GBytes *signature_bytes = NULL;
  gpgme_ctx_t context = NULL;
  gpgme_engine_info_t info;
  gpgme_error_t err;
  gpgme_key_t key = NULL;
  gpgme_data_t commit_buffer = NULL;
  gpgme_data_t signature_buffer = NULL;
  int signature_fd = -1;
  GMappedFile *signature_file = NULL;
  
  if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_COMMIT,
                                 commit_checksum, &commit_variant, error))
    goto out;
  
  if (!gs_file_open_in_tmpdir (self->tmp_dir, 0644,
                               &tmp_signature_file, &tmp_signature_output,
                               cancellable, error))
    goto out;

  gpgme_check_version (NULL);
  gpgme_set_locale (NULL, LC_CTYPE, setlocale (LC_CTYPE, NULL));
  
  if ((err = gpgme_new (&context)) != GPG_ERR_NO_ERROR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to create gpg context");
      goto out;
    }

  info = gpgme_ctx_get_engine_info (context);

  if ((err = gpgme_set_protocol (context, GPGME_PROTOCOL_OpenPGP)) !=
      GPG_ERR_NO_ERROR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to set gpg protocol");
      goto out;
    }
  
  if (homedir != NULL)
    {
      if ((err = gpgme_ctx_set_engine_info (context, info->protocol, NULL, homedir))
          != GPG_ERR_NO_ERROR)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unable to set gpg homedir to '%s'",
                       homedir);
          goto out;
        }
    }

  /* Get the secret keys with the given key id */
  if ((err = gpgme_get_key (context, key_id, &key, 1)) != GPG_ERR_NO_ERROR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No gpg key found with ID %s (homedir: %s)", key_id,
                   homedir ? homedir : "<default>");
      goto out;
    }
  
  /* Add the key to the context as a signer */
  if ((err = gpgme_signers_add (context, key)) != GPG_ERR_NO_ERROR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Error signing commit");
      goto out;
    }
  
  if ((err = gpgme_data_new_from_mem (&commit_buffer, g_variant_get_data (commit_variant),
                                      g_variant_get_size (commit_variant), FALSE)) != GPG_ERR_NO_ERROR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create buffer from commit file");
      goto out;
    }
  
  signature_fd = g_file_descriptor_based_get_fd ((GFileDescriptorBased*)tmp_signature_output);
  if (signature_fd < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to open signature file");
      goto out;
    }
  
  if ((err = gpgme_data_new_from_fd (&signature_buffer, signature_fd)) != GPG_ERR_NO_ERROR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create buffer for signature file");
      goto out;
    }
  
  if ((err = gpgme_op_sign (context, commit_buffer, signature_buffer, GPGME_SIG_MODE_DETACH))
      != GPG_ERR_NO_ERROR)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failure signing commit file");
      goto out;
    }
  
  if (!g_output_stream_close (tmp_signature_output, cancellable, error))
    goto out;
  
  signature_file = gs_file_map_noatime (tmp_signature_file, cancellable, error);
  if (!signature_file)
    goto out;
  signature_bytes = g_mapped_file_get_bytes (signature_file);
  
  if (!ostree_repo_append_gpg_signature (self, commit_checksum, signature_bytes,
                                         cancellable, error))
    goto out;

  ret = TRUE;
out:
  if (commit_buffer)
    gpgme_data_release (commit_buffer);
  if (signature_buffer)
    gpgme_data_release (signature_buffer);
  if (key)
    gpgme_key_release (key);
  if (context)
    gpgme_release (context);
  if (signature_file)
    g_mapped_file_unref (signature_file);
  return ret;
#else
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This version of ostree was compiled without GPG support");
  return FALSE;
#endif
}

static gboolean
_ostree_repo_gpg_verify_file_with_metadata (OstreeRepo          *self,
                                            GFile               *path,
                                            GVariant            *metadata,
                                            GFile               *keyringdir,
                                            GFile               *extra_keyring,
                                            GCancellable        *cancellable,
                                            GError             **error)
{
#ifdef HAVE_GPGME
  gboolean ret = FALSE;
  gs_unref_object OstreeGpgVerifier *verifier = NULL;
  gs_unref_variant GVariant *signaturedata = NULL;
  gint i, n;
  gboolean had_valid_signataure = FALSE;

  verifier = _ostree_gpg_verifier_new (cancellable, error);
  if (!verifier)
    goto out;

  if (keyringdir)
    {
      if (!_ostree_gpg_verifier_add_keyring_dir (verifier, keyringdir,
                                                 cancellable, error))
        goto out;
    }
  if (extra_keyring != NULL)
    {
      if (!_ostree_gpg_verifier_add_keyring (verifier, extra_keyring,
                                             cancellable, error))
        goto out;
    }

  if (metadata)
    signaturedata = g_variant_lookup_value (metadata, "ostree.gpgsigs", G_VARIANT_TYPE ("aay"));
  if (!signaturedata)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "GPG verification enabled, but no signatures found (use gpg-verify=false in remote config to disable)");
      goto out;
    }

  n = g_variant_n_children (signaturedata);
  for (i = 0; i < n; i++)
    {
      GVariant *signature_variant = g_variant_get_child_value (signaturedata, i);
      gs_unref_object GFile *temp_sig_path = NULL;

      if (!gs_file_open_in_tmpdir (self->tmp_dir, 0644,
                                   &temp_sig_path, NULL,
                                   cancellable, error))
        goto out;

      if (!g_file_replace_contents (temp_sig_path,
                                    (char*)g_variant_get_data (signature_variant),
                                    g_variant_get_size (signature_variant),
                                    NULL, FALSE, 0, NULL,
                                    cancellable, error))
        goto out;

      if (!_ostree_gpg_verifier_check_signature (verifier,
                                                 path,
                                                 temp_sig_path,
                                                 &had_valid_signataure,
                                                 cancellable, error))
        {
          (void) gs_file_unlink (temp_sig_path, NULL, NULL);
          goto out;
        }
      (void) gs_file_unlink (temp_sig_path, NULL, NULL);
      if (had_valid_signataure)
        break;
    }
  
  if (!had_valid_signataure)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "GPG signatures found, but none are in trusted keyring");
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
#else
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This version of ostree was compiled without GPG support");
  return FALSE;
#endif
}

/**
 * ostree_repo_verify_commit:
 * @self: Repository
 * @commit_checksum: ASCII SHA256 checksum
 * @keyringdir: (allow-none): Path to directory GPG keyrings; overrides built-in default if given
 * @extra_keyring: (allow-none): Path to additional keyring file (not a directory)
 * @cancellable: Cancellable
 * @error: Error
 *
 * Check for a valid GPG signature on commit named by the ASCII
 * checksum @commit_checksum.
 */
gboolean
ostree_repo_verify_commit (OstreeRepo   *self,
                           const gchar  *commit_checksum,
                           GFile        *keyringdir,
                           GFile        *extra_keyring,
                           GCancellable *cancellable,
                           GError      **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *commit_variant = NULL;
  gs_unref_object GFile *commit_tmp_path = NULL;
  gs_unref_object GFile *keyringdir_ref = NULL;
  gs_unref_variant GVariant *metadata = NULL;
  gs_free gchar *commit_filename = NULL;

  /* Create a temporary file for the commit */
  if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_COMMIT,
                                 commit_checksum, &commit_variant,
                                 error))
    goto out;
  if (!gs_file_open_in_tmpdir (self->tmp_dir, 0644,
                               &commit_tmp_path, NULL,
                               cancellable, error))
    goto out;
  if (!g_file_replace_contents (commit_tmp_path,
                                (char*)g_variant_get_data (commit_variant),
                                g_variant_get_size (commit_variant),
                                NULL, FALSE, 0, NULL,
                                cancellable, error))
    goto out;

  /* Load the metadata */
  if (!ostree_repo_read_commit_detached_metadata (self,
                                                  commit_checksum,
                                                  &metadata,
                                                  cancellable,
                                                  error))
    {
      g_prefix_error (error, "Failed to read detached metadata: ");
      goto out;
    }
  
  if (!_ostree_repo_gpg_verify_file_with_metadata (self,
                                                   commit_tmp_path, metadata,
                                                   keyringdir, extra_keyring,
                                                   cancellable, error))
    goto out;
  
  ret = TRUE;
out:
  if (commit_tmp_path)
    (void) gs_file_unlink (commit_tmp_path, NULL, NULL);
  return ret;
}

/**
 * ostree_repo_regenerate_summary:
 * @self: Repo
 * @additional_metadata: (allow-none): A GVariant of type a{sv}, or %NULL
 * @cancellable: Cancellable
 * @error: Error
 *
 * An OSTree repository can contain a high level "summary" file that
 * describes the available branches and other metadata.
 *
 * It is not regenerated automatically when commits are created; this
 * API is available to atomically regenerate the summary after
 * multiple commits.  It should only be invoked by one process at a
 * time.
 */
gboolean
ostree_repo_regenerate_summary (OstreeRepo     *self,
                                GVariant       *additional_metadata,
                                GCancellable   *cancellable,
                                GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *summary_path = NULL;
  gs_unref_hashtable GHashTable *refs = NULL;
  gs_unref_variant_builder GVariantBuilder *refs_builder = NULL;
  gs_unref_variant GVariant *summary = NULL;
  GList *ordered_keys = NULL;
  GList *iter = NULL;

  if (!ostree_repo_list_refs (self, NULL, &refs, cancellable, error))
    goto out;

  refs_builder = g_variant_builder_new (G_VARIANT_TYPE ("a(s(taya{sv}))"));

  ordered_keys = g_hash_table_get_keys (refs);
  ordered_keys = g_list_sort (ordered_keys, (GCompareFunc)strcmp);
  
  for (iter = ordered_keys; iter; iter = iter->next)
    {
      const char *ref = iter->data;
      const char *commit = g_hash_table_lookup (refs, ref);
      gs_unref_variant GVariant *commit_obj = NULL;

      g_assert (commit);

      if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_COMMIT, commit, &commit_obj, error))
        goto out;

      g_variant_builder_add_value (refs_builder, 
                                   g_variant_new ("(s(t@ay@a{sv}))", ref,
                                                  g_variant_get_size (commit_obj),
                                                  ostree_checksum_to_bytes_v (commit),
                                                  ot_gvariant_new_empty_string_dict ()));
    }

  {
    gs_unref_variant_builder GVariantBuilder *summary_builder =
      g_variant_builder_new (OSTREE_SUMMARY_GVARIANT_FORMAT);

    g_variant_builder_add_value (summary_builder, g_variant_builder_end (refs_builder));
    g_variant_builder_add_value (summary_builder, additional_metadata ? additional_metadata : ot_gvariant_new_empty_string_dict ());
    summary = g_variant_builder_end (summary_builder);
    g_variant_ref_sink (summary);
  }

  summary_path = g_file_get_child (self->repodir, "summary");

  if (!ot_util_variant_save (summary_path, summary, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (ordered_keys)
    g_list_free (ordered_keys);
  return ret;
}

