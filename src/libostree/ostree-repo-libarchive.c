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

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-file.h"
#include "ostree-mutable-tree.h"

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#include "ostree-libarchive-input-stream.h"
#endif

#include "otutil.h"

#ifdef HAVE_LIBARCHIVE

static void
propagate_libarchive_error (GError      **error,
                            struct archive *a)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s", archive_error_string (a));
}

static GFileInfo *
file_info_from_archive_entry_and_modifier (OstreeRepo *repo,
                                           struct archive_entry *entry,
                                           OstreeRepoCommitModifier *modifier)
{
  g_autoptr(GFileInfo) info = NULL;
  GFileInfo *modified_info = NULL;
  const struct stat *st;
  guint32 file_type;

  st = archive_entry_stat (entry);

  info = _ostree_header_gfile_info_new (st->st_mode, st->st_uid, st->st_gid);
  file_type = ot_gfile_type_for_mode (st->st_mode);

  if (file_type == G_FILE_TYPE_REGULAR)
    {
      g_file_info_set_attribute_uint64 (info, "standard::size", st->st_size);
    }
  else if (file_type == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      g_file_info_set_attribute_byte_string (info, "standard::symlink-target", archive_entry_symlink (entry));
    }

  _ostree_repo_commit_modifier_apply (repo, modifier,
                                      archive_entry_pathname (entry),
                                      info, &modified_info);

  return modified_info;
}

