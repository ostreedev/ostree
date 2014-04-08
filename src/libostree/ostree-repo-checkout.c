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
#include <attr/xattr.h>
#include <gio/gfiledescriptorbased.h>
#include <gio/gunixoutputstream.h>
#include "otutil.h"

#include "ostree-repo-file.h"
#include "ostree-core-private.h"
#include "ostree-repo-private.h"

static gboolean
checkout_object_for_uncompressed_cache (OstreeRepo      *self,
                                        const char      *loose_path,
                                        GFileInfo       *src_info,
                                        GInputStream    *content,
                                        GCancellable    *cancellable,
                                        GError         **error)
{
  gboolean ret = FALSE;
  gs_free char *temp_filename = NULL;
  gs_unref_object GOutputStream *temp_out = NULL;
  int fd;
  int res;
  guint32 file_mode;

  /* Don't make setuid files in uncompressed cache */
  file_mode = g_file_info_get_attribute_uint32 (src_info, "unix::mode");
  file_mode &= ~(S_ISUID|S_ISGID);

  if (!gs_file_open_in_tmpdir_at (self->tmp_dir_fd, file_mode,
                                  &temp_filename, &temp_out,
                                  cancellable, error))
    goto out;

  if (g_output_stream_splice (temp_out, content, 0, cancellable, error) < 0)
    goto out;

  if (!g_output_stream_flush (temp_out, cancellable, error))
    goto out;

  fd = g_file_descriptor_based_get_fd ((GFileDescriptorBased*)temp_out);

  do
    res = fsync (fd);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (G_UNLIKELY (res == -1))
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  if (!g_output_stream_close (temp_out, cancellable, error))
    goto out;

  if (!_ostree_repo_ensure_loose_objdir_at (self->uncompressed_objects_dir_fd,
                                            loose_path,
                                            cancellable, error))
    goto out;

  if (G_UNLIKELY (renameat (self->tmp_dir_fd, temp_filename,
                            self->uncompressed_objects_dir_fd, loose_path) == -1))
    {
      if (errno != EEXIST)
        {
          ot_util_set_error_from_errno (error, errno);
          g_prefix_error (error, "Storing file '%s': ", temp_filename);
          goto out;
        }
      else
        (void) unlinkat (self->tmp_dir_fd, temp_filename, 0);
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
write_regular_file_content (OstreeRepoCheckoutMode mode,
                            GOutputStream         *output,
                            GFileInfo             *file_info,
                            GVariant              *xattrs,
                            GInputStream          *input,
                            GCancellable          *cancellable,
                            GError               **error)
{
  gboolean ret = FALSE;
  int fd;
  int res;

  if (g_output_stream_splice (output, input, 0,
                              cancellable, error) < 0)
    goto out;

  if (!g_output_stream_flush (output, cancellable, error))
    goto out;

  fd = g_file_descriptor_based_get_fd ((GFileDescriptorBased*)output);

  if (mode != OSTREE_REPO_CHECKOUT_MODE_USER)
    {
      do
        res = fchown (fd,
                      g_file_info_get_attribute_uint32 (file_info, "unix::uid"),
                      g_file_info_get_attribute_uint32 (file_info, "unix::gid"));
      while (G_UNLIKELY (res == -1 && errno == EINTR));
      if (G_UNLIKELY (res == -1))
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }

      do
        res = fchmod (fd, g_file_info_get_attribute_uint32 (file_info, "unix::mode"));
      while (G_UNLIKELY (res == -1 && errno == EINTR));
      if (G_UNLIKELY (res == -1))
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
              
      if (xattrs)
        {
          if (!gs_fd_set_all_xattrs (fd, xattrs, cancellable, error))
            goto out;
        }
    }
          
  if (fsync (fd) == -1)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }
          
  if (!g_output_stream_close (output, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
checkout_file_from_input_at (OstreeRepoCheckoutMode mode,
                             GFileInfo      *file_info,
                             GVariant       *xattrs,
                             GInputStream   *input,
                             int             destination_dfd,
                             GFile          *destination_parent,
                             const char     *destination_name,
                             GCancellable   *cancellable,
                             GError        **error)
{
  gboolean ret = FALSE;
  int res;

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      do
        res = symlinkat (g_file_info_get_symlink_target (file_info),
                         destination_dfd, destination_name);
      while (G_UNLIKELY (res == -1 && errno == EINTR));
      if (res == -1)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
          
      if (xattrs)
        {
          if (!gs_dfd_and_name_set_all_xattrs (destination_dfd, destination_name,
                                               xattrs, cancellable, error))
            goto out;
        }
    }
  else if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
      gs_unref_object GOutputStream *temp_out = NULL;
      int fd;
      guint32 file_mode;

      file_mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
      /* Don't make setuid files on checkout when we're doing --user */
      if (mode == OSTREE_REPO_CHECKOUT_MODE_USER)
        file_mode &= ~(S_ISUID|S_ISGID);

      do
        fd = openat (destination_dfd, destination_name, O_WRONLY | O_CREAT | O_EXCL, file_mode);
      while (G_UNLIKELY (fd == -1 && errno == EINTR));
      if (fd == -1)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
      temp_out = g_unix_output_stream_new (fd, TRUE);
      fd = -1; /* Transfer ownership */

      if (!write_regular_file_content (mode, temp_out, file_info, xattrs, input,
                                       cancellable, error))
        goto out;
    }
  else
    g_assert_not_reached ();
  
  ret = TRUE;
 out:
  return ret;
}

/*
 * This function creates a file under a temporary name, then rename()s
 * it into place.  This implements union-like behavior.
 */
static gboolean
checkout_file_unioning_from_input_at (OstreeRepoCheckoutMode mode,
                                      GFileInfo      *file_info,
                                      GVariant       *xattrs,
                                      GInputStream   *input,
                                      int             destination_dfd,
                                      GFile          *destination_parent,
                                      const char     *destination_name,
                                      GCancellable   *cancellable,
                                      GError        **error)
{
  gboolean ret = FALSE;
  gs_free char *temp_filename = NULL;

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      if (!_ostree_make_temporary_symlink_at (destination_dfd,
                                              g_file_info_get_symlink_target (file_info),
                                              &temp_filename,
                                              cancellable, error))
        goto out;
          
      if (xattrs)
        {
          if (!gs_dfd_and_name_set_all_xattrs (destination_dfd, destination_name,
                                               xattrs, cancellable, error))
            goto out;
        }
    }
  else if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
      gs_unref_object GOutputStream *temp_out = NULL;
      guint32 file_mode;

      file_mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
      /* Don't make setuid files on checkout when we're doing --user */
      if (mode == OSTREE_REPO_CHECKOUT_MODE_USER)
        file_mode &= ~(S_ISUID|S_ISGID);

      if (!gs_file_open_in_tmpdir_at (destination_dfd, file_mode,
                                      &temp_filename, &temp_out,
                                      cancellable, error))
        goto out;

      if (!write_regular_file_content (mode, temp_out, file_info, xattrs, input,
                                       cancellable, error))
        goto out;
    }
  else
    g_assert_not_reached ();

  if (G_UNLIKELY (renameat (destination_dfd, temp_filename,
                            destination_dfd, destination_name) == -1))
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
checkout_file_hardlink (OstreeRepo                          *self,
                        OstreeRepoCheckoutMode               mode,
                        OstreeRepoCheckoutOverwriteMode      overwrite_mode,
                        const char                          *loose_path,
                        int                                  destination_dfd,
                        const char                          *destination_name,
                        gboolean                             allow_noent,
                        gboolean                            *out_was_supported,
                        GCancellable                        *cancellable,
                        GError                             **error)
{
  gboolean ret = FALSE;
  gboolean ret_was_supported = FALSE;
  int srcfd = self->mode == OSTREE_REPO_MODE_BARE ?
    self->objects_dir_fd : self->uncompressed_objects_dir_fd;

 again:
  if (linkat (srcfd, loose_path, destination_dfd, destination_name, 0) != -1)
    ret_was_supported = TRUE;
  else if (errno == EMLINK || errno == EXDEV || errno == EPERM)
    {
      /* EMLINK, EXDEV and EPERM shouldn't be fatal; we just can't do the
       * optimization of hardlinking instead of copying.
       */
      ret_was_supported = FALSE;
    }
  else if (allow_noent && errno == ENOENT)
    {
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
      (void) unlinkat (destination_dfd, destination_name, 0);
      goto again;
    }
  else
    {
      g_prefix_error (error, "Hardlinking %s to %s: ", loose_path, destination_name);
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
checkout_one_file_at (OstreeRepo                        *repo,
                      GFile                             *source,
                      GFileInfo                         *source_info,
                      int                                destination_dfd,
                      GFile                             *destination_parent,
                      const char                        *destination_name,
                      OstreeRepoCheckoutMode             mode,
                      OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                      GCancellable                      *cancellable,
                      GError                           **error)
{
  gboolean ret = FALSE;
  const char *checksum;
  gboolean is_symlink;
  gboolean did_hardlink = FALSE;
  char loose_path_buf[_OSTREE_LOOSE_PATH_MAX];
  gs_unref_object GInputStream *input = NULL;
  gs_unref_variant GVariant *xattrs = NULL;

  is_symlink = g_file_info_get_file_type (source_info) == G_FILE_TYPE_SYMBOLIC_LINK;

  checksum = ostree_repo_file_get_checksum ((OstreeRepoFile*)source);

  /* Try to do a hardlink first, if it's a regular file.  This also
   * traverses all parent repos.
   */
  if (!is_symlink)
    {
      OstreeRepo *current_repo = repo;

      while (current_repo)
        {
          gboolean is_bare = (current_repo->mode == OSTREE_REPO_MODE_BARE
                              && mode == OSTREE_REPO_CHECKOUT_MODE_NONE);
          gboolean is_archive_z2_with_cache = (current_repo->mode == OSTREE_REPO_MODE_ARCHIVE_Z2
                                               && mode == OSTREE_REPO_CHECKOUT_MODE_USER);

          /* But only under these conditions */
          if (is_bare || is_archive_z2_with_cache)
            {
              /* Override repo mode; for archive-z2 we're looking in
                 the cache, which is in "bare" form */
              _ostree_loose_path (loose_path_buf, checksum, OSTREE_OBJECT_TYPE_FILE, OSTREE_REPO_MODE_BARE);
              if (!checkout_file_hardlink (current_repo,
                                           mode, overwrite_mode, loose_path_buf,
                                           destination_dfd, destination_name,
                                           TRUE, &did_hardlink,
                                           cancellable, error))
                goto out;
              if (did_hardlink)
                break;
            }
          current_repo = current_repo->parent_repo;
        }
    }

  /* Ok, if we're archive-z2 and we didn't find an object, uncompress
   * it now, stick it in the cache, and then hardlink to that.
   */
  if (!is_symlink
      && !did_hardlink
      && repo->mode == OSTREE_REPO_MODE_ARCHIVE_Z2
      && mode == OSTREE_REPO_CHECKOUT_MODE_USER
      && repo->enable_uncompressed_cache)
    {
      if (!ostree_repo_load_file (repo, checksum, &input, NULL, NULL,
                                  cancellable, error))
        goto out;

      /* Overwrite any parent repo from earlier */
      _ostree_loose_path (loose_path_buf, checksum, OSTREE_OBJECT_TYPE_FILE, OSTREE_REPO_MODE_BARE);

      if (!checkout_object_for_uncompressed_cache (repo, loose_path_buf,
                                                   source_info, input,
                                                   cancellable, error))
        {
          g_prefix_error (error, "Unpacking loose object %s: ", checksum);
          goto out;
        }
      
      g_clear_object (&input);

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

      if (!checkout_file_hardlink (repo, mode, overwrite_mode, loose_path_buf,
                                   destination_dfd, destination_name,
                                   FALSE, &did_hardlink,
                                   cancellable, error))
        {
          g_prefix_error (error, "Using new cached uncompressed hardlink of %s to %s: ", checksum, destination_name);
          goto out;
        }
    }

  /* Fall back to copy if we couldn't hardlink */
  if (!did_hardlink)
    {
      if (!ostree_repo_load_file (repo, checksum, &input, NULL, &xattrs,
                                  cancellable, error))
        goto out;

      if (overwrite_mode == OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES)
        {
          if (!checkout_file_unioning_from_input_at (mode, source_info, xattrs, input,
                                                     destination_dfd, destination_parent,
                                                     destination_name,
                                                     cancellable, error)) 
            {
              g_prefix_error (error, "Union checkout of %s to %s: ", checksum, destination_name);
              goto out;
            }
        }
      else
        {
          if (!checkout_file_from_input_at (mode, source_info, xattrs, input,
                                            destination_dfd, destination_parent,
                                            destination_name,
                                            cancellable, error))
            {
              g_prefix_error (error, "Checkout of %s to %s: ", checksum, destination_name);
              goto out;
            }
        }

      if (input)
        {
          if (!g_input_stream_close (input, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

/*
 * checkout_tree_at:
 * @self: Repo
 * @mode: Options controlling all files
 * @overwrite_mode: Whether or not to overwrite files
 * @destination_parent_fd: Place tree here
 * @destination_name: Use this name for tree
 * @source: Source tree
 * @source_info: Source info
 * @cancellable: Cancellable
 * @error: Error
 *
 * Like ostree_repo_checkout_tree(), but check out @source into the
 * relative @destination_name, located by @destination_parent_fd.
 */
static gboolean
checkout_tree_at (OstreeRepo                        *self,
                  OstreeRepoCheckoutMode             mode,
                  OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                  int                                destination_parent_fd,
                  const char                        *destination_name,
                  GFile                             *destination,
                  OstreeRepoFile                    *source,
                  GFileInfo                         *source_info,
                  GCancellable                      *cancellable,
                  GError                           **error)
{
  gboolean ret = FALSE;
  gboolean did_exist = FALSE;
  int destination_dfd = -1;
  int res;
  gs_unref_variant GVariant *xattrs = NULL;
  gs_unref_object GFileEnumerator *dir_enum = NULL;

  /* Create initially with mode 0700, then chown/chmod only when we're
   * done.  This avoids anyone else being able to operate on partially
   * constructed dirs.
   */
  do
    res = mkdirat (destination_parent_fd, destination_name, 0700);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      if (errno == EEXIST && overwrite_mode == OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES)
        did_exist = TRUE;
      else
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  if (!gs_file_open_dir_fd_at (destination_parent_fd, destination_name,
                               &destination_dfd,
                               cancellable, error))
    goto out;

  /* Set the xattrs now, so any derived labeling works */
  if (!did_exist && mode != OSTREE_REPO_CHECKOUT_MODE_USER)
    {
      if (!ostree_repo_file_get_xattrs (source, &xattrs, NULL, error))
        goto out;

      if (xattrs)
        {
          if (!gs_fd_set_all_xattrs (destination_dfd, xattrs, cancellable, error))
            goto out;
        }
    }

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

      if (!gs_file_enumerator_iterate (dir_enum, &file_info, &src_child,
                                       cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      name = g_file_info_get_name (file_info);

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          gs_unref_object GFile *child_destination = g_file_get_child (destination, name);
          if (!checkout_tree_at (self, mode, overwrite_mode,
                                 destination_dfd, name, child_destination,
                                 (OstreeRepoFile*)src_child, file_info,
                                 cancellable, error))
            goto out;
        }
      else
        {
          if (!checkout_one_file_at (self, src_child, file_info,
                                     destination_dfd, destination, name,
                                     mode, overwrite_mode,
                                     cancellable, error))
            goto out;
        }
    }

  /* We do fchmod/fchown last so that no one else could access the
   * partially created directory and change content we're laying out.
   */
  if (!did_exist)
    {
      do
        res = fchmod (destination_dfd,
                      g_file_info_get_attribute_uint32 (source_info, "unix::mode"));
      while (G_UNLIKELY (res == -1 && errno == EINTR));
      if (G_UNLIKELY (res == -1))
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  if (!did_exist && mode != OSTREE_REPO_CHECKOUT_MODE_USER)
    {
      do
        res = fchown (destination_dfd,
                      g_file_info_get_attribute_uint32 (source_info, "unix::uid"),
                      g_file_info_get_attribute_uint32 (source_info, "unix::gid"));
      while (G_UNLIKELY (res == -1 && errno == EINTR));
      if (G_UNLIKELY (res == -1))
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  /* Finally, fsync to ensure all entries are on disk.  Ultimately
   * this should be configurable for the case where we're constructing
   * buildroots.
   */
  if (fsync (destination_dfd) == -1)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  ret = TRUE;
 out:
  if (destination_dfd != -1)
    (void) close (destination_dfd);
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
  return checkout_tree_at (self, mode, overwrite_mode,
                           AT_FDCWD,
                           gs_file_get_path_cached (destination),
                           destination,
                           source, source_info,
                           cancellable, error);
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
      objdir = g_file_get_child (self->uncompressed_objects_dir, objdir_name);

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
