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

typedef struct {
  OstreeRepo               *repo;
  OstreeRepoCheckoutMode    mode;
  OstreeRepoCheckoutOverwriteMode    overwrite_mode;
  GFile                    *destination;
  int                       dirfd;
  OstreeRepoFile           *source;
  GFileInfo                *source_info;
  GCancellable             *cancellable;

  gboolean                  caught_error;
  GError                   *error;

  GSimpleAsyncResult       *result;
} CheckoutOneFileAsyncData;

static void
checkout_file_async_data_free (gpointer      data)
{
  CheckoutOneFileAsyncData *checkout_data = data;

  g_clear_object (&checkout_data->repo);
  g_clear_object (&checkout_data->destination);
  g_clear_object (&checkout_data->source);
  g_clear_object (&checkout_data->source_info);
  g_clear_object (&checkout_data->cancellable);
  g_free (checkout_data);
}

static void
checkout_file_thread (GSimpleAsyncResult     *result,
                      GObject                *src,
                      GCancellable           *cancellable)
{
  const char *checksum;
  OstreeRepo *repo;
  gboolean is_symlink;
  gboolean hardlink_supported;
  GError *local_error = NULL;
  GError **error = &local_error;
  gs_unref_object GFile *loose_path = NULL;
  gs_unref_object GInputStream *input = NULL;
  gs_unref_variant GVariant *xattrs = NULL;
  CheckoutOneFileAsyncData *checkout_data;

  checkout_data = g_simple_async_result_get_op_res_gpointer (result);
  repo = checkout_data->repo;

  /* Hack to avoid trying to create device files as a user */
  if (checkout_data->mode == OSTREE_REPO_CHECKOUT_MODE_USER
      && g_file_info_get_file_type (checkout_data->source_info) == G_FILE_TYPE_SPECIAL)
    goto out;

  is_symlink = g_file_info_get_file_type (checkout_data->source_info) == G_FILE_TYPE_SYMBOLIC_LINK;

  checksum = ostree_repo_file_get_checksum ((OstreeRepoFile*)checkout_data->source);

  /* We can only do hardlinks in these scenarios */
  if (!is_symlink &&
      ((checkout_data->repo->mode == OSTREE_REPO_MODE_BARE && checkout_data->mode == OSTREE_REPO_CHECKOUT_MODE_NONE)
       || (checkout_data->repo->mode == OSTREE_REPO_MODE_ARCHIVE_Z2 && checkout_data->mode == OSTREE_REPO_CHECKOUT_MODE_USER)))
    {
      if (!find_loose_for_checkout (checkout_data->repo, checksum, &loose_path,
                                    cancellable, error))
        goto out;
    }
  /* Also, if we're archive-z and we didn't find an object, uncompress it now,
   * stick it in the cache, and then hardlink to that.
   */
  if (!is_symlink
      && loose_path == NULL
      && repo->mode == OSTREE_REPO_MODE_ARCHIVE_Z2
      && checkout_data->mode == OSTREE_REPO_CHECKOUT_MODE_USER
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
                                     checkout_data->source_info, xattrs, 
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
      if (!checkout_file_hardlink (checkout_data->repo, checkout_data->mode,
                                   checkout_data->overwrite_mode, loose_path,
                                   checkout_data->destination, checkout_data->dirfd,
                                   &hardlink_supported, cancellable, error))
        {
          g_prefix_error (error, "Hardlinking loose object %s to %s: ", checksum,
                          gs_file_get_path_cached (checkout_data->destination));
          goto out;
        }
    }

  /* Fall back to copy if there's no loose object, or we couldn't hardlink */
  if (loose_path == NULL || !hardlink_supported)
    {
      if (!ostree_repo_load_file (checkout_data->repo, checksum, &input, NULL, &xattrs,
                                  cancellable, error))
        goto out;

      if (!checkout_file_from_input (checkout_data->destination,
                                     checkout_data->mode,
                                     checkout_data->overwrite_mode,
                                     checkout_data->source_info, xattrs, 
                                     input, cancellable, error))
        {
          g_prefix_error (error, "Copying object %s to %s: ", checksum,
                          gs_file_get_path_cached (checkout_data->destination));
          goto out;
        }
    }

 out:
  if (local_error)
    g_simple_async_result_take_error (result, local_error);
}