static gboolean
import_libarchive_entry_file (OstreeRepo           *self,
                              OstreeRepoImportArchiveOptions  *opts,
                              struct archive       *a,
                              struct archive_entry *entry,
                              GFileInfo            *file_info,
                              guchar              **out_csum,
                              GCancellable         *cancellable,
                              GError              **error)
{
  gboolean ret = FALSE;
  g_autoptr(GInputStream) file_object_input = NULL;
  g_autoptr(GInputStream) archive_stream = NULL;
  guint64 length;
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  switch (g_file_info_get_file_type (file_info))
    {
    case G_FILE_TYPE_REGULAR:
      archive_stream = _ostree_libarchive_input_stream_new (a);
      break;
    case G_FILE_TYPE_SYMBOLIC_LINK:
      break;
    default:
      if (opts->ignore_unsupported_content)
        {
          ret = TRUE;
          goto out;
        }
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unable to import non-regular/non-symlink file '%s'",
                       archive_entry_pathname (entry));
          goto out;
        }
    }
  
  if (!ostree_raw_file_to_content_stream (archive_stream, file_info, NULL,
                                          &file_object_input, &length, cancellable, error))
    goto out;
  
  if (!ostree_repo_write_content (self, NULL, file_object_input, length, out_csum,
                                  cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
write_libarchive_entry_to_mtree (OstreeRepo           *self,
                                 OstreeRepoImportArchiveOptions  *opts,
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
  g_autoptr(GFileInfo) file_info = NULL;
  g_autoptr(GPtrArray) split_path = NULL;
  g_autoptr(GPtrArray) hardlink_split_path = NULL;
  glnx_unref_object OstreeMutableTree *subdir = NULL;
  glnx_unref_object OstreeMutableTree *parent = NULL;
  glnx_unref_object OstreeMutableTree *hardlink_source_parent = NULL;
  g_autofree char *hardlink_source_checksum = NULL;
  glnx_unref_object OstreeMutableTree *hardlink_source_subdir = NULL;
  g_autofree guchar *tmp_csum = NULL;
  g_autofree char *tmp_checksum = NULL;

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
      file_info = file_info_from_archive_entry_and_modifier (self, entry, modifier);

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_UNKNOWN)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unsupported file for import: %s", pathname);
          goto out;
        }

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {

          if (!_ostree_repo_write_directory_meta (self, file_info, NULL, &tmp_csum, cancellable, error))
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

          if (!import_libarchive_entry_file (self, opts, a, entry, file_info, &tmp_csum,
                                             cancellable, error))
            goto out;

          if (tmp_csum)
            { 
              g_free (tmp_checksum);
              tmp_checksum = ostree_checksum_from_bytes (tmp_csum);
              if (!ostree_mutable_tree_replace_file (parent, basename,
                                                     tmp_checksum,
                                                     error))
                goto out;
            }
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
create_empty_dir_with_uidgid (OstreeRepo   *self,
                              guint32       uid,
                              guint32       gid,
                              guint8      **out_csum,
                              GCancellable *cancellable,
                              GError      **error)
{
  g_autoptr(GFileInfo) tmp_dir_info = g_file_info_new ();
          
  g_file_info_set_attribute_uint32 (tmp_dir_info, "unix::uid", uid);
  g_file_info_set_attribute_uint32 (tmp_dir_info, "unix::gid", gid);
  g_file_info_set_attribute_uint32 (tmp_dir_info, "unix::mode", 0755 | S_IFDIR);
  
  return _ostree_repo_write_directory_meta (self, tmp_dir_info, NULL, out_csum, cancellable, error);
}
#endif

/**
 * ostree_repo_import_archive_to_mtree:
 * @self: An #OstreeRepo
 * @opts: Options structure, ensure this is zeroed, then set specific variables
 * @archive: Really this is "struct archive*"
 * @mtree: The #OstreeMutableTree to write to
 * @modifier: (allow-none): Optional commit modifier
 * @cancellable: Cancellable
 * @error: Error
 *
 * Import an archive file @archive into the repository, and write its
 * file structure to @mtree.
 */
gboolean
ostree_repo_import_archive_to_mtree (OstreeRepo                   *self,
                                     OstreeRepoImportArchiveOptions  *opts,
                                     void                         *archive,
                                     OstreeMutableTree            *mtree,
                                     OstreeRepoCommitModifier     *modifier,
                                     GCancellable                 *cancellable,
                                     GError                      **error)
{
#ifdef HAVE_LIBARCHIVE
  gboolean ret = FALSE;
  struct archive *a = archive;
  struct archive_entry *entry;
  g_autofree guchar *tmp_csum = NULL;
  int r;


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

      /* TODO - refactor this to only create the metadata on demand
       * (i.e. if there is a missing parent dir)
       */
      if (opts->autocreate_parents && !tmp_csum)
        {
          /* Here, we auto-pick the first uid/gid we find in the
           * archive.  Realistically this is probably always going to
           * be root, but eh, at least we try to match.
           */
          if (!create_empty_dir_with_uidgid (self, archive_entry_uid (entry),
                                             archive_entry_gid (entry),
                                             &tmp_csum, cancellable, error))
            goto out;
        }

      if (!write_libarchive_entry_to_mtree (self, opts, mtree, a,
                                            entry, modifier, tmp_csum,
                                            cancellable, error))
        goto out;
    }

  /* If we didn't import anything at all, and autocreation of parents
   * is enabled, automatically create a root directory.  This is
   * useful primarily when importing Docker image layers, which can
   * just be metadata.
   */
  if (!ostree_mutable_tree_get_metadata_checksum (mtree) && opts->autocreate_parents)
    {
      char tmp_checksum[65];

      if (!tmp_csum)
        {
          /* We didn't have any archive entries to match, so pick uid 0, gid 0. */
          if (!create_empty_dir_with_uidgid (self, 0, 0, &tmp_csum, cancellable, error))
            goto out;
        }
      
      ostree_checksum_inplace_from_bytes (tmp_csum, tmp_checksum);
      ostree_mutable_tree_set_metadata_checksum (mtree, tmp_checksum);
    }

  ret = TRUE;
 out:
  return ret;
#else
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This version of ostree is not compiled with libarchive support");
  return FALSE;
#endif
}
                          
/**
 * ostree_repo_write_archive_to_mtree:
 * @self: An #OstreeRepo
 * @archive: A path to an archive file
 * @mtree: The #OstreeMutableTree to write to
 * @modifier: (allow-none): Optional commit modifier
 * @autocreate_parents: Autocreate parent directories
 * @cancellable: Cancellable
 * @error: Error
 *
 * Import an archive file @archive into the repository, and write its
 * file structure to @mtree.
 */
gboolean
ostree_repo_write_archive_to_mtree (OstreeRepo                *self,
                                    GFile                     *archive,
                                    OstreeMutableTree         *mtree,
                                    OstreeRepoCommitModifier  *modifier,
                                    gboolean                   autocreate_parents,
                                    GCancellable             *cancellable,
                                    GError                  **error)
{
#ifdef HAVE_LIBARCHIVE
  gboolean ret = FALSE;
  struct archive *a = NULL;
  g_autoptr(GFileInfo) tmp_dir_info = NULL;
  OstreeRepoImportArchiveOptions opts = { 0, };

  a = archive_read_new ();
#ifdef HAVE_ARCHIVE_READ_SUPPORT_FILTER_ALL
  archive_read_support_filter_all (a);
#else
  archive_read_support_compression_all (a);
#endif
  archive_read_support_format_all (a);
  if (archive_read_open_filename (a, gs_file_get_path_cached (archive), 8192) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }

  opts.autocreate_parents = !!autocreate_parents;

  if (!ostree_repo_import_archive_to_mtree (self, &opts, a, mtree, modifier, cancellable, error))
    goto out;

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

#ifdef HAVE_LIBARCHIVE

static gboolean
file_to_archive_entry_common (GFile         *root,
                              OstreeRepoExportArchiveOptions *opts,
                              GFile         *path,
                              GFileInfo  *file_info,
                              struct archive_entry *entry,
                              GError            **error)
{
  gboolean ret = FALSE;
  g_autofree char *pathstr = g_file_get_relative_path (root, path);
  g_autoptr(GVariant) xattrs = NULL;
  time_t ts = (time_t) opts->timestamp_secs;

  if (pathstr && !pathstr[0])
    {
      g_free (pathstr);
      pathstr = g_strdup (".");
    }

  archive_entry_update_pathname_utf8 (entry, pathstr);
  archive_entry_set_ctime (entry, ts, 0);
  archive_entry_set_mtime (entry, ts, 0);
  archive_entry_set_atime (entry, ts, 0);
  archive_entry_set_uid (entry, g_file_info_get_attribute_uint32 (file_info, "unix::uid"));
  archive_entry_set_gid (entry, g_file_info_get_attribute_uint32 (file_info, "unix::gid"));
  archive_entry_set_mode (entry, g_file_info_get_attribute_uint32 (file_info, "unix::mode"));

  if (!ostree_repo_file_get_xattrs ((OstreeRepoFile*)path, &xattrs, NULL, error))
    goto out;

  if (!opts->disable_xattrs)
    {
      int i, n;
      
      n = g_variant_n_children (xattrs);
      for (i = 0; i < n; i++)
        {
          const guint8* name;
          g_autoptr(GVariant) value = NULL;
          const guint8* value_data;
          gsize value_len;

          g_variant_get_child (xattrs, i, "(^&ay@ay)", &name, &value);
          value_data = g_variant_get_fixed_array (value, &value_len, 1);

          archive_entry_xattr_add_entry (entry, (char*)name,
                                         (char*) value_data, value_len);
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
write_header_free_entry (struct archive *a,
                         struct archive_entry **entryp,
                         GError **error)
{
  struct archive_entry *entry = *entryp;
  gboolean ret = FALSE;

  if (archive_write_header (a, entry) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      goto out;
    }

  ret = TRUE;
 out:
  archive_entry_free (entry);
  *entryp = NULL;
  return ret;
}

static gboolean
write_directory_to_libarchive_recurse (OstreeRepo               *self,
                                       OstreeRepoExportArchiveOptions *opts,
                                       GFile                    *root,
                                       GFile                    *dir,
                                       struct archive           *a,
                                       GCancellable             *cancellable,
                                       GError                  **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFileInfo) dir_info = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  struct archive_entry *entry = NULL;

  dir_info = g_file_query_info (dir, OSTREE_GIO_FAST_QUERYINFO,
                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                cancellable, error);
  if (!dir_info)
    goto out;

  entry = archive_entry_new2 (a);
  if (!file_to_archive_entry_common (root, opts, dir, dir_info, entry, error))
    goto out;
  if (!write_header_free_entry (a, &entry, error))
    goto out;

  dir_enum = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO, 
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, error);
  if (!dir_enum)
    goto out;

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *path;

      if (!gs_file_enumerator_iterate (dir_enum, &file_info, &path,
                                       cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      /* First, handle directories recursively */
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!write_directory_to_libarchive_recurse (self, opts, root, path, a,
                                                      cancellable, error))
            goto out;

          /* Go to the next entry */
          continue;
        }

      /* Past here, should be a regular file or a symlink */

      entry = archive_entry_new2 (a);
      if (!file_to_archive_entry_common (root, opts, path, file_info, entry, error))
        goto out;

      switch (g_file_info_get_file_type (file_info))
        {
        case G_FILE_TYPE_SYMBOLIC_LINK:
          {
            archive_entry_set_symlink (entry, g_file_info_get_symlink_target (file_info));
            if (!write_header_free_entry (a, &entry, error))
              goto out;
          }
          break;
        case G_FILE_TYPE_REGULAR:
          {
            guint8 buf[8192];
            g_autoptr(GInputStream) file_in = NULL;
            g_autoptr(GFileInfo) file_info = NULL;
            const char *checksum;

            checksum = ostree_repo_file_get_checksum ((OstreeRepoFile*)path);

            if (!ostree_repo_load_file (self, checksum, &file_in, &file_info, NULL,
                                        cancellable, error))
              goto out;

            archive_entry_set_size (entry, g_file_info_get_size (file_info));

            if (archive_write_header (a, entry) != ARCHIVE_OK)
              {
                propagate_libarchive_error (error, a);
                goto out;
              }

            while (TRUE)
              {
                gssize bytes_read = g_input_stream_read (file_in, buf, sizeof (buf),
                                                         cancellable, error);
                if (bytes_read < 0)
                  goto out;
                if (bytes_read == 0)
                  break;

                { ssize_t r = archive_write_data (a, buf, bytes_read);
                  if (r != bytes_read)
                    {
                      propagate_libarchive_error (error, a);
                      g_prefix_error (error, "Failed to write %" G_GUINT64_FORMAT " bytes (code %" G_GUINT64_FORMAT"): ", (guint64)bytes_read, (guint64)r);
                      goto out;
                    }
                }
              }

            if (archive_write_finish_entry (a) != ARCHIVE_OK)
              {
                propagate_libarchive_error (error, a);
                goto out;
              }

            archive_entry_free (entry);
            entry = NULL;
          }
          break;
        default:
          g_assert_not_reached ();
        }
    }

  ret = TRUE;
 out:
  if (entry)
    archive_entry_free (entry);
  return ret;
}
#endif

/**
 * ostree_repo_export_tree_to_archive:
 * @self: An #OstreeRepo
 * @opts: Options controlling conversion
 * @root: An #OstreeRepoFile for the base directory
 * @archive: A `struct archive`, but specified as void to avoid a dependency on the libarchive headers
 * @cancellable: Cancellable
 * @error: Error
 *
 * Import an archive file @archive into the repository, and write its
 * file structure to @mtree.
 */
gboolean
ostree_repo_export_tree_to_archive (OstreeRepo                *self,
                                    OstreeRepoExportArchiveOptions *opts,
                                    OstreeRepoFile            *root,
                                    void                      *archive,
                                    GCancellable             *cancellable,
                                    GError                  **error)
{
#ifdef HAVE_LIBARCHIVE
  gboolean ret = FALSE;
  struct archive *a = archive;

  if (!write_directory_to_libarchive_recurse (self, opts, (GFile*)root, (GFile*)root,
                                              a, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
#else
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This version of ostree is not compiled with libarchive support");
  return FALSE;
#endif
}
