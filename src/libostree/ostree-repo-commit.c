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
#include <gio/gfiledescriptorbased.h>
#include <gio/gunixinputstream.h>
#include "otutil.h"
#include "libgsystem.h"

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-file-enumerator.h"
#include "ostree-checksum-input-stream.h"
#include "ostree-mutable-tree.h"
#include "ostree-varint.h"
#include <attr/xattr.h>
#include <glib/gprintf.h>

gboolean
_ostree_repo_ensure_loose_objdir_at (int             dfd,
                                     const char     *loose_path,
                                     GCancellable   *cancellable,
                                     GError        **error)
{
  char loose_prefix[3];

  loose_prefix[0] = loose_path[0];
  loose_prefix[1] = loose_path[1];
  loose_prefix[2] = '\0';
  if (mkdirat (dfd, loose_prefix, 0777) == -1)
    {
      int errsv = errno;
      if (G_UNLIKELY (errsv != EEXIST))
        {
          gs_set_error_from_errno (error, errsv);
          return FALSE;
        }
    }
  return TRUE;
}

void
_ostree_repo_get_tmpobject_path (OstreeRepo       *repo,
                                 char             *output,
                                 const char       *checksum,
                                 OstreeObjectType  objtype)
{
  g_sprintf (output,
             "%s/tmpobject-%s.%s",
             repo->boot_id,
             checksum,
             ostree_object_type_to_string (objtype));
}

static GVariant *
create_file_metadata (guint32       uid,
                      guint32       gid,
                      guint32       mode,
                      GVariant     *xattrs)
{
  GVariant *ret_metadata = NULL;
  gs_unref_variant GVariant *tmp_xattrs = NULL;

  if (xattrs == NULL)
    tmp_xattrs = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("(ayay)"), NULL, 0));

  ret_metadata = g_variant_new ("(uuu@a(ayay))",
                                GUINT32_TO_BE (uid),
                                GUINT32_TO_BE (gid),
                                GUINT32_TO_BE (mode),
                                xattrs ? xattrs : tmp_xattrs);
  g_variant_ref_sink (ret_metadata);

  return ret_metadata;
}

static gboolean
write_file_metadata_to_xattr (int fd,
                              guint32       uid,
                              guint32       gid,
                              guint32       mode,
                              GVariant     *xattrs,
                              GError       **error)
{
  gs_unref_variant GVariant *filemeta = NULL;
  int res;

  filemeta = create_file_metadata (uid, gid, mode, xattrs);

  do
    res = fsetxattr (fd, "user.ostreemeta",
                     (char*)g_variant_get_data (filemeta),
                     g_variant_get_size (filemeta),
                     0);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (G_UNLIKELY (res == -1))
    {
      gs_set_error_from_errno (error, errno);
      return FALSE;
    }

  return TRUE;
}


static gboolean
commit_loose_object_trusted (OstreeRepo        *self,
                             const char        *checksum,
                             OstreeObjectType   objtype,
                             const char        *loose_path,
                             const char        *temp_filename,
                             gboolean           object_is_symlink,
                             guint32            uid,
                             guint32            gid,
                             guint32            mode,
                             GVariant          *xattrs,
                             int                fd,
                             GCancellable      *cancellable,
                             GError           **error)
{
  gboolean ret = FALSE;

  /* We may be writing as root to a non-root-owned repository; if so,
   * automatically inherit the non-root ownership.
   */
  if (self->mode == OSTREE_REPO_MODE_ARCHIVE_Z2
      && self->target_owner_uid != -1) 
    {
      if (G_UNLIKELY (fchownat (self->tmp_dir_fd, temp_filename,
                                self->target_owner_uid,
                                self->target_owner_gid,
                                AT_SYMLINK_NOFOLLOW) == -1))
        {
          gs_set_error_from_errno (error, errno);
          goto out;
        }
    }

  /* Special handling for symlinks in bare repositories */
  if (object_is_symlink && self->mode == OSTREE_REPO_MODE_BARE)
    {
      /* Now that we know the checksum is valid, apply uid/gid, mode bits,
       * and extended attributes.
       *
       * Note, this does not apply for bare-user repos, as they store symlinks
       * as regular files.
       */
      if (G_UNLIKELY (fchownat (self->tmp_dir_fd, temp_filename,
                                uid, gid,
                                AT_SYMLINK_NOFOLLOW) == -1))
        {
          gs_set_error_from_errno (error, errno);
          goto out;
        }

      if (xattrs != NULL)
        {
          if (!gs_dfd_and_name_set_all_xattrs (self->tmp_dir_fd, temp_filename,
                                               xattrs, cancellable, error))
            goto out;
        }
    }
  else
    {
      int res;
      struct timespec times[2];

      if (objtype == OSTREE_OBJECT_TYPE_FILE && self->mode == OSTREE_REPO_MODE_BARE)
        {
          do
            res = fchown (fd, uid, gid);
          while (G_UNLIKELY (res == -1 && errno == EINTR));
          if (G_UNLIKELY (res == -1))
            {
              gs_set_error_from_errno (error, errno);
              goto out;
            }

          do
            res = fchmod (fd, mode);
          while (G_UNLIKELY (res == -1 && errno == EINTR));
          if (G_UNLIKELY (res == -1))
            {
              gs_set_error_from_errno (error, errno);
              goto out;
            }

          if (xattrs)
            {
              if (!gs_fd_set_all_xattrs (fd, xattrs, cancellable, error))
                goto out;
            }
        }

      if (objtype == OSTREE_OBJECT_TYPE_FILE && self->mode == OSTREE_REPO_MODE_BARE_USER)
        {
          if (!write_file_metadata_to_xattr (fd, uid, gid, mode, xattrs, error))
            goto out;

          if (!object_is_symlink)
            {
              /* We need to apply at least some mode bits, because the repo file was created
                 with mode 644, and we need e.g. exec bits to be right when we do a user-mode
                 checkout. To make this work we apply all user bits and the read bits for
                 group/other */
              do
                res = fchmod (fd, mode | 0744);
              while (G_UNLIKELY (res == -1 && errno == EINTR));
              if (G_UNLIKELY (res == -1))
                {
                  gs_set_error_from_errno (error, errno);
                  goto out;
                }
            }
        }

      if (objtype == OSTREE_OBJECT_TYPE_FILE && (self->mode == OSTREE_REPO_MODE_BARE ||
                                                 self->mode == OSTREE_REPO_MODE_BARE_USER))
        {
          /* To satisfy tools such as guile which compare mtimes
           * to determine whether or not source files need to be compiled,
           * set the modification time to 0.
           */
          times[0].tv_sec = 0; /* atime */
          times[0].tv_nsec = UTIME_OMIT;
          times[1].tv_sec = 0; /* mtime */
          times[1].tv_nsec = 0;
          do
            res = futimens (fd, times);
          while (G_UNLIKELY (res == -1 && errno == EINTR));
          if (G_UNLIKELY (res == -1))
            {
              gs_set_error_from_errno (error, errno);
              goto out;
            }
        }

      /* Ensure that in case of a power cut, these files have the data we
       * want.   See http://lwn.net/Articles/322823/
       */
      if (!self->in_transaction && !self->disable_fsync)
        {
          if (fsync (fd) == -1)
            {
              gs_set_error_from_errno (error, errno);
              goto out;
            }
        }
    }
  
  if (!_ostree_repo_ensure_loose_objdir_at (self->objects_dir_fd, loose_path,
                                            cancellable, error))
    goto out;

  {
    gs_free gchar *tmp_dest = NULL;
    int dir;
    const char *dest;

    if (self->in_transaction)
      {
        char tmpbuf[_OSTREE_LOOSE_PATH_MAX];
        _ostree_repo_get_tmpobject_path (self, tmpbuf, checksum, objtype);
        tmp_dest = g_strdup (tmpbuf);
        dir = self->tmp_dir_fd;
        dest = tmp_dest;
      }
    else
      {
        dir = self->objects_dir_fd;
        dest = loose_path;
      }

    if (G_UNLIKELY (renameat (self->tmp_dir_fd, temp_filename,
                              dir, dest) == -1))
      {
        if (errno != EEXIST)
          {
            gs_set_error_from_errno (error, errno);
            g_prefix_error (error, "Storing file '%s': ", temp_filename);
            goto out;
          }
        else
          (void) unlinkat (self->tmp_dir_fd, temp_filename, 0);
      }
  }
  ret = TRUE;
 out:
  return ret;
}

typedef struct
{
  gsize unpacked;
  gsize archived;
} OstreeContentSizeCacheEntry;

static OstreeContentSizeCacheEntry *
content_size_cache_entry_new (gsize unpacked,
                              gsize archived)
{
  OstreeContentSizeCacheEntry *entry = g_slice_new0 (OstreeContentSizeCacheEntry);

  entry->unpacked = unpacked;
  entry->archived = archived;

  return entry;
}

static void
content_size_cache_entry_free (gpointer entry)
{
  if (entry)
    g_slice_free (OstreeContentSizeCacheEntry, entry);
}

static void
repo_store_size_entry (OstreeRepo       *self,
                       const gchar      *checksum,
                       gsize             unpacked,
                       gsize             archived)
{
  if (G_UNLIKELY (self->object_sizes == NULL))
    self->object_sizes = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, content_size_cache_entry_free);

  g_hash_table_replace (self->object_sizes,
                        g_strdup (checksum),
                        content_size_cache_entry_new (unpacked, archived));
}