static void
checkout_one_file_async (OstreeRepo                  *self,
                         OstreeRepoCheckoutMode    mode,
                         OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                         OstreeRepoFile           *source,
                         GFileInfo                *source_info,
                         GFile                    *destination,
                         int                       dirfd,
                         GCancellable             *cancellable,
                         GAsyncReadyCallback       callback,
                         gpointer                  user_data)
{
  CheckoutOneFileAsyncData *checkout_data;

  checkout_data = g_new0 (CheckoutOneFileAsyncData, 1);
  checkout_data->repo = g_object_ref (self);
  checkout_data->mode = mode;
  checkout_data->overwrite_mode = overwrite_mode;
  checkout_data->destination = g_object_ref (destination);
  checkout_data->dirfd = dirfd;
  checkout_data->source = g_object_ref (source);
  checkout_data->source_info = g_object_ref (source_info);
  checkout_data->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  checkout_data->result = g_simple_async_result_new ((GObject*) self,
                                                     callback, user_data,
                                                     checkout_one_file_async);

  g_simple_async_result_set_op_res_gpointer (checkout_data->result, checkout_data,
                                             checkout_file_async_data_free);

  g_simple_async_result_run_in_thread (checkout_data->result,
                                       checkout_file_thread, G_PRIORITY_DEFAULT,
                                       cancellable);
  g_object_unref (checkout_data->result);
}

static gboolean
checkout_one_file_finish (OstreeRepo               *self,
                          GAsyncResult             *result,
                          GError                  **error)
{
  GSimpleAsyncResult *simple;

  g_return_val_if_fail (g_simple_async_result_is_valid (result, (GObject*)self, checkout_one_file_async), FALSE);

  simple = G_SIMPLE_ASYNC_RESULT (result);
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;
  return TRUE;
}

typedef struct {
  OstreeRepo               *repo;
  OstreeRepoCheckoutMode    mode;
  OstreeRepoCheckoutOverwriteMode    overwrite_mode;
  GFile                    *destination;
  OstreeRepoFile           *source;
  GFileInfo                *source_info;
  GCancellable             *cancellable;

  gboolean                  caught_error;
  GError                   *error;

  DIR                      *dir_handle;

  gboolean                  dir_enumeration_complete;
  guint                     pending_ops;
  guint                     pending_file_ops;
  GPtrArray                *pending_dirs;
  GMainLoop                *loop;
  GSimpleAsyncResult       *result;
} CheckoutTreeAsyncData;

static void
checkout_tree_async_data_free (gpointer      data)
{
  CheckoutTreeAsyncData *checkout_data = data;

  g_clear_object (&checkout_data->repo);
  g_clear_object (&checkout_data->destination);
  g_clear_object (&checkout_data->source);
  g_clear_object (&checkout_data->source_info);
  g_clear_object (&checkout_data->cancellable);
  if (checkout_data->pending_dirs)
    g_ptr_array_unref (checkout_data->pending_dirs);
  if (checkout_data->dir_handle)
    (void) closedir (checkout_data->dir_handle);
  g_free (checkout_data);
}

static void
on_tree_async_child_op_complete (CheckoutTreeAsyncData   *data,
                                 GError                  *local_error)
{
  data->pending_ops--;

  if (local_error)
    {
      if (!data->caught_error)
        {
          data->caught_error = TRUE;
          g_propagate_error (&data->error, local_error);
        }
      else
        g_clear_error (&local_error);
    }

  if (data->pending_ops != 0)
    return;

  if (data->caught_error)
    g_simple_async_result_take_error (data->result, data->error);
  g_simple_async_result_complete_in_idle (data->result);
  g_object_unref (data->result);
}

static void
on_one_subdir_checked_out (GObject          *src,
                           GAsyncResult     *result,
                           gpointer          user_data)
{
  CheckoutTreeAsyncData *data = user_data;
  GError *local_error = NULL;

  if (!ostree_repo_checkout_tree_finish ((OstreeRepo*) src, result, &local_error))
    goto out;

 out:
  on_tree_async_child_op_complete (data, local_error);
}

static void
process_pending_dirs (CheckoutTreeAsyncData *data)
{
  guint i;

  g_assert (data->dir_enumeration_complete);
  g_assert (data->pending_file_ops == 0);

  /* Don't hold a FD open while we're processing
   * recursive calls, otherwise we can pretty easily
   * hit the max of 1024 fds =(
   */
  if (data->dir_handle)
    {
      (void) closedir (data->dir_handle);
      data->dir_handle = NULL;
    }

  if (data->pending_dirs != NULL)
    {
      for (i = 0; i < data->pending_dirs->len; i++)
        {
          GFileInfo *file_info = data->pending_dirs->pdata[i];
          const char *name;
          gs_unref_object GFile *dest_path = NULL;
          gs_unref_object GFile *src_child = NULL;

          name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 

          dest_path = g_file_get_child (data->destination, name);
          src_child = g_file_get_child ((GFile*)data->source, name);

          ostree_repo_checkout_tree_async (data->repo,
                                           data->mode,
                                           data->overwrite_mode,
                                           dest_path, (OstreeRepoFile*)src_child, file_info,
                                           data->cancellable,
                                           on_one_subdir_checked_out,
                                           data);
          data->pending_ops++;
        }
      g_ptr_array_set_size (data->pending_dirs, 0);
      on_tree_async_child_op_complete (data, NULL);
    }
}

