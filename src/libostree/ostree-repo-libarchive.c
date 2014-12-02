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

#include "ostree-repo-private.h"
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
  gs_unref_object GFileInfo *info = g_file_info_new ();
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

  _ostree_repo_commit_modifier_apply (repo, modifier,
                                      archive_entry_pathname (entry),
                                      info, &modified_info);

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
  gs_unref_object GInputStream *file_object_input = NULL;
  gs_unref_object GInputStream *archive_stream = NULL;
  guint64 length;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    archive_stream = _ostree_libarchive_input_stream_new (a);

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
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_ptrarray GPtrArray *split_path = NULL;
  gs_unref_ptrarray GPtrArray *hardlink_split_path = NULL;
  gs_unref_object OstreeMutableTree *subdir = NULL;
  gs_unref_object OstreeMutableTree *parent = NULL;
  gs_unref_object OstreeMutableTree *hardlink_source_parent = NULL;
  gs_free char *hardlink_source_checksum = NULL;
  gs_unref_object OstreeMutableTree *hardlink_source_subdir = NULL;
  gs_free guchar *tmp_csum = NULL;
  gs_free char *tmp_checksum = NULL;

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

          if (!import_libarchive_entry_file (self, a, entry, file_info, &tmp_csum,
                                             cancellable, error))
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
  struct archive_entry *entry;
  int r;
  gs_unref_object GFileInfo *tmp_dir_info = NULL;
  gs_free guchar *tmp_csum = NULL;

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

          if (!_ostree_repo_write_directory_meta (self, tmp_dir_info, NULL, &tmp_csum, cancellable, error))
            goto out;
        }

      if (!write_libarchive_entry_to_mtree (self, mtree, a,
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