static int
compare_ascii_checksums_for_sorting (gconstpointer  a_pp,
                                     gconstpointer  b_pp)
{
  char *a = *((char**)a_pp);
  char *b = *((char**)b_pp);

  return strcmp (a, b);
}

/*
 * Create sizes metadata GVariant and add it to the metadata variant given.
 */
static gboolean
add_size_index_to_metadata (OstreeRepo        *self,
                            GVariant          *original_metadata,
                            GVariant         **out_metadata,
                            GCancellable      *cancellable,
                            GError           **error)
{
  gboolean ret = FALSE;
  gs_unref_variant_builder GVariantBuilder *builder = NULL;
    
  if (original_metadata)
    {
      builder = ot_util_variant_builder_from_variant (original_metadata, G_VARIANT_TYPE ("a{sv}"));
    }
  else
    {
      builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
    }

  if (self->object_sizes &&
      g_hash_table_size (self->object_sizes) > 0)
    {
      GHashTableIter entries = { 0 };
      gchar *e_checksum = NULL;
      OstreeContentSizeCacheEntry *e_size = NULL;
      GVariantBuilder index_builder;
      guint i;
      gs_unref_ptrarray GPtrArray *sorted_keys = NULL;
      
      g_hash_table_iter_init (&entries, self->object_sizes);
      g_variant_builder_init (&index_builder,
                              G_VARIANT_TYPE ("a" _OSTREE_OBJECT_SIZES_ENTRY_SIGNATURE));

      /* Sort the checksums so we can bsearch if desired */
      sorted_keys = g_ptr_array_new ();
      while (g_hash_table_iter_next (&entries,
                                     (gpointer *) &e_checksum,
                                     (gpointer *) &e_size))
        g_ptr_array_add (sorted_keys, e_checksum);
      g_ptr_array_sort (sorted_keys, compare_ascii_checksums_for_sorting);

      for (i = 0; i < sorted_keys->len; i++)
        {
          guint8 csum[32];
          const char *e_checksum = sorted_keys->pdata[i];
          GString *buffer = g_string_new (NULL);

          ostree_checksum_inplace_to_bytes (e_checksum, csum);
          g_string_append_len (buffer, (char*)csum, 32);

          e_size = g_hash_table_lookup (self->object_sizes, e_checksum);
          _ostree_write_varuint64 (buffer, e_size->archived);
          _ostree_write_varuint64 (buffer, e_size->unpacked);

          g_variant_builder_add (&index_builder, "@ay",
                                 ot_gvariant_new_bytearray ((guint8*)buffer->str, buffer->len));
          g_string_free (buffer, TRUE);
        }
      
      g_variant_builder_add (builder, "{sv}", "ostree.sizes",
                             g_variant_builder_end (&index_builder));
    }
    
  ret = TRUE;
  *out_metadata = g_variant_builder_end (builder);
  g_variant_ref_sink (*out_metadata);

  return ret;
}

