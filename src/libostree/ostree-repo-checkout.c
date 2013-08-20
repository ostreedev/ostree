/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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
#include "otutil.h"

#include "ostree-repo-file.h"
#include "ostree-repo-private.h"

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
  gs_unref_object GFile *dir = NULL;
  gs_unref_object GFile *temp_file = NULL;
  gs_unref_object GFileInfo *temp_info = NULL;

  if (mode == OSTREE_REPO_CHECKOUT_MODE_USER)
    {
      temp_info = g_file_info_dup (finfo);
      
      g_file_info_set_attribute_uint32 (temp_info, "unix::uid", geteuid ());
      g_file_info_set_attribute_uint32 (temp_info, "unix::gid", getegid ());

      xattrs = NULL;
    }
  else
    temp_info = g_object_ref (finfo);

  if (overwrite_mode == OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES)
    {
      if (g_file_info_get_file_type (temp_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!ostree_create_file_from_input (file, temp_info,
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
                                                   temp_info, xattrs, input, &temp_file, 
                                                   cancellable, error))
            goto out;

          if (g_file_info_get_file_type (temp_info) == G_FILE_TYPE_REGULAR)
            {
              if (!gs_file_sync_data (temp_file, cancellable, error))
                goto out;
            }

          if (rename (gs_file_get_path_cached (temp_file), gs_file_get_path_cached (file)) < 0)
            {
              ot_util_set_error_from_errno (error, errno);
              goto out;
            }
        }
    }
  else
    {
      if (!ostree_create_file_from_input (file, temp_info,
                                          xattrs, input, cancellable, error))
        goto out;

      if (g_file_info_get_file_type (temp_info) == G_FILE_TYPE_REGULAR)
        {
          if (!gs_file_sync_data (file, cancellable, error))
            goto out;
        }
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
                        int                       dirfd,
                        gboolean                 *out_was_supported,
                        GCancellable             *cancellable,
                        GError                  **error)
{
  gboolean ret = FALSE;
  gboolean ret_was_supported = FALSE;
  gs_unref_object GFile *dir = NULL;

 again:
  if (dirfd != -1 &&
      linkat (-1, gs_file_get_path_cached (source),
              dirfd, gs_file_get_basename_cached (destination), 0) != -1)
    ret_was_supported = TRUE;
  else if (link (gs_file_get_path_cached (source), gs_file_get_path_cached (destination)) != -1)
    ret_was_supported = TRUE;
  else if (errno == EMLINK || errno == EXDEV || errno == EPERM)
    {
      /* EMLINK, EXDEV and EPERM shouldn't be fatal; we just can't do the
       * optimization of hardlinking instead of copying.
       */
      ret_was_supported = FALSE;
    }
  else if (errno == EEXIST && overwrite_mode == OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES)
    { 
      /* Idiocy, from man rename(2)
       *
       * "If oldpath and newpath are existing hard links referring to
       * the same file, then rename() does nothing, and returns a
       * success status."
       *
       * So we can't make this atomic.  
       */
      (void) unlink (gs_file_get_path_cached (destination));
      goto again;
      ret_was_supported = TRUE;
    }
  else
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  ret = TRUE;
  if (out_was_supported)
    *out_was_supported = ret_was_supported;
 out:
  return ret;
}

static gboolean
find_loose_for_checkout (OstreeRepo             *self,
                         const char             *checksum,
                         GFile                 **out_loose_path,
                         GCancellable           *cancellable,
                         GError                **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *path = NULL;
  struct stat stbuf;

  do
    {
      switch (self->mode)
        {
        case OSTREE_REPO_MODE_BARE:
          path = _ostree_repo_get_object_path (self, checksum, OSTREE_OBJECT_TYPE_FILE);
          break;
        case OSTREE_REPO_MODE_ARCHIVE_Z2:
          {
            if (self->enable_uncompressed_cache)
              path = _ostree_repo_get_uncompressed_object_cache_path (self, checksum);
            else
              path = NULL;
          }
          break;
        }

      if (!path)
        {
          self = self->parent_repo;
          continue;
        }

      if (lstat (gs_file_get_path_cached (path), &stbuf) < 0)
        {
          if (errno != ENOENT)
            {
              ot_util_set_error_from_errno (error, errno);
              goto out;
            }
          self = self->parent_repo;
        }
      else if (S_ISLNK (stbuf.st_mode))
        {
          /* Don't check out symbolic links via hardlink; it's very easy
           * to hit the maximum number of hardlinks to an inode this way,
           * especially since right now we have a lot of symbolic links to
           * busybox.
           *
           * fs/ext4/ext4.h:#define EXT4_LINK_MAX		65000
           */
          self = self->parent_repo;
        }
      else
        break;

      g_clear_object (&path);
    } while (self != NULL);

  ret = TRUE;
  ot_transfer_out_value (out_loose_path, &path);
 out:
  return ret;
}