static void
on_one_file_checked_out (GObject          *src,
                         GAsyncResult     *result,
                         gpointer          user_data)
{
  CheckoutTreeAsyncData *data = user_data;
  GError *local_error = NULL;

  if (!checkout_one_file_finish ((OstreeRepo*) src, result, &local_error))
    goto out;

 out:
  data->pending_file_ops--;
  if (data->dir_enumeration_complete && data->pending_file_ops == 0)
    process_pending_dirs (data);
  on_tree_async_child_op_complete (data, local_error);
}

static void
on_got_next_files (GObject          *src,
                   GAsyncResult     *result,
                   gpointer          user_data)
{
  CheckoutTreeAsyncData *data = user_data;
  GError *local_error = NULL;
  GList *files = NULL;
  GList *iter = NULL;

  files = g_file_enumerator_next_files_finish ((GFileEnumerator*) src, result, &local_error);
  if (local_error)
    goto out;

  if (!files)
    data->dir_enumeration_complete = TRUE;
  else
    {
      g_file_enumerator_next_files_async ((GFileEnumerator*)src, 50, G_PRIORITY_DEFAULT,
                                          data->cancellable,
                                          on_got_next_files, data);
      data->pending_ops++;
    }

  if (data->dir_enumeration_complete && data->pending_file_ops == 0)
    process_pending_dirs (data);

  for (iter = files; iter; iter = iter->next)
    {
      GFileInfo *file_info = iter->data;
      const char *name;
      guint32 type;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

      if (type != G_FILE_TYPE_DIRECTORY)
        {
          gs_unref_object GFile *dest_path = NULL;
          gs_unref_object GFile *src_child = NULL;

          dest_path = g_file_get_child (data->destination, name);
          src_child = g_file_get_child ((GFile*)data->source, name);

          checkout_one_file_async (data->repo, data->mode,
                                   data->overwrite_mode,
                                   (OstreeRepoFile*)src_child, file_info, 
                                   dest_path, dirfd(data->dir_handle),
                                   data->cancellable, on_one_file_checked_out,
                                   data);
          data->pending_file_ops++;
          data->pending_ops++;
        }
      else
        {
          if (data->pending_dirs == NULL)
            {
              data->pending_dirs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
              data->pending_ops++;
            }
          g_ptr_array_add (data->pending_dirs, g_object_ref (file_info));
        }
      g_object_unref (file_info);
    }

  g_list_free (files);

 out:
  on_tree_async_child_op_complete (data, local_error);
}

void
ostree_repo_checkout_tree_async (OstreeRepo               *self,
                                 OstreeRepoCheckoutMode    mode,
                                 OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                                 GFile                    *destination,
                                 OstreeRepoFile           *source,
                                 GFileInfo                *source_info,
                                 GCancellable             *cancellable,
                                 GAsyncReadyCallback       callback,
                                 gpointer                  user_data)
{
  CheckoutTreeAsyncData *checkout_data;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_variant GVariant *xattrs = NULL;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;

  checkout_data = g_new0 (CheckoutTreeAsyncData, 1);
  checkout_data->repo = g_object_ref (self);
  checkout_data->mode = mode;
  checkout_data->overwrite_mode = overwrite_mode;
  checkout_data->destination = g_object_ref (destination);
  checkout_data->source = g_object_ref (source);
  checkout_data->source_info = g_object_ref (source_info);
  checkout_data->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  checkout_data->pending_ops++; /* Count this function */

  checkout_data->result = g_simple_async_result_new ((GObject*) self,
                                                     callback, user_data,
                                                     ostree_repo_checkout_tree_async);

  g_simple_async_result_set_op_res_gpointer (checkout_data->result, checkout_data,
                                             checkout_tree_async_data_free);

  if (!ostree_repo_file_get_xattrs (checkout_data->source, &xattrs, NULL, error))
    goto out;

  if (!checkout_file_from_input (checkout_data->destination,
                                 checkout_data->mode,
                                 checkout_data->overwrite_mode,
                                 checkout_data->source_info,
                                 xattrs, NULL,
                                 cancellable, error))
    goto out;

  checkout_data->dir_handle = opendir (gs_file_get_path_cached (checkout_data->destination));
  if (!checkout_data->dir_handle)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  g_clear_pointer (&xattrs, (GDestroyNotify) g_variant_unref);

  dir_enum = g_file_enumerate_children ((GFile*)checkout_data->source,
                                        OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, 
                                        error);
  if (!dir_enum)
    goto out;

  g_file_enumerator_next_files_async (dir_enum, 50, G_PRIORITY_DEFAULT, cancellable,
                                      on_got_next_files, checkout_data);
  checkout_data->pending_ops++;

 out:
  on_tree_async_child_op_complete (checkout_data, local_error);
}

gboolean
ostree_repo_checkout_tree_finish (OstreeRepo               *self,
                                  GAsyncResult             *result,
                                  GError                  **error)
{
  GSimpleAsyncResult *simple;

  g_return_val_if_fail (g_simple_async_result_is_valid (result, (GObject*)self, ostree_repo_checkout_tree_async), FALSE);

  simple = G_SIMPLE_ASYNC_RESULT (result);
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;
  return TRUE;
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