static gboolean
fallocate_stream (GFileDescriptorBased      *stream,
                  goffset                    size,
                  GCancellable              *cancellable,
                  GError                   **error)
{
  gboolean ret = FALSE;
  int fd = g_file_descriptor_based_get_fd (stream);

  if (size > 0)
    {
      int r = posix_fallocate (fd, 0, size);
      if (r != 0)
        {
          gs_set_error_from_errno (error, r);
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
_ostree_repo_open_trusted_content_bare (OstreeRepo          *self,
                                        const char          *checksum,
                                        guint64              content_len,
                                        OstreeRepoTrustedContentBareCommit *out_state,
                                        GOutputStream      **out_stream,
                                        gboolean            *out_have_object,
                                        GCancellable        *cancellable,
                                        GError             **error)
{
  gboolean ret = FALSE;
  gs_free char *temp_filename = NULL;
  gs_unref_object GOutputStream *ret_stream = NULL;
  gboolean have_obj;
  char loose_objpath[_OSTREE_LOOSE_PATH_MAX];

  if (!_ostree_repo_has_loose_object (self, checksum, OSTREE_OBJECT_TYPE_FILE,
                                      &have_obj, loose_objpath,
                                      NULL,
                                      cancellable, error))
    goto out;

  if (!have_obj)
    {
      if (!gs_file_open_in_tmpdir_at (self->tmp_dir_fd, 0644, &temp_filename, &ret_stream,
                                      cancellable, error))
        goto out;
      
      if (!fallocate_stream ((GFileDescriptorBased*)ret_stream, content_len,
                             cancellable, error))
        goto out;
    }

  ret = TRUE;
  if (!have_obj)
    {
      out_state->temp_filename = temp_filename;
      temp_filename = NULL;
      out_state->fd = g_file_descriptor_based_get_fd ((GFileDescriptorBased*)ret_stream);
      gs_transfer_out_value (out_stream, &ret_stream);
    }
  *out_have_object = have_obj;
 out:
  return ret;
}

gboolean
_ostree_repo_commit_trusted_content_bare (OstreeRepo          *self,
                                          const char          *checksum,
                                          OstreeRepoTrustedContentBareCommit *state,
                                          guint32              uid,
                                          guint32              gid,
                                          guint32              mode,
                                          GVariant            *xattrs,
                                          GCancellable        *cancellable,
                                          GError             **error)
{
  gboolean ret = FALSE;
  char loose_objpath[_OSTREE_LOOSE_PATH_MAX];

  if (state->fd != -1)
    {
      _ostree_loose_path (loose_objpath, checksum, OSTREE_OBJECT_TYPE_FILE, self->mode);
      
      if (!commit_loose_object_trusted (self, checksum, OSTREE_OBJECT_TYPE_FILE,
                                        loose_objpath,
                                        state->temp_filename,
                                        FALSE, uid, gid, mode,
                                        xattrs, state->fd,
                                        cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  g_free (state->temp_filename);
  return ret;
}

static gboolean
write_object (OstreeRepo         *self,
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
  gs_free char *temp_filename = NULL;
  gs_unref_object GFile *stored_path = NULL;
  gs_free guchar *ret_csum = NULL;
  gs_unref_object OstreeChecksumInputStream *checksum_input = NULL;
  gs_unref_object GInputStream *file_input = NULL;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_variant GVariant *xattrs = NULL;
  gs_unref_object GOutputStream *temp_out = NULL;
  gboolean have_obj;
  GChecksum *checksum = NULL;
  gboolean temp_file_is_regular;
  gboolean temp_file_is_symlink;
  gboolean object_is_symlink = FALSE;
  char loose_objpath[_OSTREE_LOOSE_PATH_MAX];
  gssize unpacked_size = 0;
  gboolean indexable = FALSE;

  g_return_val_if_fail (expected_checksum || out_csum, FALSE);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (expected_checksum)
    {
      if (!_ostree_repo_has_loose_object (self, expected_checksum, objtype,
                                          &have_obj, loose_objpath,
                                          NULL, cancellable, error))
        goto out;
      if (have_obj)
        {
          if (out_csum)
            *out_csum = ostree_checksum_to_bytes (expected_checksum);
          ret = TRUE;
          goto out;
        }
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
      if (!ostree_content_stream_parse (FALSE, checksum_input ? (GInputStream*)checksum_input : input,
                                        file_object_length, FALSE,
                                        &file_input, &file_info, &xattrs,
                                        cancellable, error))
        goto out;

      temp_file_is_regular = g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR;
      temp_file_is_symlink = object_is_symlink =
        g_file_info_get_file_type (file_info) == G_FILE_TYPE_SYMBOLIC_LINK;

      if (repo_mode == OSTREE_REPO_MODE_BARE_USER && object_is_symlink)
        {
          const char *target_str = g_file_info_get_symlink_target (file_info);
          gs_unref_bytes GBytes *target = g_bytes_new (target_str, strlen (target_str) + 1);

          /* For bare-user we can't store symlinks as symlinks, as symlinks don't
             support user xattrs to store the ownership. So, instead store them
             as regular files */
          temp_file_is_regular = TRUE;
          temp_file_is_symlink = FALSE;
          if (file_input != NULL)
            g_object_unref (file_input);

          /* Include the terminating zero so we can e.g. mmap this file */
          file_input = g_memory_input_stream_new_from_bytes (target);
        }

      if (!(temp_file_is_regular || temp_file_is_symlink))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unsupported file type %u", g_file_info_get_file_type (file_info));
          goto out;
        }

      /* For regular files, we create them with default mode, and only
       * later apply any xattrs and setuid bits.  The rationale here
       * is that an attacker on the network with the ability to MITM
       * could potentially cause the system to make a temporary setuid
       * binary with trailing garbage, creating a window on the local
       * system where a malicious setuid binary exists.
       */
      if ((repo_mode == OSTREE_REPO_MODE_BARE || repo_mode == OSTREE_REPO_MODE_BARE_USER) && temp_file_is_regular)
        {
          guint64 size = g_file_info_get_size (file_info);

          if (!gs_file_open_in_tmpdir_at (self->tmp_dir_fd, 0644, &temp_filename, &temp_out,
                                          cancellable, error))
            goto out;

          if (!fallocate_stream ((GFileDescriptorBased*)temp_out, size,
                                 cancellable, error))
            goto out;

          if (g_output_stream_splice (temp_out, file_input, 0,
                                      cancellable, error) < 0)
            goto out;
        }
      else if (repo_mode == OSTREE_REPO_MODE_BARE && temp_file_is_symlink)
        {
          if (!_ostree_make_temporary_symlink_at (self->tmp_dir_fd,
                                                  g_file_info_get_symlink_target (file_info),
                                                  &temp_filename,
                                                  cancellable, error))
            goto out;
        }
      else if (repo_mode == OSTREE_REPO_MODE_ARCHIVE_Z2)
        {
          gs_unref_variant GVariant *file_meta = NULL;
          gs_unref_object GConverter *zlib_compressor = NULL;
          gs_unref_object GOutputStream *compressed_out_stream = NULL;

          if (self->generate_sizes)
            indexable = TRUE;

          if (!gs_file_open_in_tmpdir_at (self->tmp_dir_fd, 0644,
                                          &temp_filename, &temp_out,
                                          cancellable, error))
            goto out;
          temp_file_is_regular = TRUE;

          file_meta = _ostree_zlib_file_header_new (file_info, xattrs);

          if (!_ostree_write_variant_with_size (temp_out, file_meta, 0, NULL, NULL,
                                                cancellable, error))
            goto out;

          if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
            {
              zlib_compressor = (GConverter*)g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW, 9);
              compressed_out_stream = g_converter_output_stream_new (temp_out, zlib_compressor);
              /* Don't close the base; we'll do that later */
              g_filter_output_stream_set_close_base_stream ((GFilterOutputStream*)compressed_out_stream, FALSE);
              
              unpacked_size = g_output_stream_splice (compressed_out_stream, file_input,
                                                      0, cancellable, error);
              if (unpacked_size < 0)
                goto out;
            }
        }
      else
        g_assert_not_reached ();
    }
  else
    {
      if (!gs_file_open_in_tmpdir_at (self->tmp_dir_fd, 0644, &temp_filename, &temp_out,
                                      cancellable, error))
        goto out;

      if (!fallocate_stream ((GFileDescriptorBased*)temp_out, file_object_length,
                             cancellable, error))
        goto out;

      if (g_output_stream_splice (temp_out, checksum_input ? (GInputStream*)checksum_input : input,
                                  0,
                                  cancellable, error) < 0)
        goto out;
      temp_file_is_regular = TRUE;
    }

  if (temp_out)
    {
      if (!g_output_stream_flush (temp_out, cancellable, error))
        goto out;
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

  g_assert (actual_checksum != NULL); /* Pacify static analysis */
          
  if (indexable && temp_file_is_regular)
    {
      struct stat stbuf;

      if (fstatat (self->tmp_dir_fd, temp_filename, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
        {
          gs_set_error_from_errno (error, errno);
          goto out;
        }

      repo_store_size_entry (self, actual_checksum, unpacked_size, stbuf.st_size);
    }

  if (!_ostree_repo_has_loose_object (self, actual_checksum, objtype,
                                      &have_obj, loose_objpath, NULL,
                                      cancellable, error))
    goto out;
          
  do_commit = !have_obj;

  if (do_commit)
    {
      guint32 uid, gid, mode;
      int fd = -1;

      if (file_info)
        {
          uid = g_file_info_get_attribute_uint32 (file_info, "unix::uid");
          gid = g_file_info_get_attribute_uint32 (file_info, "unix::gid");
          mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
        }
      else
        uid = gid = mode = 0;

      if (temp_out)
        fd = g_file_descriptor_based_get_fd ((GFileDescriptorBased*)temp_out);
      
      if (!commit_loose_object_trusted (self, actual_checksum, objtype,
                                        loose_objpath,
                                        temp_filename,
                                        object_is_symlink,
                                        uid, gid, mode,
                                        xattrs, fd,
                                        cancellable, error))
        goto out;

      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        {
          if (G_UNLIKELY (file_object_length > OSTREE_MAX_METADATA_WARN_SIZE))
            {
              gs_free char *metasize = g_format_size (file_object_length);
              gs_free char *warnsize = g_format_size (OSTREE_MAX_METADATA_WARN_SIZE);
              gs_free char *maxsize = g_format_size (OSTREE_MAX_METADATA_SIZE);
              g_warning ("metadata object %s is %s, which is larger than the warning threshold of %s." \
                         "  The hard limit on metadata size is %s.  Put large content in the tree itself, not in metadata.",
                         actual_checksum,
                         metasize, warnsize, maxsize);
            }
        }

      g_clear_pointer (&temp_filename, g_free);
    }

  g_mutex_lock (&self->txn_stats_lock);
  if (do_commit)
    {
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        {
          self->txn_stats.metadata_objects_written++;
        }
      else
        {
          self->txn_stats.content_objects_written++;
          self->txn_stats.content_bytes_written += file_object_length;
        }
    }
  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    self->txn_stats.metadata_objects_total++;
  else
    self->txn_stats.content_objects_total++;
  g_mutex_unlock (&self->txn_stats_lock);
      
  if (checksum)
    ret_csum = ot_csum_from_gchecksum (checksum);

  ret = TRUE;
  ot_transfer_out_value(out_csum, &ret_csum);
 out:
  if (temp_filename)
    (void) unlinkat (self->tmp_dir_fd, temp_filename, 0);
  g_clear_pointer (&checksum, (GDestroyNotify) g_checksum_free);
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

  if (self->parent_repo)
    {
      if (!scan_loose_devino (self->parent_repo, devino_cache, cancellable, error))
        goto out;
    }

  repo_mode = ostree_repo_get_mode (self);

  if (!_ostree_repo_get_loose_object_dirs (self, &object_dirs, cancellable, error))
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
            case OSTREE_REPO_MODE_ARCHIVE_Z2:
            case OSTREE_REPO_MODE_BARE:
            case OSTREE_REPO_MODE_BARE_USER:
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

/**
 * ostree_repo_scan_hardlinks:
 * @self: An #OstreeRepo
 * @cancellable: Cancellable
 * @error: Error
 *
 * When ostree builds a mutable tree from directory like in
 * ostree_repo_write_directory_to_mtree(), it has to scan all files that you
 * pass in and compute their checksums. If your commit contains hardlinks from
 * ostree's existing repo, ostree can build a mapping of device numbers and
 * inodes to their checksum.
 *
 * There is an upfront cost to creating this mapping, as this will scan the
 * entire objects directory. If your commit is composed of mostly hardlinks to
 * existing ostree objects, then this will speed up considerably, so call it
 * before you call ostree_write_directory_to_mtree() or similar.
 */
gboolean
ostree_repo_scan_hardlinks (OstreeRepo    *self,
                            GCancellable  *cancellable,
                            GError       **error)
{
  gboolean ret = FALSE;

  g_return_val_if_fail (self->in_transaction == TRUE, FALSE);

  if (!self->loose_object_devino_hash)
    self->loose_object_devino_hash = g_hash_table_new_full (devino_hash, devino_equal, g_free, g_free);
  g_hash_table_remove_all (self->loose_object_devino_hash);
  if (!scan_loose_devino (self, self->loose_object_devino_hash, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_prepare_transaction:
 * @self: An #OstreeRepo
 * @out_transaction_resume: (allow-none) (out): Whether this transaction
 * is resuming from a previous one.
 * @cancellable: Cancellable
 * @error: Error
 *
 * Starts or resumes a transaction. In order to write to a repo, you
 * need to start a transaction. You can complete the transaction with
 * ostree_repo_commit_transaction(), or abort the transaction with
 * ostree_repo_abort_transaction().
 *
 * Currently, transactions are not atomic, and aborting a transaction
 * will not erase any data you  write during the transaction.
 */
gboolean
ostree_repo_prepare_transaction (OstreeRepo     *self,
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

  memset (&self->txn_stats, 0, sizeof (OstreeRepoTransactionStats));

  self->in_transaction = TRUE;
  if (ret_transaction_resume)
    {
      if (!ot_gfile_ensure_unlinked (self->transaction_lock_path, cancellable, error))
        goto out;
    }

  if (mkdirat (self->tmp_dir_fd, self->boot_id, 0777) == -1)
    {
      int errsv = errno;
      if (G_UNLIKELY (errsv != EEXIST))
        {
          gs_set_error_from_errno (error, errsv);
          goto out;
        }
    }

  transaction_str = g_strdup_printf ("pid=%llu", (unsigned long long) getpid ());
  if (!g_file_make_symbolic_link (self->transaction_lock_path, transaction_str,
                                  cancellable, error))
    goto out;

  ret = TRUE;
  if (out_transaction_resume)
    *out_transaction_resume = ret_transaction_resume;
 out:
  return ret;
}

static gboolean
rename_pending_loose_objects (OstreeRepo        *self,
                              GCancellable      *cancellable,
                              GError           **error)
{
  gboolean ret = FALSE;
  gs_dirfd_iterator_cleanup GSDirFdIterator child_dfd_iter = { 0, };

  if (!gs_dirfd_iterator_init_at (self->tmp_dir_fd, self->boot_id, FALSE, &child_dfd_iter, error))
    goto out;

  while (TRUE)
    {
      struct dirent *out_dent;

      if (!gs_dirfd_iterator_next_dent (&child_dfd_iter, &out_dent, cancellable, error))
        goto out;

      if (out_dent == NULL)
        break;

      if (strncmp (out_dent->d_name, "tmpobject-", 10) == 0)
        {
          char loose_path[_OSTREE_LOOSE_PATH_MAX];
          gs_free gchar *checksum = NULL;
          OstreeObjectType type;
          ostree_object_from_string (out_dent->d_name + 10,
                                     &checksum,
                                     &type);

          _ostree_loose_path (loose_path, checksum, type, self->mode);

          if (!_ostree_repo_ensure_loose_objdir_at (self->objects_dir_fd, loose_path,
                                                    cancellable, error))
            goto out;

          if (G_UNLIKELY (renameat (child_dfd_iter.fd, out_dent->d_name,
                                    self->objects_dir_fd, loose_path) < 0))
            {
              (void) unlinkat (self->tmp_dir_fd, out_dent->d_name, 0);
              if (errno != EEXIST)
                {
                  gs_set_error_from_errno (error, errno);
                  g_prefix_error (error, "Storing file '%s': ", loose_path);
                  goto out;
                }
            }
          continue;
        }
    }

  if (!gs_shutil_rm_rf_at (self->tmp_dir_fd, self->boot_id, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
cleanup_tmpdir (OstreeRepo        *self,
                GCancellable      *cancellable,
                GError           **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *enumerator = NULL;
  guint64 curtime_secs;

  enumerator = g_file_enumerate_children (self->tmp_dir, "standard::name,time::modified",
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable,
                                          error);
  if (!enumerator)
    goto out;

  curtime_secs = g_get_real_time () / 1000000;

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *path;
      guint64 mtime;
      guint64 delta;

      if (!gs_file_enumerator_iterate (enumerator, &file_info, &path,
                                       cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      mtime = g_file_info_get_attribute_uint64 (file_info, "time::modified");
      if (mtime > curtime_secs)
        continue;
      /* Only delete files older than a day.  To do better, we would
       * need to coordinate between multiple processes in a reliable
       * fashion.  See
       * https://bugzilla.gnome.org/show_bug.cgi?id=709115
       */
      delta = curtime_secs - mtime;
      if (delta > 60*60*24)
        {
          if (!gs_shutil_rm_rf (path, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static void
ensure_txn_refs (OstreeRepo *self)
{
  if (self->txn_refs == NULL)
    self->txn_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

/**
 * ostree_repo_transaction_set_refspec:
 * @self: An #OstreeRepo
 * @refspec: The refspec to write
 * @checksum: The checksum to point it to
 *
 * Like ostree_repo_transaction_set_ref(), but takes concatenated
 * @refspec format as input instead of separate remote and name
 * arguments.
 */
void
ostree_repo_transaction_set_refspec (OstreeRepo *self,
                                     const char *refspec,
                                     const char *checksum)
{
  g_return_if_fail (self->in_transaction == TRUE);

  ensure_txn_refs (self);

  g_hash_table_replace (self->txn_refs, g_strdup (refspec), g_strdup (checksum));
}

/**
 * ostree_repo_transaction_set_ref:
 * @self: An #OstreeRepo
 * @remote: (allow-none): A remote for the ref
 * @ref: The ref to write
 * @checksum: The checksum to point it to
 *
 * If @checksum is not %NULL, then record it as the target of ref named
 * @ref; if @remote is provided, the ref will appear to originate from that
 * remote.
 *
 * Otherwise, if @checksum is %NULL, then record that the ref should
 * be deleted.
 *
 * The change will not be written out immediately, but when the transaction
 * is completed with ostree_repo_complete_transaction(). If the transaction
 * is instead aborted with ostree_repo_abort_transaction(), no changes will
 * be made to the repository.
 */
void
ostree_repo_transaction_set_ref (OstreeRepo *self,
                                 const char *remote,
                                 const char *ref,
                                 const char *checksum)
{
  char *refspec;

  g_return_if_fail (self->in_transaction == TRUE);

  ensure_txn_refs (self);

  if (remote)
    refspec = g_strdup_printf ("%s:%s", remote, ref);
  else
    refspec = g_strdup (ref);

  g_hash_table_replace (self->txn_refs, refspec, g_strdup (checksum));
}

/**
 * ostree_repo_set_ref_immediate:
 * @self: An #OstreeRepo
 * @remote: (allow-none): A remote for the ref
 * @ref: The ref to write
 * @checksum: The checksum to point it to
 * @cancellable: GCancellable
 * @error: GError
 *
 * This is like ostree_repo_transaction_set_ref(), except it may be
 * invoked outside of a transaction.  This is presently safe for the
 * case where we're creating or overwriting an existing ref.
 */
gboolean
ostree_repo_set_ref_immediate (OstreeRepo *self,
                               const char *remote,
                               const char *ref,
                               const char *checksum,
                               GCancellable  *cancellable,
                               GError       **error)
{
  return _ostree_repo_write_ref (self, remote, ref, checksum,
                                 cancellable, error);
}

/**
 * ostree_repo_commit_transaction:
 * @self: An #OstreeRepo
 * @out_stats: (allow-none) (out): A set of statistics of things
 * that happened during this transaction.
 * @cancellable: Cancellable
 * @error: Error
 *
 * Complete the transaction. Any refs set with
 * ostree_repo_transaction_set_ref() or
 * ostree_repo_transaction_set_refspec() will be written out.
 */
gboolean
ostree_repo_commit_transaction (OstreeRepo                  *self,
                                OstreeRepoTransactionStats  *out_stats,
                                GCancellable                *cancellable,
                                GError                     **error)
{
  gboolean ret = FALSE;

  g_return_val_if_fail (self->in_transaction == TRUE, FALSE);

  if (syncfs (self->tmp_dir_fd) < 0)
    {
      gs_set_error_from_errno (error, errno);
      goto out;
    }

  if (! rename_pending_loose_objects (self, cancellable, error))
    goto out;

  if (!cleanup_tmpdir (self, cancellable, error))
    goto out;

  if (self->loose_object_devino_hash)
    g_hash_table_remove_all (self->loose_object_devino_hash);

  if (self->txn_refs)
    if (!_ostree_repo_update_refs (self, self->txn_refs, cancellable, error))
      goto out;
  g_clear_pointer (&self->txn_refs, g_hash_table_destroy);

  self->in_transaction = FALSE;

  if (!ot_gfile_ensure_unlinked (self->transaction_lock_path, cancellable, error))
    goto out;

  if (out_stats)
    *out_stats = self->txn_stats;

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

  if (!self->in_transaction)
    return TRUE;

  if (!cleanup_tmpdir (self, cancellable, error))
    goto out;

  if (self->loose_object_devino_hash)
    g_hash_table_remove_all (self->loose_object_devino_hash);

  g_clear_pointer (&self->txn_refs, g_hash_table_destroy);

  self->in_transaction = FALSE;

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_write_metadata:
 * @self: Repo
 * @objtype: Object type
 * @expected_checksum: (allow-none): If provided, validate content against this checksum
 * @object: Metadata
 * @out_csum: (out) (array fixed-size=32) (allow-none): Binary checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store the metadata object @variant.  Return the checksum
 * as @out_csum.
 *
 * If @expected_checksum is not %NULL, verify it against the
 * computed checksum.
 */
gboolean
ostree_repo_write_metadata (OstreeRepo         *self,
                            OstreeObjectType    objtype,
                            const char         *expected_checksum,
                            GVariant           *object,
                            guchar            **out_csum,
                            GCancellable       *cancellable,
                            GError            **error)
{
  gboolean ret = FALSE;
  gs_unref_object GInputStream *input = NULL;
  gs_unref_variant GVariant *normalized = NULL;

  normalized = g_variant_get_normal_form (object);

  if (G_UNLIKELY (g_variant_get_size (normalized) > OSTREE_MAX_METADATA_SIZE))
    {
      gs_free char *input_bytes = g_format_size (g_variant_get_size (normalized));
      gs_free char *max_bytes = g_format_size (OSTREE_MAX_METADATA_SIZE);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Metadata object of type '%s' is %s; maximum metadata size is %s",
                   ostree_object_type_to_string (objtype),
                   input_bytes,
                   max_bytes);
      goto out;
    }

  input = ot_variant_read (normalized);

  if (!write_object (self, objtype, expected_checksum,
                     input, g_variant_get_size (normalized),
                     out_csum,
                     cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_write_metadata_stream_trusted:
 * @self: Repo
 * @objtype: Object type
 * @checksum: Store object with this ASCII SHA256 checksum
 * @object_input: Metadata object stream
 * @length: Length, may be 0 for unknown
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store the metadata object @variant; the provided @checksum is
 * trusted.
 */
gboolean
ostree_repo_write_metadata_stream_trusted (OstreeRepo        *self,
                                           OstreeObjectType   objtype,
                                           const char        *checksum,
                                           GInputStream      *object_input,
                                           guint64            length,
                                           GCancellable      *cancellable,
                                           GError           **error)
{
  return write_object (self, objtype, checksum, object_input, length, NULL,
                       cancellable, error);
}

/**
 * ostree_repo_write_metadata_trusted:
 * @self: Repo
 * @objtype: Object type
 * @checksum: Store object with this ASCII SHA256 checksum
 * @variant: Metadata object
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store the metadata object @variant; the provided @checksum is
 * trusted.
 */
gboolean
ostree_repo_write_metadata_trusted (OstreeRepo         *self,
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

  return write_object (self, type, checksum,
                       input, g_variant_get_size (normalized),
                       NULL,
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
} WriteMetadataAsyncData;

static void
write_metadata_async_data_free (gpointer user_data)
{
  WriteMetadataAsyncData *data = user_data;

  g_clear_object (&data->repo);
  g_clear_object (&data->cancellable);
  g_variant_unref (data->object);
  g_free (data->result_csum);
  g_free (data->expected_checksum);
  g_free (data);
}

static void
write_metadata_thread (GSimpleAsyncResult  *res,
                       GObject             *object,
                       GCancellable        *cancellable)
{
  GError *error = NULL;
  WriteMetadataAsyncData *data;

  data = g_simple_async_result_get_op_res_gpointer (res);
  if (!ostree_repo_write_metadata (data->repo, data->objtype, data->expected_checksum,
                                   data->object,
                                   &data->result_csum,
                                   cancellable, &error))
    g_simple_async_result_take_error (res, error);
}

/**
 * ostree_repo_write_metadata_async:
 * @self: Repo
 * @objtype: Object type
 * @expected_checksum: (allow-none): If provided, validate content against this checksum
 * @object: Metadata
 * @cancellable: Cancellable
 * @callback: Invoked when metadata is writed
 * @user_data: Data for @callback
 *
 * Asynchronously store the metadata object @variant.  If provided,
 * the checksum @expected_checksum will be verified.
 */
void
ostree_repo_write_metadata_async (OstreeRepo               *self,
                                  OstreeObjectType          objtype,
                                  const char               *expected_checksum,
                                  GVariant                 *object,
                                  GCancellable             *cancellable,
                                  GAsyncReadyCallback       callback,
                                  gpointer                  user_data)
{
  WriteMetadataAsyncData *asyncdata;

  asyncdata = g_new0 (WriteMetadataAsyncData, 1);
  asyncdata->repo = g_object_ref (self);
  asyncdata->objtype = objtype;
  asyncdata->expected_checksum = g_strdup (expected_checksum);
  asyncdata->object = g_variant_ref (object);
  asyncdata->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  asyncdata->result = g_simple_async_result_new ((GObject*) self,
                                                 callback, user_data,
                                                 ostree_repo_write_metadata_async);

  g_simple_async_result_set_op_res_gpointer (asyncdata->result, asyncdata,
                                             write_metadata_async_data_free);
  g_simple_async_result_run_in_thread (asyncdata->result, write_metadata_thread, G_PRIORITY_DEFAULT, cancellable);
  g_object_unref (asyncdata->result);
}

gboolean
ostree_repo_write_metadata_finish (OstreeRepo        *self,
                                   GAsyncResult      *result,
                                   guchar           **out_csum,
                                   GError           **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  WriteMetadataAsyncData *data;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == ostree_repo_write_metadata_async);

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  data = g_simple_async_result_get_op_res_gpointer (simple);
  /* Transfer ownership */
  *out_csum = data->result_csum;
  data->result_csum = NULL;
  return TRUE;
}

gboolean
_ostree_repo_write_directory_meta (OstreeRepo   *self,
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

  return ostree_repo_write_metadata (self, OSTREE_OBJECT_TYPE_DIR_META, NULL,
                                     dirmeta, out_csum, cancellable, error);
}

GFile *
_ostree_repo_get_object_path (OstreeRepo       *self,
                              const char       *checksum,
                              OstreeObjectType  type)
{
  char *relpath;
  GFile *ret;
  gboolean compressed;

  compressed = (type == OSTREE_OBJECT_TYPE_FILE
                && ostree_repo_get_mode (self) == OSTREE_REPO_MODE_ARCHIVE_Z2);
  relpath = _ostree_get_relative_object_path (checksum, type, compressed);
  ret = g_file_resolve_relative_path (self->repodir, relpath);
  g_free (relpath);

  return ret;
}

/**
 * ostree_repo_write_content_trusted:
 * @self: Repo
 * @checksum: Store content using this ASCII SHA256 checksum
 * @object_input: Content stream
 * @length: Length of @object_input
 * @cancellable: Cancellable
 * @error: Data for @callback
 *
 * Store the content object streamed as @object_input, with total
 * length @length.  The given @checksum will be treated as trusted.
 *
 * This function should be used when importing file objects from local
 * disk, for example.
 */
gboolean
ostree_repo_write_content_trusted (OstreeRepo       *self,
                                   const char       *checksum,
                                   GInputStream     *object_input,
                                   guint64           length,
                                   GCancellable     *cancellable,
                                   GError          **error)
{
  return write_object (self, OSTREE_OBJECT_TYPE_FILE, checksum,
                       object_input, length, NULL,
                       cancellable, error);
}

/**
 * ostree_repo_write_content:
 * @self: Repo
 * @expected_checksum: (allow-none): If provided, validate content against this checksum
 * @object_input: Content object stream
 * @length: Length of @object_input
 * @out_csum: (out) (array fixed-size=32) (allow-none): Binary checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store the content object streamed as @object_input,
 * with total length @length.  The actual checksum will
 * be returned as @out_csum.
 */
gboolean
ostree_repo_write_content (OstreeRepo       *self,
                           const char       *expected_checksum,
                           GInputStream     *object_input,
                           guint64           length,
                           guchar          **out_csum,
                           GCancellable     *cancellable,
                           GError          **error)
{
  return write_object (self, OSTREE_OBJECT_TYPE_FILE, expected_checksum,
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
} WriteContentAsyncData;

static void
write_content_async_data_free (gpointer user_data)
{
  WriteContentAsyncData *data = user_data;

  g_clear_object (&data->repo);
  g_clear_object (&data->cancellable);
  g_clear_object (&data->object);
  g_free (data->result_csum);
  g_free (data->expected_checksum);
  g_free (data);
}

static void
write_content_thread (GSimpleAsyncResult  *res,
                      GObject             *object,
                      GCancellable        *cancellable)
{
  GError *error = NULL;
  WriteContentAsyncData *data;

  data = g_simple_async_result_get_op_res_gpointer (res);
  if (!ostree_repo_write_content (data->repo, data->expected_checksum,
                                  data->object, data->file_object_length,
                                  &data->result_csum,
                                  cancellable, &error))
    g_simple_async_result_take_error (res, error);
}

/**
 * ostree_repo_write_content_async:
 * @self: Repo
 * @expected_checksum: (allow-none): If provided, validate content against this checksum
 * @object: Input
 * @length: Length of @object
 * @cancellable: Cancellable
 * @callback: Invoked when content is writed
 * @user_data: User data for @callback
 *
 * Asynchronously store the content object @object.  If provided, the
 * checksum @expected_checksum will be verified.
 */
void
ostree_repo_write_content_async (OstreeRepo               *self,
                                 const char               *expected_checksum,
                                 GInputStream             *object,
                                 guint64                   length,
                                 GCancellable             *cancellable,
                                 GAsyncReadyCallback       callback,
                                 gpointer                  user_data)
{
  WriteContentAsyncData *asyncdata;

  asyncdata = g_new0 (WriteContentAsyncData, 1);
  asyncdata->repo = g_object_ref (self);
  asyncdata->expected_checksum = g_strdup (expected_checksum);
  asyncdata->object = g_object_ref (object);
  asyncdata->file_object_length = length;
  asyncdata->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  asyncdata->result = g_simple_async_result_new ((GObject*) self,
                                                 callback, user_data,
                                                 ostree_repo_write_content_async);

  g_simple_async_result_set_op_res_gpointer (asyncdata->result, asyncdata,
                                             write_content_async_data_free);
  g_simple_async_result_run_in_thread (asyncdata->result, write_content_thread, G_PRIORITY_DEFAULT, cancellable);
  g_object_unref (asyncdata->result);
}

/**
 * ostree_repo_write_content_finish:
 * @self: a #OstreeRepo
 * @result: a #GAsyncResult
 * @out_csum: (out) (transfer full): A binary SHA256 checksum of the content object
 * @error: a #GError
 *
 * Completes an invocation of ostree_repo_write_content_async().
 */
gboolean
ostree_repo_write_content_finish (OstreeRepo        *self,
                                  GAsyncResult      *result,
                                  guchar           **out_csum,
                                  GError           **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  WriteContentAsyncData *data;

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == ostree_repo_write_content_async);

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  data = g_simple_async_result_get_op_res_gpointer (simple);
  ot_transfer_out_value (out_csum, &data->result_csum);
  return TRUE;
}

static GVariant *
create_empty_gvariant_dict (void)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE("a{sv}"));
  return g_variant_builder_end (&builder);
}

/**
 * ostree_repo_write_commit:
 * @self: Repo
 * @parent: (allow-none): ASCII SHA256 checksum for parent, or %NULL for none
 * @subject: Subject
 * @body: (allow-none): Body
 * @metadata: (allow-none): GVariant of type a{sv}, or %NULL for none
 * @root: The tree to point the commit to
 * @out_commit: (out): Resulting ASCII SHA256 checksum for commit
 * @cancellable: Cancellable
 * @error: Error
 *
 * Write a commit metadata object, referencing @root_contents_checksum
 * and @root_metadata_checksum.
 */
gboolean
ostree_repo_write_commit (OstreeRepo      *self,
                          const char      *parent,
                          const char      *subject,
                          const char      *body,
                          GVariant        *metadata,
                          OstreeRepoFile  *root,
                          char           **out_commit,
                          GCancellable    *cancellable,
                          GError         **error)
{
  gboolean ret = FALSE;
  gs_free char *ret_commit = NULL;
  gs_unref_variant GVariant *commit = NULL;
  gs_unref_variant GVariant *new_metadata = NULL;
  gs_free guchar *commit_csum = NULL;
  GDateTime *now = NULL;
  OstreeRepoFile *repo_root = OSTREE_REPO_FILE (root);

  g_return_val_if_fail (subject != NULL, FALSE);

  /* Add sizes information to our metadata object */
  if (!add_size_index_to_metadata (self, metadata, &new_metadata,
                                   cancellable, error))
    goto out;

  now = g_date_time_new_now_utc ();
  commit = g_variant_new ("(@a{sv}@ay@a(say)sst@ay@ay)",
                          new_metadata ? new_metadata : create_empty_gvariant_dict (),
                          parent ? ostree_checksum_to_bytes_v (parent) : ot_gvariant_new_bytearray (NULL, 0),
                          g_variant_new_array (G_VARIANT_TYPE ("(say)"), NULL, 0),
                          subject, body ? body : "",
                          GUINT64_TO_BE (g_date_time_to_unix (now)),
                          ostree_checksum_to_bytes_v (ostree_repo_file_tree_get_contents_checksum (repo_root)),
                          ostree_checksum_to_bytes_v (ostree_repo_file_tree_get_metadata_checksum (repo_root)));
  g_variant_ref_sink (commit);
  if (!ostree_repo_write_metadata (self, OSTREE_OBJECT_TYPE_COMMIT, NULL,
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

GFile *
_ostree_repo_get_commit_metadata_loose_path (OstreeRepo        *self,
                                             const char        *checksum)
{
  char buf[_OSTREE_LOOSE_PATH_MAX];
  _ostree_loose_path_with_suffix (buf, checksum, OSTREE_OBJECT_TYPE_COMMIT, self->mode,
                                  "meta");
  return g_file_resolve_relative_path (self->objects_dir, buf);
}

/**
 * ostree_repo_read_commit_detached_metadata:
 * @self: Repo
 * @checksum: ASCII SHA256 commit checksum
 * @out_metadata: (out) (transfer full): Metadata associated with commit in with format "a{sv}", or %NULL if none exists
 * @cancellable: Cancellable
 * @error: Error
 *
 * OSTree commits can have arbitrary metadata associated; this
 * function retrieves them.  If none exists, @out_metadata will be set
 * to %NULL.
 */
gboolean
ostree_repo_read_commit_detached_metadata (OstreeRepo      *self,
                                           const char      *checksum,
                                           GVariant       **out_metadata,
                                           GCancellable    *cancellable,
                                           GError         **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *metadata_path =
    _ostree_repo_get_commit_metadata_loose_path (self, checksum);
  gs_unref_variant GVariant *ret_metadata = NULL;
  GError *temp_error = NULL;
  
  if (!ot_util_variant_map (metadata_path, G_VARIANT_TYPE ("a{sv}"),
                            TRUE, &ret_metadata, &temp_error))
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_metadata, &ret_metadata);
 out:
  return ret;
}

/**
 * ostree_repo_write_commit_detached_metadata:
 * @self: Repo
 * @checksum: ASCII SHA256 commit checksum
 * @metadata: (allow-none): Metadata to associate with commit in with format "a{sv}", or %NULL to delete
 * @cancellable: Cancellable
 * @error: Error
 *
 * Replace any existing metadata associated with commit referred to by
 * @checksum with @metadata.  If @metadata is %NULL, then existing
 * data will be deleted.
 */
gboolean
ostree_repo_write_commit_detached_metadata (OstreeRepo      *self,
                                            const char      *checksum,
                                            GVariant        *metadata,
                                            GCancellable    *cancellable,
                                            GError         **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *metadata_path =
    _ostree_repo_get_commit_metadata_loose_path (self, checksum);
  gs_unref_variant GVariant *normalized = NULL;

  if (!_ostree_repo_ensure_loose_objdir_at (self->objects_dir_fd, checksum,
                                            cancellable, error))
    goto out;

  normalized = g_variant_get_normal_form (metadata);

  if (!g_file_replace_contents (metadata_path,
                                g_variant_get_data (normalized),
                                g_variant_get_size (normalized),
                                NULL, FALSE, 0, NULL,
                                cancellable, error))
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

struct OstreeRepoCommitModifier {
  volatile gint refcount;

  OstreeRepoCommitModifierFlags flags;
  OstreeRepoCommitFilter filter;
  gpointer user_data;
  GDestroyNotify destroy_notify;

  OstreeRepoCommitModifierXattrCallback xattr_callback;
  GDestroyNotify xattr_destroy;
  gpointer xattr_user_data;

  OstreeSePolicy *sepolicy;
};

OstreeRepoCommitFilterResult
_ostree_repo_commit_modifier_apply (OstreeRepo               *self,
                                    OstreeRepoCommitModifier *modifier,
                                    const char               *path,
                                    GFileInfo                *file_info,
                                    GFileInfo               **out_modified_info)
{
  OstreeRepoCommitFilterResult result;
  GFileInfo *modified_info;

  if (modifier == NULL || modifier->filter == NULL)
    {
      *out_modified_info = g_object_ref (file_info);
      return OSTREE_REPO_COMMIT_FILTER_ALLOW;
    }

  modified_info = g_file_info_dup (file_info);
  result = modifier->filter (self, path, modified_info, modifier->user_data);
  *out_modified_info = modified_info;

  return result;
}

static char *
ptrarray_path_join (GPtrArray  *path)
{
  GString *path_buf;

  path_buf = g_string_new ("");

  if (path->len == 0)
    g_string_append_c (path_buf, '/');
  else
    {
      guint i;
      for (i = 0; i < path->len; i++)
        {
          const char *elt = path->pdata[i];

          g_string_append_c (path_buf, '/');
          g_string_append (path_buf, elt);
        }
    }

  return g_string_free (path_buf, FALSE);
}

static gboolean
apply_commit_filter (OstreeRepo               *self,
                     OstreeRepoCommitModifier *modifier,
                     const char               *relpath,
                     GFileInfo                *file_info,
                     GFileInfo               **out_modified_info)
{
  if (modifier == NULL || modifier->filter == NULL)
    {
      *out_modified_info = g_object_ref (file_info);
      return OSTREE_REPO_COMMIT_FILTER_ALLOW;
    }

  return _ostree_repo_commit_modifier_apply (self, modifier, relpath, file_info, out_modified_info);
}

static gboolean
get_modified_xattrs (OstreeRepo                       *self,
                     OstreeRepoCommitModifier         *modifier,
                     const char                       *relpath,
                     GFileInfo                        *file_info,
                     GFile                            *path,
                     int                               dfd,
                     const char                       *dfd_subpath,
                     GVariant                        **out_xattrs,
                     GCancellable                     *cancellable,
                     GError                          **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *ret_xattrs = NULL;

  if (modifier && modifier->xattr_callback)
    {
      ret_xattrs = modifier->xattr_callback (self, relpath, file_info,
                                             modifier->xattr_user_data);
    }
  else if (!(modifier && (modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS) > 0))
    {
      if (path)
        {
          if (!gs_file_get_all_xattrs (path, &ret_xattrs, cancellable, error))
            goto out;
        }
      else if (dfd_subpath == NULL)
        {
          g_assert (dfd != -1);
          if (!gs_fd_get_all_xattrs (dfd, &ret_xattrs,
                                     cancellable, error))
            goto out;
        }
      else
        {
          g_assert (dfd != -1);
          if (!gs_dfd_and_name_get_all_xattrs (dfd, dfd_subpath, &ret_xattrs,
                                               cancellable, error))
            goto out;
        }
    }

  if (modifier && modifier->sepolicy)
    {
      gs_free char *label = NULL;

      if (!ostree_sepolicy_get_label (modifier->sepolicy, relpath,
                                      g_file_info_get_attribute_uint32 (file_info, "unix::mode"),
                                      &label, cancellable, error))
        goto out;

      if (label)
        {
          GVariantBuilder *builder;

          if (ret_xattrs)
            builder = ot_util_variant_builder_from_variant (ret_xattrs,
                                                            G_VARIANT_TYPE ("a(ayay)"));
          else
            builder = g_variant_builder_new (G_VARIANT_TYPE ("a(ayay)"));

          g_variant_builder_add_value (builder,
                                       g_variant_new ("(@ay@ay)",
                                                      g_variant_new_bytestring ("security.selinux"),
                                                      g_variant_new_bytestring (label)));
          if (ret_xattrs)
            g_variant_unref (ret_xattrs);

          ret_xattrs = g_variant_builder_end (builder);
          g_variant_ref_sink (ret_xattrs);
        }
    }
  
  ret = TRUE;
  gs_transfer_out_value (out_xattrs, &ret_xattrs);
 out:
  return ret;
}

static gboolean
write_directory_to_mtree_internal (OstreeRepo                  *self,
                                   GFile                       *dir,
                                   OstreeMutableTree           *mtree,
                                   OstreeRepoCommitModifier    *modifier,
                                   GPtrArray                   *path,
                                   GCancellable                *cancellable,
                                   GError                     **error);
static gboolean
write_dfd_iter_to_mtree_internal (OstreeRepo                  *self,
                                  GSDirFdIterator             *src_dfd_iter,
                                  OstreeMutableTree           *mtree,
                                  OstreeRepoCommitModifier    *modifier,
                                  GPtrArray                   *path,
                                  GCancellable                *cancellable,
                                  GError                     **error);

static gboolean
write_directory_content_to_mtree_internal (OstreeRepo                  *self,
                                           OstreeRepoFile              *repo_dir,
                                           GFileEnumerator             *dir_enum,
                                           GSDirFdIterator             *dfd_iter,
                                           GFileInfo                   *child_info,
                                           OstreeMutableTree           *mtree,
                                           OstreeRepoCommitModifier    *modifier,
                                           GPtrArray                   *path,
                                           GCancellable                *cancellable,
                                           GError                     **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *child = NULL;
  gs_unref_object GFileInfo *modified_info = NULL;
  gs_unref_object OstreeMutableTree *child_mtree = NULL;
  gs_free char *child_relpath = NULL;
  const char *name;
  GFileType file_type;
  OstreeRepoCommitFilterResult filter_result;

  g_assert (dir_enum != NULL || dfd_iter != NULL);

  name = g_file_info_get_name (child_info);
  g_ptr_array_add (path, (char*)name);

  if (modifier != NULL)
    child_relpath = ptrarray_path_join (path);

  filter_result = apply_commit_filter (self, modifier, child_relpath, child_info, &modified_info);

  if (filter_result != OSTREE_REPO_COMMIT_FILTER_ALLOW)
    {
      g_ptr_array_remove_index (path, path->len - 1);
      ret = TRUE;
      goto out;
    }

  file_type = g_file_info_get_file_type (child_info);
  switch (file_type)
    {
    case G_FILE_TYPE_DIRECTORY:
    case G_FILE_TYPE_SYMBOLIC_LINK:
    case G_FILE_TYPE_REGULAR:
      break;
    default:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unsupported file type: '%s'",
                   gs_file_get_path_cached (child));
      goto out;
    }

  if (dir_enum != NULL)
    child = g_file_enumerator_get_child (dir_enum, child_info);

  if (file_type == G_FILE_TYPE_DIRECTORY)
    {
      if (!ostree_mutable_tree_ensure_dir (mtree, name, &child_mtree, error))
        goto out;

      if (dir_enum != NULL)
        {
          if (!write_directory_to_mtree_internal (self, child, child_mtree,
                                                  modifier, path,
                                                  cancellable, error))
            goto out;
        }
      else
        {
          gs_dirfd_iterator_cleanup GSDirFdIterator child_dfd_iter = { 0, };

          if (!gs_dirfd_iterator_init_at (dfd_iter->fd, name, FALSE, &child_dfd_iter, error))
            goto out;

          if (!write_dfd_iter_to_mtree_internal (self, &child_dfd_iter, child_mtree,
                                                 modifier, path,
                                                 cancellable, error))
            goto out;
        }
    }
  else if (repo_dir)
    {
      g_assert (dir_enum != NULL);
      g_debug ("Adding: %s", gs_file_get_path_cached (child));
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
              if (child != NULL)
                {
                  file_input = (GInputStream*)g_file_read (child, cancellable, error);
                  if (!file_input)
                    goto out;
                }
              else
                {
                  if (!ot_openat_read_stream (dfd_iter->fd, name, FALSE,
                                              &file_input, cancellable, error))
                    goto out;
                }
            }

          if (!get_modified_xattrs (self, modifier,
                                    child_relpath, child_info, child, dfd_iter->fd, name,
                                    &xattrs,
                                    cancellable, error))
            goto out;

          if (!ostree_raw_file_to_content_stream (file_input,
                                                  modified_info, xattrs,
                                                  &file_object_input, &file_obj_length,
                                                  cancellable, error))
            goto out;
          if (!ostree_repo_write_content (self, NULL, file_object_input, file_obj_length,
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

  ret = TRUE;
 out:
  return ret;
}

static gboolean
write_directory_to_mtree_internal (OstreeRepo                  *self,
                                   GFile                       *dir,
                                   OstreeMutableTree           *mtree,
                                   OstreeRepoCommitModifier    *modifier,
                                   GPtrArray                   *path,
                                   GCancellable                *cancellable,
                                   GError                     **error)
{
  gboolean ret = FALSE;
  OstreeRepoCommitFilterResult filter_result;
  OstreeRepoFile *repo_dir = NULL;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFileInfo *child_info = NULL;

  if (dir)
    g_debug ("Examining: %s", gs_file_get_path_cached (dir));

  /* If the directory is already in the repository, we can try to
   * reuse checksums to skip checksumming. */
  if (dir && OSTREE_IS_REPO_FILE (dir) && modifier == NULL)
    repo_dir = (OstreeRepoFile *) dir;

  if (repo_dir)
    {
      if (!ostree_repo_file_ensure_resolved (repo_dir, error))
        goto out;

      ostree_mutable_tree_set_metadata_checksum (mtree, ostree_repo_file_tree_get_metadata_checksum (repo_dir));

      /* If the mtree was empty beforehand, the checksums on the mtree can simply
       * become the checksums on the tree in the repo. Super simple. */
      if (g_hash_table_size (ostree_mutable_tree_get_files (mtree)) == 0 &&
          g_hash_table_size (ostree_mutable_tree_get_subdirs (mtree)) == 0)
        {
          ostree_mutable_tree_set_contents_checksum (mtree, ostree_repo_file_tree_get_contents_checksum (repo_dir));
          ret = TRUE;
          goto out;
        }

      filter_result = OSTREE_REPO_COMMIT_FILTER_ALLOW;
    }
  else
    {
      gs_unref_object GFileInfo *modified_info = NULL;
      gs_unref_variant GVariant *xattrs = NULL;
      gs_free guchar *child_file_csum = NULL;
      gs_free char *tmp_checksum = NULL;
      gs_free char *relpath = NULL;

      child_info = g_file_query_info (dir, OSTREE_GIO_FAST_QUERYINFO,
                                      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                      cancellable, error);
      if (!child_info)
        goto out;

      if (modifier != NULL)
        relpath = ptrarray_path_join (path);

      filter_result = apply_commit_filter (self, modifier, relpath, child_info, &modified_info);

      if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
        {
          if (!get_modified_xattrs (self, modifier, relpath, child_info,
                                    dir, -1, NULL,
                                    &xattrs,
                                    cancellable, error))
            goto out;

          if (!_ostree_repo_write_directory_meta (self, modified_info, xattrs, &child_file_csum,
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
      gs_unref_object GFileEnumerator *dir_enum = NULL;

      dir_enum = g_file_enumerate_children ((GFile*)dir, OSTREE_GIO_FAST_QUERYINFO,
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            cancellable,
                                            error);
      if (!dir_enum)
        goto out;

      while (TRUE)
        {
          GFileInfo *child_info;
          
          if (!gs_file_enumerator_iterate (dir_enum, &child_info, NULL,
                                           cancellable, error))
            goto out;
          if (child_info == NULL)
            break;

          if (!write_directory_content_to_mtree_internal (self, repo_dir, dir_enum, NULL,
                                                          child_info,
                                                          mtree, modifier, path,
                                                          cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
write_dfd_iter_to_mtree_internal (OstreeRepo                  *self,
                                  GSDirFdIterator             *src_dfd_iter,
                                  OstreeMutableTree           *mtree,
                                  OstreeRepoCommitModifier    *modifier,
                                  GPtrArray                   *path,
                                  GCancellable                *cancellable,
                                  GError                     **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileInfo *child_info = NULL;
  gs_unref_object GFileInfo *modified_info = NULL;
  gs_unref_variant GVariant *xattrs = NULL;
  gs_free guchar *child_file_csum = NULL;
  gs_free char *tmp_checksum = NULL;
  gs_free char *relpath = NULL;
  OstreeRepoCommitFilterResult filter_result;
  struct stat dir_stbuf;

  if (fstat (src_dfd_iter->fd, &dir_stbuf) != 0)
    {
      gs_set_error_from_errno (error, errno);
      goto out;
    }

  child_info = _ostree_header_gfile_info_new (dir_stbuf.st_mode, dir_stbuf.st_uid, dir_stbuf.st_gid);

  if (modifier != NULL)
    {
      relpath = ptrarray_path_join (path);
      
      filter_result = apply_commit_filter (self, modifier, relpath, child_info, &modified_info);
    }
  else
    {
      filter_result = OSTREE_REPO_COMMIT_FILTER_ALLOW;
      modified_info = g_object_ref (child_info);
    }

  if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
    {
      if (!get_modified_xattrs (self, modifier, relpath, modified_info,
                                NULL, src_dfd_iter->fd, NULL,
                                &xattrs,
                                cancellable, error))
        goto out;

      if (!_ostree_repo_write_directory_meta (self, modified_info, xattrs, &child_file_csum,
                                              cancellable, error))
        goto out;

      g_free (tmp_checksum);
      tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
      ostree_mutable_tree_set_metadata_checksum (mtree, tmp_checksum);
    }

  if (filter_result != OSTREE_REPO_COMMIT_FILTER_ALLOW)
    {
      ret = TRUE;
      goto out;
    }

  while (TRUE)
    {
      struct dirent *dent;
      struct stat stbuf;
      gs_unref_object GFileInfo *child_info = NULL;

      if (!gs_dirfd_iterator_next_dent (src_dfd_iter, &dent, cancellable, error))
        goto out;

      if (dent == NULL)
        break;

      if (fstatat (src_dfd_iter->fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
        {
          gs_set_error_from_errno (error, errno);
          goto out;
        }

      child_info = _ostree_header_gfile_info_new (stbuf.st_mode, stbuf.st_uid, stbuf.st_gid);
      g_file_info_set_name (child_info, dent->d_name);

      if (S_ISREG (stbuf.st_mode))
        {
          g_file_info_set_size (child_info, stbuf.st_size);
        }
      else if (S_ISLNK (stbuf.st_mode))
        {
          if (!ot_readlinkat_gfile_info (src_dfd_iter->fd, dent->d_name,
                                         child_info, cancellable, error))
            goto out;
        }
      else if (S_ISDIR (stbuf.st_mode))
        ;
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Not a regular file or symlink: %s",
                       dent->d_name);
          goto out;
        }

      if (!write_directory_content_to_mtree_internal (self, NULL, NULL, src_dfd_iter,
                                                      child_info,
                                                      mtree, modifier, path,
                                                      cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_write_directory_to_mtree:
 * @self: Repo
 * @dir: Path to a directory
 * @mtree: Overlay directory contents into this tree
 * @modifier: (allow-none): Optional modifier
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store objects for @dir and all children into the repository @self,
 * overlaying the resulting filesystem hierarchy into @mtree.
 */
gboolean
ostree_repo_write_directory_to_mtree (OstreeRepo                *self,
                                      GFile                     *dir,
                                      OstreeMutableTree         *mtree,
                                      OstreeRepoCommitModifier  *modifier,
                                      GCancellable              *cancellable,
                                      GError                   **error)
{
  gboolean ret = FALSE;
  GPtrArray *path = NULL;

  if (modifier && modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_GENERATE_SIZES)
    {
      self->generate_sizes = TRUE;
    }

  path = g_ptr_array_new ();
  if (g_file_is_native (dir))
    {
      gs_dirfd_iterator_cleanup GSDirFdIterator dfd_iter = { 0, };

      if (!gs_dirfd_iterator_init_at (AT_FDCWD, gs_file_get_path_cached (dir), FALSE,
                                      &dfd_iter, error))
        goto out;

      if (!write_dfd_iter_to_mtree_internal (self, &dfd_iter, mtree, modifier, path,
                                             cancellable, error))
        goto out;
    }
  else
    {
      if (!write_directory_to_mtree_internal (self, dir, mtree, modifier, path,
                                              cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  if (path)
    g_ptr_array_free (path, TRUE);
  return ret;
}

/**
 * ostree_repo_write_mtree:
 * @self: Repo
 * @mtree: Mutable tree
 * @out_file: (out): An #OstreeRepoFile representing @mtree's root.
 * @cancellable: Cancellable
 * @error: Error
 *
 * Write all metadata objects for @mtree to repo; the resulting
 * @out_file points to the %OSTREE_OBJECT_TYPE_DIR_TREE object that
 * the @mtree represented.
 */
gboolean
ostree_repo_write_mtree (OstreeRepo           *self,
                         OstreeMutableTree    *mtree,
                         GFile               **out_file,
                         GCancellable         *cancellable,
                         GError              **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  const char *contents_checksum, *metadata_checksum;
  gs_unref_object GFile *ret_file = NULL;

  metadata_checksum = ostree_mutable_tree_get_metadata_checksum (mtree);
  if (!metadata_checksum)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't commit an empty tree");
      goto out;
    }

  contents_checksum = ostree_mutable_tree_get_contents_checksum (mtree);
  if (contents_checksum)
    {
      ret_file = G_FILE (_ostree_repo_file_new_root (self, contents_checksum, metadata_checksum));
    }
  else
    {
      gs_unref_hashtable GHashTable *dir_metadata_checksums = NULL;
      gs_unref_hashtable GHashTable *dir_contents_checksums = NULL;
      gs_unref_variant GVariant *serialized_tree = NULL;
      gs_free guchar *contents_csum = NULL;
      char contents_checksum_buf[65];

      dir_contents_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free, (GDestroyNotify)g_free);
      dir_metadata_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free, (GDestroyNotify)g_free);

      g_hash_table_iter_init (&hash_iter, ostree_mutable_tree_get_subdirs (mtree));
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *name = key;
          gs_unref_object GFile *child_file = NULL;
          OstreeMutableTree *child_dir = value;

          if (!ostree_repo_write_mtree (self, child_dir, &child_file,
                                        cancellable, error))
            goto out;

          g_hash_table_replace (dir_contents_checksums, g_strdup (name),
                                g_strdup (ostree_repo_file_tree_get_contents_checksum (OSTREE_REPO_FILE (child_file))));
          g_hash_table_replace (dir_metadata_checksums, g_strdup (name),
                                g_strdup (ostree_repo_file_tree_get_metadata_checksum (OSTREE_REPO_FILE (child_file))));
        }

      serialized_tree = create_tree_variant_from_hashes (ostree_mutable_tree_get_files (mtree),
                                                         dir_contents_checksums,
                                                         dir_metadata_checksums);

      if (!ostree_repo_write_metadata (self, OSTREE_OBJECT_TYPE_DIR_TREE, NULL,
                                       serialized_tree, &contents_csum,
                                       cancellable, error))
        goto out;

      ostree_checksum_inplace_from_bytes (contents_csum, contents_checksum_buf);
      ostree_mutable_tree_set_contents_checksum (mtree, contents_checksum_buf);

      ret_file = G_FILE (_ostree_repo_file_new_root (self, contents_checksum_buf, metadata_checksum));
    }

  ret = TRUE;
  ot_transfer_out_value (out_file, &ret_file);
 out:
  return ret;
}

/**
 * ostree_repo_commit_modifier_new:
 * @flags: Control options for filter
 * @commit_filter: (allow-none): Function that can inspect individual files
 * @user_data: (allow-none): User data
 * @destroy_notify: A #GDestroyNotify
 *
 * Returns: (transfer full): A new commit modifier.
 */
OstreeRepoCommitModifier *
ostree_repo_commit_modifier_new (OstreeRepoCommitModifierFlags  flags,
                                 OstreeRepoCommitFilter         commit_filter,
                                 gpointer                       user_data,
                                 GDestroyNotify                 destroy_notify)
{
  OstreeRepoCommitModifier *modifier = g_new0 (OstreeRepoCommitModifier, 1);

  modifier->refcount = 1;
  modifier->flags = flags;
  modifier->filter = commit_filter;
  modifier->user_data = user_data;
  modifier->destroy_notify = destroy_notify;

  return modifier;
}

OstreeRepoCommitModifier *
ostree_repo_commit_modifier_ref (OstreeRepoCommitModifier *modifier)
{
  g_atomic_int_inc (&modifier->refcount);
  return modifier;
}

void
ostree_repo_commit_modifier_unref (OstreeRepoCommitModifier *modifier)
{
  if (!modifier)
    return;
  if (!g_atomic_int_dec_and_test (&modifier->refcount))
    return;

  if (modifier->destroy_notify)
    modifier->destroy_notify (modifier->user_data);

  if (modifier->xattr_destroy)
    modifier->xattr_destroy (modifier->xattr_user_data);

  g_clear_object (&modifier->sepolicy);

  g_free (modifier);
  return;
}

/**
 * ostree_repo_commit_modifier_set_xattr_callback:
 * @modifier: An #OstreeRepoCommitModifier
 * @callback: Function to be invoked, should return extended attributes for path
 * @destroy: Destroy notification
 * @user_data: Data for @callback:
 *
 * If set, this function should return extended attributes to use for
 * the given path.  This is useful for things like ACLs and SELinux,
 * where a build system can label the files as it's committing to the
 * repository.
 */
void
ostree_repo_commit_modifier_set_xattr_callback (OstreeRepoCommitModifier  *modifier,
                                                OstreeRepoCommitModifierXattrCallback  callback,
                                                GDestroyNotify                         destroy,
                                                gpointer                               user_data)
{
  modifier->xattr_callback = callback;
  modifier->xattr_destroy = destroy;
  modifier->xattr_user_data = user_data;
}

/**
 * ostree_repo_commit_modifier_set_sepolicy:
 * @modifier: An #OstreeRepoCommitModifier
 * @sepolicy: (allow-none): Policy to use for labeling
 *
 * If @policy is non-%NULL, use it to look up labels to use for
 * "security.selinux" extended attributes.
 *
 * Note that any policy specified this way operates in addition to any
 * extended attributes provided via
 * ostree_repo_commit_modifier_set_xattr_callback().  However if both
 * specify a value for "security.selinux", then the one from the
 * policy wins.
 */
void
ostree_repo_commit_modifier_set_sepolicy (OstreeRepoCommitModifier              *modifier,
                                          OstreeSePolicy                        *sepolicy)
{
  g_clear_object (&modifier->sepolicy);
  modifier->sepolicy = sepolicy ? g_object_ref (sepolicy) : NULL;
}

G_DEFINE_BOXED_TYPE(OstreeRepoCommitModifier, ostree_repo_commit_modifier,
                    ostree_repo_commit_modifier_ref,
                    ostree_repo_commit_modifier_unref);

static OstreeRepoTransactionStats *
ostree_repo_transaction_stats_copy (OstreeRepoTransactionStats *stats)
{
  return g_memdup (stats, sizeof (OstreeRepoTransactionStats));
}

static void
ostree_repo_transaction_stats_free (OstreeRepoTransactionStats *stats)
{
  return g_free (stats);
}

G_DEFINE_BOXED_TYPE(OstreeRepoTransactionStats, ostree_repo_transaction_stats,
                    ostree_repo_transaction_stats_copy,
                    ostree_repo_transaction_stats_free);