static gboolean
checkout_one_file (OstreeRepo                        *repo,
                   GFile                             *source,
                   GFileInfo                         *source_info,
                   GFile                             *destination,
                   OstreeRepoCheckoutMode             mode,
                   OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                   GCancellable                      *cancellable,
                   GError                           **error)
{
  gboolean ret = FALSE;
  const char *checksum;
  gboolean is_symlink;
  gboolean hardlink_supported;
  gs_unref_object GFile *loose_path = NULL;
  gs_unref_object GInputStream *input = NULL;
  gs_unref_variant GVariant *xattrs = NULL;

  /* Hack to avoid trying to create device files as a user */
  if (mode == OSTREE_REPO_CHECKOUT_MODE_USER
      && g_file_info_get_file_type (source_info) == G_FILE_TYPE_SPECIAL)
    goto out;

  is_symlink = g_file_info_get_file_type (source_info) == G_FILE_TYPE_SYMBOLIC_LINK;

  checksum = ostree_repo_file_get_checksum ((OstreeRepoFile*)source);

  /* We can only do hardlinks in these scenarios */
  if (!is_symlink &&
      ((repo->mode == OSTREE_REPO_MODE_BARE && mode == OSTREE_REPO_CHECKOUT_MODE_NONE)
       || (repo->mode == OSTREE_REPO_MODE_ARCHIVE_Z2 && mode == OSTREE_REPO_CHECKOUT_MODE_USER)))
    {
      if (!find_loose_for_checkout (repo, checksum, &loose_path,
                                    cancellable, error))
        goto out;
    }
  /* Also, if we're archive-z and we didn't find an object, uncompress it now,
   * stick it in the cache, and then hardlink to that.
   */
  if (!is_symlink
      && loose_path == NULL
      && repo->mode == OSTREE_REPO_MODE_ARCHIVE_Z2
      && mode == OSTREE_REPO_CHECKOUT_MODE_USER
      && repo->enable_uncompressed_cache)
    {
      gs_unref_object GFile *objdir = NULL;

      loose_path = _ostree_repo_get_uncompressed_object_cache_path (repo, checksum);
      if (!ostree_repo_load_file (repo, checksum, &input, NULL, &xattrs,
                                  cancellable, error))
        goto out;

      objdir = g_file_get_parent (loose_path);
      if (!gs_file_ensure_directory (objdir, TRUE, cancellable, error))
        {
          g_prefix_error (error, "Creating cache directory %s: ",
                          gs_file_get_path_cached (objdir));
          goto out;
        }

      /* Use UNION_FILES to make this last-one-wins thread behavior
       * for now; we lose deduplication potentially, but oh well
       */ 
      if (!checkout_file_from_input (loose_path,
                                     OSTREE_REPO_CHECKOUT_MODE_USER,
                                     OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES,
                                     source_info, xattrs, 
                                     input, cancellable, error))
        {
          g_prefix_error (error, "Unpacking loose object %s: ", checksum);
          goto out;
        }

      /* Store the 2-byte objdir prefix (e.g. e3) in a set.  The basic
       * idea here is that if we had to unpack an object, it's very
       * likely we're replacing some other object, so we may need a GC.
       *
       * This model ensures that we do work roughly proportional to
       * the size of the changes.  For example, we don't scan any
       * directories if we didn't modify anything, meaning you can
       * checkout the same tree multiple times very quickly.
       *
       * This is also scale independent; we don't hardcode e.g. looking
       * at 1000 objects.
       *
       * The downside is that if we're unlucky, we may not free
       * an object for quite some time.
       */
      g_mutex_lock (&repo->cache_lock);
      {
        gpointer key = GUINT_TO_POINTER ((g_ascii_xdigit_value (checksum[0]) << 4) + 
                                         g_ascii_xdigit_value (checksum[1]));
        if (repo->updated_uncompressed_dirs == NULL)
          repo->updated_uncompressed_dirs = g_hash_table_new (NULL, NULL);
        g_hash_table_insert (repo->updated_uncompressed_dirs, key, key);
      }
      g_mutex_unlock (&repo->cache_lock);
    }

  if (loose_path)
    {
      /* If we found one, try hardlinking */
      if (!checkout_file_hardlink (repo, mode,
                                   overwrite_mode, loose_path,
                                   destination, -1,
                                   &hardlink_supported, cancellable, error))
        {
          g_prefix_error (error, "Hardlinking loose object %s to %s: ", checksum,
                          gs_file_get_path_cached (destination));
          goto out;
        }
    }

  /* Fall back to copy if there's no loose object, or we couldn't hardlink */
  if (loose_path == NULL || !hardlink_supported)
    {
      if (!ostree_repo_load_file (repo, checksum, &input, NULL, &xattrs,
                                  cancellable, error))
        goto out;

      if (!checkout_file_from_input (destination, mode, overwrite_mode,
                                     source_info, xattrs, 
                                     input, cancellable, error))
        {
          g_prefix_error (error, "Copying object %s to %s: ", checksum,
                          gs_file_get_path_cached (destination));
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_checkout_tree:
 * @self: Repo
 * @mode: Options controlling all files
 * @overwrite_mode: Whether or not to overwrite files
 * @destination: Place tree here
 * @source: Source tree
 * @source_info: Source info
 * @cancellable: Cancellable
 * @error: Error
 *
 * Check out @source into @destination, which must live on the
 * physical filesystem.  @source may be any subdirectory of a given
 * commit.  The @mode and @overwrite_mode allow control over how the
 * files are checked out.
 */
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
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_variant GVariant *xattrs = NULL;
  gs_unref_object GFileEnumerator *dir_enum = NULL;

  if (!ostree_repo_file_get_xattrs (source, &xattrs, NULL, error))
    goto out;

  if (!checkout_file_from_input (destination,
                                 mode,
                                 overwrite_mode,
                                 source_info,
                                 xattrs, NULL,
                                 cancellable, error))
    goto out;

  g_clear_pointer (&xattrs, (GDestroyNotify) g_variant_unref);

  dir_enum = g_file_enumerate_children ((GFile*)source,
                                        OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *src_child;
      const char *name;
      gs_unref_object GFile *dest_path = NULL;

      if (!gs_file_enumerator_iterate (dir_enum, &file_info, &src_child,
                                       cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      name = g_file_info_get_name (file_info);
      dest_path = g_file_get_child (destination, name);

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!ostree_repo_checkout_tree (self, mode, overwrite_mode, dest_path,
                                          (OstreeRepoFile*)src_child, file_info,
                                          cancellable, error))
            goto out;
        }
      else
        {
          if (!checkout_one_file (self, src_child, file_info, dest_path,
                                  mode, overwrite_mode,
                                  cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_checkout_gc:
 * @self: Repo
 * @cancellable: Cancellable
 * @error: Error
 *
 * Call this after finishing a succession of checkout operations; it
 * will delete any currently-unused uncompressed objects from the
 * cache.
 */
gboolean
ostree_repo_checkout_gc (OstreeRepo        *self,
                         GCancellable      *cancellable,
                         GError           **error)
{
  gboolean ret = FALSE;
  gs_unref_hashtable GHashTable *to_clean_dirs = NULL;
  GHashTableIter iter;
  gpointer key, value;

  g_mutex_lock (&self->cache_lock);
  to_clean_dirs = self->updated_uncompressed_dirs;
  self->updated_uncompressed_dirs = g_hash_table_new (NULL, NULL);
  g_mutex_unlock (&self->cache_lock);

  if (to_clean_dirs)
    g_hash_table_iter_init (&iter, to_clean_dirs);
  while (to_clean_dirs && g_hash_table_iter_next (&iter, &key, &value))
    {
      gs_unref_object GFile *objdir = NULL;
      gs_unref_object GFileEnumerator *enumerator = NULL;
      gs_free char *objdir_name = NULL;

      objdir_name = g_strdup_printf ("%02x", GPOINTER_TO_UINT (key));
      objdir = ot_gfile_get_child_build_path (self->uncompressed_objects_dir, "objects",
                                              objdir_name, NULL);

      enumerator = g_file_enumerate_children (objdir, "standard::name,standard::type,unix::inode,unix::nlink", 
                                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                              cancellable, 
                                              error);
      if (!enumerator)
        goto out;
  
      while (TRUE)
        {
          GFileInfo *file_info;
          guint32 nlinks;

          if (!gs_file_enumerator_iterate (enumerator, &file_info, NULL,
                                           cancellable, error))
            goto out;
          if (file_info == NULL)
            break;
          
          nlinks = g_file_info_get_attribute_uint32 (file_info, "unix::nlink");
          if (nlinks == 1)
            {
              gs_unref_object GFile *objpath = NULL;
              objpath = g_file_get_child (objdir, g_file_info_get_name (file_info));
              if (!gs_file_unlink (objpath, cancellable, error))
                goto out;
            }
        }
    }

  ret = TRUE;
 out:
  return ret;
}
