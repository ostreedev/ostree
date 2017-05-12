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
#include <gio/gunixoutputstream.h>
#include "otutil.h"

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-file-enumerator.h"
#include "ostree-checksum-input-stream.h"
#include "ostree-mutable-tree.h"
#include "ostree-varint.h"
#include <sys/xattr.h>
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
      if (G_UNLIKELY (errno != EEXIST))
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }
  return TRUE;
}

static GVariant *
create_file_metadata (guint32       uid,
                      guint32       gid,
                      guint32       mode,
                      GVariant     *xattrs)
{
  GVariant *ret_metadata = NULL;
  g_autoptr(GVariant) tmp_xattrs = NULL;

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
  g_autoptr(GVariant) filemeta = NULL;
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
      glnx_set_error_from_errno (error);
      g_prefix_error (error, "Unable to set xattr: ");
      return FALSE;
    }

  return TRUE;
}

/* See https://github.com/ostreedev/ostree/pull/698 */
#ifdef WITH_SMACK
#define XATTR_NAME_SMACK "security.SMACK64"
#endif

static void
ot_security_smack_reset_dfd_name (int dfd, const char *name)
{
#ifdef WITH_SMACK
  char buf[PATH_MAX];
  /* See glnx-xattrs.c */
  snprintf (buf, sizeof (buf), "/proc/self/fd/%d/%s", dfd, name);
  (void) lremovexattr (buf, XATTR_NAME_SMACK);
#endif
}

static void
ot_security_smack_reset_fd (int fd)
{
#ifdef WITH_SMACK
  (void) fremovexattr (fd, XATTR_NAME_SMACK);
#endif
}

gboolean
_ostree_repo_commit_loose_final (OstreeRepo        *self,
                                 const char        *checksum,
                                 OstreeObjectType   objtype,
                                 int                temp_dfd,
                                 int                fd,
                                 const char        *temp_filename,
                                 GCancellable      *cancellable,
                                 GError           **error)
{
  int dest_dfd;
  char tmpbuf[_OSTREE_LOOSE_PATH_MAX];

  _ostree_loose_path (tmpbuf, checksum, objtype, self->mode);

  if (self->in_transaction)
    dest_dfd = self->commit_stagedir_fd;
  else
    dest_dfd = self->objects_dir_fd;

  if (!_ostree_repo_ensure_loose_objdir_at (dest_dfd, tmpbuf,
                                            cancellable, error))
    return FALSE;

  if (fd != -1)
    {
      if (!glnx_link_tmpfile_at (temp_dfd, GLNX_LINK_TMPFILE_NOREPLACE_IGNORE_EXIST,
                                 fd, temp_filename, dest_dfd, tmpbuf, error))
        return FALSE;
    }
  else
    {
      if (G_UNLIKELY (renameat (temp_dfd, temp_filename,
                                dest_dfd, tmpbuf) == -1))
        {
          if (errno != EEXIST)
            return glnx_throw_errno_prefix (error, "Storing file '%s'", temp_filename);
          else
            (void) unlinkat (temp_dfd, temp_filename, 0);
        }
    }

  return TRUE;
}

static gboolean
commit_loose_object_trusted (OstreeRepo        *self,
                             const char        *checksum,
                             OstreeObjectType   objtype,
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
      if (fd != -1)
        {
          if (fchown (fd, self->target_owner_uid, self->target_owner_gid) < 0)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
      else if (G_UNLIKELY (fchownat (self->tmp_dir_fd, temp_filename,
                                     self->target_owner_uid,
                                     self->target_owner_gid,
                                     AT_SYMLINK_NOFOLLOW) == -1))
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  /* Special handling for symlinks in bare repositories */
  if (object_is_symlink && self->mode == OSTREE_REPO_MODE_BARE_USER_ONLY)
    {
      /* We don't store the metadata in bare-user-only, so we're done. */
    }
  else if (object_is_symlink && self->mode == OSTREE_REPO_MODE_BARE)
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
          glnx_set_error_from_errno (error);
          goto out;
        }

      if (xattrs != NULL)
        {
          ot_security_smack_reset_dfd_name (self->tmp_dir_fd, temp_filename);
          if (!glnx_dfd_name_set_all_xattrs (self->tmp_dir_fd, temp_filename,
                                               xattrs, cancellable, error))
            goto out;
        }
    }
  else
    {
      int res;

      if (objtype == OSTREE_OBJECT_TYPE_FILE && self->mode == OSTREE_REPO_MODE_BARE)
        {
          do
            res = fchown (fd, uid, gid);
          while (G_UNLIKELY (res == -1 && errno == EINTR));
          if (G_UNLIKELY (res == -1))
            {
              glnx_set_error_from_errno (error);
              goto out;
            }

          do
            res = fchmod (fd, mode);
          while (G_UNLIKELY (res == -1 && errno == EINTR));
          if (G_UNLIKELY (res == -1))
            {
              glnx_set_error_from_errno (error);
              goto out;
            }

          if (xattrs)
            {
              ot_security_smack_reset_fd (fd);
              if (!glnx_fd_set_all_xattrs (fd, xattrs, cancellable, error))
                goto out;
            }
        }

      if (objtype == OSTREE_OBJECT_TYPE_FILE &&
          (self->mode == OSTREE_REPO_MODE_BARE_USER || self->mode == OSTREE_REPO_MODE_BARE_USER_ONLY))
        {
          if (!object_is_symlink)
            {
              /* We need to apply at least some mode bits, because the repo file was created
                 with mode 644, and we need e.g. exec bits to be right when we do a user-mode
                 checkout. To make this work we apply all user bits and the read bits for
                 group/other.  Furthermore, setting user xattrs requires write access, so
                 this makes sure it's at least writable by us.  (O_TMPFILE uses mode 0 by default) */
              if (fchmod (fd, mode | 0744) < 0)
                return glnx_throw_errno (error);
            }

          if (self->mode == OSTREE_REPO_MODE_BARE_USER &&
              !write_file_metadata_to_xattr (fd, uid, gid, mode, xattrs, error))
            return FALSE;
        }

      if (objtype == OSTREE_OBJECT_TYPE_FILE && _ostree_repo_mode_is_bare (self->mode))
        {
          /* To satisfy tools such as guile which compare mtimes
           * to determine whether or not source files need to be compiled,
           * set the modification time to OSTREE_TIMESTAMP.
           */
          const struct timespec times[2] = { { OSTREE_TIMESTAMP, UTIME_OMIT }, { OSTREE_TIMESTAMP, 0} };
          do
            res = futimens (fd, times);
          while (G_UNLIKELY (res == -1 && errno == EINTR));
          if (G_UNLIKELY (res == -1))
            {
              glnx_set_error_from_errno (error);
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
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
    }

  if (!_ostree_repo_commit_loose_final (self, checksum, objtype,
                                        self->tmp_dir_fd, fd, temp_filename,
                                        cancellable, error))
    goto out;
  
  ret = TRUE;
 out:
  if (G_UNLIKELY (error && *error))
    g_prefix_error (error, "Writing object %s.%s: ", checksum, ostree_object_type_to_string (objtype));
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
  g_autoptr(GVariantBuilder) builder = NULL;
    
  /* original_metadata may be NULL */
  builder = ot_util_variant_builder_from_variant (original_metadata, G_VARIANT_TYPE ("a{sv}"));

  if (self->object_sizes &&
      g_hash_table_size (self->object_sizes) > 0)
    {
      GHashTableIter entries = { 0 };
      gchar *e_checksum = NULL;
      OstreeContentSizeCacheEntry *e_size = NULL;
      GVariantBuilder index_builder;
      guint i;
      g_autoptr(GPtrArray) sorted_keys = NULL;
      
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
          guint8 csum[OSTREE_SHA256_DIGEST_LEN];
          const char *e_checksum = sorted_keys->pdata[i];
          g_autoptr(GString) buffer = g_string_new (NULL);

          ostree_checksum_inplace_to_bytes (e_checksum, csum);
          g_string_append_len (buffer, (char*)csum, sizeof (csum));

          e_size = g_hash_table_lookup (self->object_sizes, e_checksum);
          _ostree_write_varuint64 (buffer, e_size->archived);
          _ostree_write_varuint64 (buffer, e_size->unpacked);

          g_variant_builder_add (&index_builder, "@ay",
                                 ot_gvariant_new_bytearray ((guint8*)buffer->str, buffer->len));
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
ot_fallocate (int fd, goffset size, GError **error)
{
  if (size == 0)
    return TRUE;

  int r = posix_fallocate (fd, 0, size);
  if (r != 0)
    {
      /* posix_fallocate is a weird deviation from errno standards */
      errno = r;
      return glnx_throw_errno_prefix (error, "fallocate");
    }
  return TRUE;
}

gboolean
_ostree_repo_open_content_bare (OstreeRepo          *self,
                                const char          *checksum,
                                guint64              content_len,
                                OstreeRepoContentBareCommit *out_state,
                                GOutputStream      **out_stream,
                                gboolean            *out_have_object,
                                GCancellable        *cancellable,
                                GError             **error)
{
  gboolean ret = FALSE;
  g_autofree char *temp_filename = NULL;
  g_autoptr(GOutputStream) ret_stream = NULL;
  gboolean have_obj;

  if (!_ostree_repo_has_loose_object (self, checksum, OSTREE_OBJECT_TYPE_FILE, &have_obj,
                                      cancellable, error))
    goto out;

  if (!have_obj)
    {
      int fd;

      if (!glnx_open_tmpfile_linkable_at (self->tmp_dir_fd, ".", O_WRONLY|O_CLOEXEC,
                                          &fd, &temp_filename, error))
        goto out;

      if (!ot_fallocate (fd, content_len, error))
        goto out;

      ret_stream = g_unix_output_stream_new (fd, TRUE);
    }

  ret = TRUE;
  if (!have_obj)
    {
      out_state->temp_filename = temp_filename;
      temp_filename = NULL;
      out_state->fd = g_file_descriptor_based_get_fd ((GFileDescriptorBased*)ret_stream);
      if (out_stream)
        *out_stream = g_steal_pointer (&ret_stream);
    }
  *out_have_object = have_obj;
 out:
  return ret;
}

gboolean
_ostree_repo_commit_trusted_content_bare (OstreeRepo          *self,
                                          const char          *checksum,
                                          OstreeRepoContentBareCommit *state,
                                          guint32              uid,
                                          guint32              gid,
                                          guint32              mode,
                                          GVariant            *xattrs,
                                          GCancellable        *cancellable,
                                          GError             **error)
{
  gboolean ret = FALSE;

  if (state->fd != -1)
    {
      if (!commit_loose_object_trusted (self, checksum, OSTREE_OBJECT_TYPE_FILE,
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
create_regular_tmpfile_linkable_with_content (OstreeRepo *self,
                                              guint64 length,
                                              GInputStream *input,
                                              int *out_fd,
                                              char **out_path,
                                              GCancellable *cancellable,
                                              GError **error)
{
  glnx_fd_close int temp_fd = -1;
  g_autofree char *temp_filename = NULL;

  if (!glnx_open_tmpfile_linkable_at (self->tmp_dir_fd, ".", O_WRONLY|O_CLOEXEC,
                                      &temp_fd, &temp_filename,
                                      error))
    return FALSE;

  if (!ot_fallocate (temp_fd, length, error))
    return FALSE;

  if (G_IS_FILE_DESCRIPTOR_BASED (input))
    {
      int infd = g_file_descriptor_based_get_fd ((GFileDescriptorBased*) input);
      if (glnx_regfile_copy_bytes (infd, temp_fd, (off_t)length, TRUE) < 0)
        return glnx_throw_errno_prefix (error, "regfile copy");
    }
  else
    {
      g_autoptr(GOutputStream) temp_out = g_unix_output_stream_new (temp_fd, FALSE);
      if (g_output_stream_splice (temp_out, input, 0,
                                  cancellable, error) < 0)
        return FALSE;
    }

  if (fchmod (temp_fd, 0644) < 0)
    return glnx_throw_errno_prefix (error, "fchmod");

  *out_fd = temp_fd; temp_fd = -1;
  *out_path = g_steal_pointer (&temp_filename);
  return TRUE;
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
  const char *actual_checksum = NULL;
  g_autofree char *actual_checksum_owned = NULL;
  gboolean do_commit;
  OstreeRepoMode repo_mode;
  g_autofree char *temp_filename = NULL;
  g_autofree guchar *ret_csum = NULL;
  glnx_unref_object OtChecksumInstream *checksum_input = NULL;
  g_autoptr(GInputStream) file_input = NULL;
  g_autoptr(GFileInfo) file_info = NULL;
  g_autoptr(GVariant) xattrs = NULL;
  gboolean have_obj;
  gboolean temp_file_is_regular;
  gboolean temp_file_is_symlink;
  glnx_fd_close int temp_fd = -1;
  gboolean object_is_symlink = FALSE;
  gssize unpacked_size = 0;
  gboolean indexable = FALSE;

  g_return_val_if_fail (expected_checksum || out_csum, FALSE);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (expected_checksum)
    {
      if (!_ostree_repo_has_loose_object (self, expected_checksum, objtype, &have_obj,
                                          cancellable, error))
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
    checksum_input = ot_checksum_instream_new (input, G_CHECKSUM_SHA256);

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
          g_autoptr(GBytes) target = g_bytes_new (target_str, strlen (target_str) + 1);

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
      if ((_ostree_repo_mode_is_bare (repo_mode)) && temp_file_is_regular)
        {
          guint64 size = g_file_info_get_size (file_info);

          if (!create_regular_tmpfile_linkable_with_content (self, size, file_input,
                                                             &temp_fd, &temp_filename,
                                                             cancellable, error))
            goto out;
        }
      else if (_ostree_repo_mode_is_bare (repo_mode) && temp_file_is_symlink)
        {
          /* Note: This will not be hit for bare-user mode because its converted to a
             regular file and take the branch above */
          if (!_ostree_make_temporary_symlink_at (self->tmp_dir_fd,
                                                  g_file_info_get_symlink_target (file_info),
                                                  &temp_filename,
                                                  cancellable, error))
            goto out;
        }
      else if (repo_mode == OSTREE_REPO_MODE_ARCHIVE_Z2)
        {
          g_autoptr(GVariant) file_meta = NULL;
          g_autoptr(GConverter) zlib_compressor = NULL;
          g_autoptr(GOutputStream) compressed_out_stream = NULL;
          g_autoptr(GOutputStream) temp_out = NULL;

          if (self->generate_sizes)
            indexable = TRUE;

          if (!glnx_open_tmpfile_linkable_at (self->tmp_dir_fd, ".", O_WRONLY|O_CLOEXEC,
                                              &temp_fd, &temp_filename,
                                              error))
            goto out;
          temp_file_is_regular = TRUE;
          temp_out = g_unix_output_stream_new (temp_fd, FALSE);

          file_meta = _ostree_zlib_file_header_new (file_info, xattrs);

          if (!_ostree_write_variant_with_size (temp_out, file_meta, 0, NULL, NULL,
                                                cancellable, error))
            goto out;

          if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
            {
              zlib_compressor = (GConverter*)g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW, self->zlib_compression_level);
              compressed_out_stream = g_converter_output_stream_new (temp_out, zlib_compressor);
              /* Don't close the base; we'll do that later */
              g_filter_output_stream_set_close_base_stream ((GFilterOutputStream*)compressed_out_stream, FALSE);
              
              unpacked_size = g_output_stream_splice (compressed_out_stream, file_input,
                                                      0, cancellable, error);
              if (unpacked_size < 0)
                goto out;
            }

          if (!g_output_stream_flush (temp_out, cancellable, error))
            goto out;

          if (fchmod (temp_fd, 0644) < 0)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
      else
        g_assert_not_reached ();
    }
  else
    {
      if (!create_regular_tmpfile_linkable_with_content (self, file_object_length,
                                                         checksum_input ? (GInputStream*)checksum_input : input,
                                                         &temp_fd, &temp_filename,
                                                         cancellable, error))
        goto out;
      temp_file_is_regular = TRUE;
    }

  if (!checksum_input)
    actual_checksum = expected_checksum;
  else
    {
      actual_checksum = actual_checksum_owned = ot_checksum_instream_get_string (checksum_input);
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

      if (fstat (temp_fd, &stbuf) == -1)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }

      repo_store_size_entry (self, actual_checksum, unpacked_size, stbuf.st_size);
    }

  if (!_ostree_repo_has_loose_object (self, actual_checksum, objtype, &have_obj,
                                      cancellable, error))
    goto out;
          
  do_commit = !have_obj;

  if (do_commit)
    {
      guint32 uid, gid, mode;

      if (file_info)
        {
          uid = g_file_info_get_attribute_uint32 (file_info, "unix::uid");
          gid = g_file_info_get_attribute_uint32 (file_info, "unix::gid");
          mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
        }
      else
        uid = gid = mode = 0;
      
      if (!commit_loose_object_trusted (self, actual_checksum, objtype,
                                        temp_filename,
                                        object_is_symlink,
                                        uid, gid, mode,
                                        xattrs, temp_fd,
                                        cancellable, error))
        goto out;


      if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
        {
          GError *local_error = NULL;
          /* If we are writing a commit, be sure there is no tombstone for it.
             We may have deleted the commit and now we are trying to pull it again.  */
          if (!ostree_repo_delete_object (self,
                                          OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT,
                                          actual_checksum,
                                          cancellable,
                                          &local_error))
            {
              if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                g_clear_error (&local_error);
              else
                {
                  g_propagate_error (error, local_error);
                  goto out;
                }
            }
        }

      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        {
          if (G_UNLIKELY (file_object_length > OSTREE_MAX_METADATA_WARN_SIZE))
            {
              g_autofree char *metasize = g_format_size (file_object_length);
              g_autofree char *warnsize = g_format_size (OSTREE_MAX_METADATA_WARN_SIZE);
              g_autofree char *maxsize = g_format_size (OSTREE_MAX_METADATA_SIZE);
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

  if (checksum_input)
    {
      g_assert (actual_checksum);
      ret_csum = ostree_checksum_to_bytes (actual_checksum);
    }

  ret = TRUE;
  ot_transfer_out_value(out_csum, &ret_csum);
 out:
  if (temp_filename)
    (void) unlinkat (self->tmp_dir_fd, temp_filename, 0);
  return ret;
}

static gboolean
scan_one_loose_devino (OstreeRepo                     *self,
                       int                             object_dir_fd,
                       GHashTable                     *devino_cache,
                       GCancellable                   *cancellable,
                       GError                        **error)
{
  gboolean ret = FALSE;
  int res;
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

  if (!glnx_dirfd_iterator_init_at (object_dir_fd, ".", FALSE,
                                    &dfd_iter, error))
    goto out;
  
  while (TRUE)
    {
      struct dirent *dent;
      g_auto(GLnxDirFdIterator) child_dfd_iter = { 0, };

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        goto out;
          
      if (dent == NULL)
        break;

      /* All object directories only have two character entries */
      if (strlen (dent->d_name) != 2)
        continue;

      if (!glnx_dirfd_iterator_init_at (dfd_iter.fd, dent->d_name, FALSE,
                                        &child_dfd_iter, error))
        goto out;

      while (TRUE)
        {
          struct stat stbuf;
          OstreeDevIno *key;
          struct dirent *child_dent;
          const char *dot;
          gboolean skip;
          const char *name;

          if (!glnx_dirfd_iterator_next_dent (&child_dfd_iter, &child_dent, cancellable, error))
            goto out;
          
          if (child_dent == NULL)
            break;

          name = child_dent->d_name;

          switch (self->mode)
            {
            case OSTREE_REPO_MODE_ARCHIVE_Z2:
            case OSTREE_REPO_MODE_BARE:
            case OSTREE_REPO_MODE_BARE_USER:
            case OSTREE_REPO_MODE_BARE_USER_ONLY:
              skip = !g_str_has_suffix (name, ".file");
              break;
            default:
              g_assert_not_reached ();
            }
          if (skip)
            continue;

          dot = strrchr (name, '.');
          g_assert (dot);
          
          /* Skip anything that doesn't look like a 64 character checksum */
          if ((dot - name) != 62)
            continue;

          do
            res = fstatat (child_dfd_iter.fd, child_dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW);
          while (G_UNLIKELY (res == -1 && errno == EINTR));
          if (res == -1)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }

          key = g_new (OstreeDevIno, 1);
          key->dev = stbuf.st_dev;
          key->ino = stbuf.st_ino;
          memcpy (key->checksum, dent->d_name, 2);
          memcpy (key->checksum + 2, name, 62);
          key->checksum[sizeof(key->checksum)-1] = '\0';
          
          g_hash_table_add (devino_cache, key);
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
scan_loose_devino (OstreeRepo                     *self,
                   GHashTable                     *devino_cache,
                   GCancellable                   *cancellable,
                   GError                        **error)
{
  gboolean ret = FALSE;

  if (self->parent_repo)
    {
      if (!scan_loose_devino (self->parent_repo, devino_cache, cancellable, error))
        goto out;
    }

  if (self->mode == OSTREE_REPO_MODE_ARCHIVE_Z2)
    {
      if (!scan_one_loose_devino (self, self->uncompressed_objects_dir_fd, devino_cache,
                                  cancellable, error))
        goto out;
    }

  if (!scan_one_loose_devino (self, self->objects_dir_fd,
                              devino_cache, cancellable, error))
    goto out;
    
  ret = TRUE;
 out:
  return ret;
}

static const char *
devino_cache_lookup (OstreeRepo           *self,
                     OstreeRepoCommitModifier *modifier,
                     guint32               device,
                     guint32               inode)
{
  OstreeDevIno dev_ino_key;
  OstreeDevIno *dev_ino_val;
  GHashTable *cache;

  if (self->loose_object_devino_hash)
    cache = self->loose_object_devino_hash;
  else if (modifier && modifier->devino_cache)
    cache = modifier->devino_cache;
  else
    return NULL;

  dev_ino_key.dev = device;
  dev_ino_key.ino = inode;
  dev_ino_val = g_hash_table_lookup (cache, &dev_ino_key);
  if (!dev_ino_val)
    return NULL;
  return dev_ino_val->checksum;
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
    self->loose_object_devino_hash = (GHashTable*)ostree_repo_devino_cache_new ();
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
 * is resuming from a previous one.  This is a legacy state, now OSTree
 * pulls use per-commit `state/.commitpartial` files.
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

  g_return_val_if_fail (self->in_transaction == FALSE, FALSE);

  memset (&self->txn_stats, 0, sizeof (OstreeRepoTransactionStats));

  self->in_transaction = TRUE;

  gboolean ret_transaction_resume = FALSE;
  if (!_ostree_repo_allocate_tmpdir (self->tmp_dir_fd,
                                     self->stagedir_prefix,
                                     &self->commit_stagedir_name,
                                     &self->commit_stagedir_fd,
                                     &self->commit_stagedir_lock,
                                     &ret_transaction_resume,
                                     cancellable, error))
    return FALSE;

  if (out_transaction_resume)
    *out_transaction_resume = ret_transaction_resume;
  return TRUE;
}

static gboolean
rename_pending_loose_objects (OstreeRepo        *self,
                              GCancellable      *cancellable,
                              GError           **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

  if (!glnx_dirfd_iterator_init_at (self->commit_stagedir_fd, ".", FALSE, &dfd_iter, error))
    return FALSE;

  /* Iterate over the outer checksum dir */
  while (TRUE)
    {
      struct dirent *dent;
      g_auto(GLnxDirFdIterator) child_dfd_iter = { 0, };

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (dent->d_type != DT_DIR)
        continue;

      /* All object directories only have two character entries */
      if (strlen (dent->d_name) != 2)
        continue;

      if (!glnx_dirfd_iterator_init_at (dfd_iter.fd, dent->d_name, FALSE,
                                        &child_dfd_iter, error))
        return FALSE;

      /* Iterate over inner checksum dir */
      while (TRUE)
        {
          struct dirent *child_dent;
          char loose_objpath[_OSTREE_LOOSE_PATH_MAX];

          if (!glnx_dirfd_iterator_next_dent (&child_dfd_iter, &child_dent, cancellable, error))
            return FALSE;
          if (child_dent == NULL)
            break;

          loose_objpath[0] = dent->d_name[0];
          loose_objpath[1] = dent->d_name[1];
          loose_objpath[2] = '/';

          g_strlcpy (loose_objpath + 3, child_dent->d_name, sizeof (loose_objpath)-3);

          if (!_ostree_repo_ensure_loose_objdir_at (self->objects_dir_fd, loose_objpath,
                                                    cancellable, error))
            return FALSE;

          if (G_UNLIKELY (renameat (child_dfd_iter.fd, loose_objpath + 3,
                                    self->objects_dir_fd, loose_objpath) < 0))
            return glnx_throw_errno (error);
        }
    }

  if (!glnx_shutil_rm_rf_at (self->tmp_dir_fd, self->commit_stagedir_name,
                             cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
cleanup_tmpdir (OstreeRepo        *self,
                GCancellable      *cancellable,
                GError           **error)
{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  guint64 curtime_secs;

  curtime_secs = g_get_real_time () / 1000000;

  if (!glnx_dirfd_iterator_init_at (self->tmp_dir_fd, ".", TRUE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      guint64 delta;
      struct dirent *dent;
      struct stat stbuf;
      g_auto(GLnxLockFile) lockfile = GLNX_LOCK_FILE_INIT;
      gboolean did_lock;

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;

      if (dent == NULL)
        break;

      /* Special case this; we create it when opening, and don't want
       * to blow it away.
       */
      if (strcmp (dent->d_name, "cache") == 0)
        continue;

      if (TEMP_FAILURE_RETRY (fstatat (dfd_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW)) < 0)
        {
          if (errno == ENOENT) /* Did another cleanup win? */
            continue;
          return glnx_throw_errno (error);
        }

      /* First, if it's a directory which needs locking, but it's
       * busy, skip it.
       */
      if (_ostree_repo_is_locked_tmpdir (dent->d_name))
        {
          if (!_ostree_repo_try_lock_tmpdir (dfd_iter.fd, dent->d_name,
                                             &lockfile, &did_lock, error))
            return FALSE;
          if (!did_lock)
            continue;
        }

      /* If however this is the staging directory for the *current*
       * boot, then don't delete it now - we may end up reusing it, as
       * is the point.
       */
      if (g_str_has_prefix (dent->d_name, self->stagedir_prefix))
        continue;
      else if (g_str_has_prefix (dent->d_name, OSTREE_REPO_TMPDIR_STAGING))
        {
          /* But, crucially we can now clean up staging directories
           * from *other* boots
           */
          if (!glnx_shutil_rm_rf_at (dfd_iter.fd, dent->d_name, cancellable, error))
            return FALSE;
        }
      /* FIXME - move OSTREE_REPO_TMPDIR_FETCHER underneath the
       * staging/boot-id scheme as well, since all of the "did it get
       * fsync'd" concerns apply to that as well.  Then we can skip
       * this special case.
       */
      else if (g_str_has_prefix (dent->d_name, OSTREE_REPO_TMPDIR_FETCHER))
        continue;
      else
        {
          /* Now we do time-based cleanup.  Ignore it if it's somehow
           * in the future...
           */
          if (stbuf.st_mtime > curtime_secs)
            continue;

          /* Now, we're pruning content based on the expiry, which
           * defaults to a day.  That's what we were doing before we
           * had locking...but in future we can be smarter here.
           */
          delta = curtime_secs - stbuf.st_mtime;
          if (delta > self->tmp_expiry_seconds)
            {
              if (!glnx_shutil_rm_rf_at (dfd_iter.fd, dent->d_name, cancellable, error))
                return FALSE;
            }
        }
    }

  return TRUE;
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
 * @checksum: (nullable): The checksum to point it to
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
 * @checksum: (nullable): The checksum to point it to
 *
 * If @checksum is not %NULL, then record it as the target of ref named
 * @ref; if @remote is provided, the ref will appear to originate from that
 * remote.
 *
 * Otherwise, if @checksum is %NULL, then record that the ref should
 * be deleted.
 *
 * The change will not be written out immediately, but when the transaction
 * is completed with ostree_repo_commit_transaction(). If the transaction
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
 * @checksum: (allow-none): The checksum to point it to, or %NULL to unset
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
  g_return_val_if_fail (self->in_transaction == TRUE, FALSE);

  if ((self->test_error_flags & OSTREE_REPO_TEST_ERROR_PRE_COMMIT) > 0)
    return glnx_throw (error, "OSTREE_REPO_TEST_ERROR_PRE_COMMIT specified");

  /* FIXME: Added since valgrind in el7 doesn't know about
   * `syncfs`...we should delete this later.
   */
  if (g_getenv ("OSTREE_SUPPRESS_SYNCFS") == NULL)
    {
      if (syncfs (self->tmp_dir_fd) < 0)
        return glnx_throw_errno (error);
    }

  if (!rename_pending_loose_objects (self, cancellable, error))
    return FALSE;

  if (!cleanup_tmpdir (self, cancellable, error))
    return FALSE;

  if (self->loose_object_devino_hash)
    g_hash_table_remove_all (self->loose_object_devino_hash);

  if (self->txn_refs)
    if (!_ostree_repo_update_refs (self, self->txn_refs, cancellable, error))
      return FALSE;
  g_clear_pointer (&self->txn_refs, g_hash_table_destroy);

  if (self->commit_stagedir_fd != -1)
    {
      (void) close (self->commit_stagedir_fd);
      self->commit_stagedir_fd = -1;

      glnx_release_lock_file (&self->commit_stagedir_lock);
    }

  g_clear_pointer (&self->commit_stagedir_name, g_free);

  self->in_transaction = FALSE;

  if (!ot_ensure_unlinked_at (self->repo_dir_fd, "transaction", 0))
    return FALSE;

  if (out_stats)
    *out_stats = self->txn_stats;

  return TRUE;
}

gboolean
ostree_repo_abort_transaction (OstreeRepo     *self,
                               GCancellable   *cancellable,
                               GError        **error)
{
  /* Note early return */
  if (!self->in_transaction)
    return TRUE;

  if (!cleanup_tmpdir (self, cancellable, error))
    return FALSE;

  if (self->loose_object_devino_hash)
    g_hash_table_remove_all (self->loose_object_devino_hash);

  g_clear_pointer (&self->txn_refs, g_hash_table_destroy);

  if (self->commit_stagedir_fd != -1)
    {
      (void) close (self->commit_stagedir_fd);
      self->commit_stagedir_fd = -1;

      glnx_release_lock_file (&self->commit_stagedir_lock);
    }
  g_clear_pointer (&self->commit_stagedir_name, g_free);

  self->in_transaction = FALSE;

  return TRUE;
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
  g_autoptr(GVariant) normalized = NULL;

  normalized = g_variant_get_normal_form (object);

  if (G_UNLIKELY (g_variant_get_size (normalized) > OSTREE_MAX_METADATA_SIZE))
    {
      g_autofree char *input_bytes = g_format_size (g_variant_get_size (normalized));
      g_autofree char *max_bytes = g_format_size (OSTREE_MAX_METADATA_SIZE);
      return glnx_throw (error, "Metadata object of type '%s' is %s; maximum metadata size is %s",
                         ostree_object_type_to_string (objtype), input_bytes, max_bytes);
    }

  g_autoptr(GInputStream) input = ot_variant_read (normalized);

  if (!write_object (self, objtype, expected_checksum,
                     input, g_variant_get_size (normalized),
                     out_csum,
                     cancellable, error))
    return FALSE;

  return TRUE;
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
  g_autoptr(GInputStream) input = NULL;
  g_autoptr(GVariant) normalized = NULL;

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
  g_autoptr(GVariant) dirmeta = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  dirmeta = ostree_create_directory_metadata (file_info, xattrs);

  return ostree_repo_write_metadata (self, OSTREE_OBJECT_TYPE_DIR_META, NULL,
                                     dirmeta, out_csum, cancellable, error);
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
 * @subject: (allow-none): Subject
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
  GDateTime *now = NULL;

  now = g_date_time_new_now_utc ();
  ret = ostree_repo_write_commit_with_time (self,
                                          parent,
                                          subject,
                                          body,
                                          metadata,
                                          root,
                                          g_date_time_to_unix (now),
                                          out_commit,
                                          cancellable,
                                          error);
  g_date_time_unref (now);
  return ret;
}

/**
 * ostree_repo_write_commit_with_time:
 * @self: Repo
 * @parent: (allow-none): ASCII SHA256 checksum for parent, or %NULL for none
 * @subject: (allow-none): Subject
 * @body: (allow-none): Body
 * @metadata: (allow-none): GVariant of type a{sv}, or %NULL for none
 * @root: The tree to point the commit to
 * @time: The time to use to stamp the commit
 * @out_commit: (out): Resulting ASCII SHA256 checksum for commit
 * @cancellable: Cancellable
 * @error: Error
 *
 * Write a commit metadata object, referencing @root_contents_checksum
 * and @root_metadata_checksum.
 */
gboolean
ostree_repo_write_commit_with_time (OstreeRepo      *self,
                                    const char      *parent,
                                    const char      *subject,
                                    const char      *body,
                                    GVariant        *metadata,
                                    OstreeRepoFile  *root,
                                    guint64          time,
                                    char           **out_commit,
                                    GCancellable    *cancellable,
                                    GError         **error)
{
  OstreeRepoFile *repo_root = OSTREE_REPO_FILE (root);

  /* Add sizes information to our metadata object */
  g_autoptr(GVariant) new_metadata = NULL;
  if (!add_size_index_to_metadata (self, metadata, &new_metadata,
                                   cancellable, error))
    return FALSE;

  g_autoptr(GVariant) commit =
    g_variant_new ("(@a{sv}@ay@a(say)sst@ay@ay)",
                   new_metadata ? new_metadata : create_empty_gvariant_dict (),
                   parent ? ostree_checksum_to_bytes_v (parent) : ot_gvariant_new_bytearray (NULL, 0),
                   g_variant_new_array (G_VARIANT_TYPE ("(say)"), NULL, 0),
                   subject ? subject : "", body ? body : "",
                   GUINT64_TO_BE (time),
                   ostree_checksum_to_bytes_v (ostree_repo_file_tree_get_contents_checksum (repo_root)),
                   ostree_checksum_to_bytes_v (ostree_repo_file_tree_get_metadata_checksum (repo_root)));
  g_variant_ref_sink (commit);
  g_autofree guchar *commit_csum = NULL;
  if (!ostree_repo_write_metadata (self, OSTREE_OBJECT_TYPE_COMMIT, NULL,
                                   commit, &commit_csum,
                                   cancellable, error))
    return FALSE;

  g_autofree char *ret_commit = ostree_checksum_from_bytes (commit_csum);
  ot_transfer_out_value(out_commit, &ret_commit);
  return TRUE;
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
  char buf[_OSTREE_LOOSE_PATH_MAX];
  _ostree_loose_path (buf, checksum, OSTREE_OBJECT_TYPE_COMMIT_META, self->mode);

  g_autoptr(GVariant) ret_metadata = NULL;
  if (self->commit_stagedir_fd != -1 &&
      !ot_util_variant_map_at (self->commit_stagedir_fd, buf,
                               G_VARIANT_TYPE ("a{sv}"),
                               OT_VARIANT_MAP_ALLOW_NOENT | OT_VARIANT_MAP_TRUSTED, &ret_metadata, error))
    return glnx_prefix_error (error, "Unable to read existing detached metadata");

  if (ret_metadata == NULL &&
      !ot_util_variant_map_at (self->objects_dir_fd, buf,
                               G_VARIANT_TYPE ("a{sv}"),
                               OT_VARIANT_MAP_ALLOW_NOENT | OT_VARIANT_MAP_TRUSTED, &ret_metadata, error))
    return glnx_prefix_error (error, "Unable to read existing detached metadata");

  if (ret_metadata == NULL && self->parent_repo)
    return ostree_repo_read_commit_detached_metadata (self->parent_repo,
                                                      checksum,
                                                      out_metadata,
                                                      cancellable,
                                                      error);
  ot_transfer_out_value (out_metadata, &ret_metadata);
  return TRUE;
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
  char pathbuf[_OSTREE_LOOSE_PATH_MAX];
  g_autoptr(GVariant) normalized = NULL;
  gsize normalized_size = 0;
  const guint8 *data = NULL;
  int dest_dfd;

  if (self->in_transaction)
    dest_dfd = self->commit_stagedir_fd;
  else
    dest_dfd = self->objects_dir_fd;

  _ostree_loose_path (pathbuf, checksum, OSTREE_OBJECT_TYPE_COMMIT_META, self->mode);

  if (!_ostree_repo_ensure_loose_objdir_at (dest_dfd, checksum,
                                            cancellable, error))
    return FALSE;

  if (metadata != NULL)
    {
      normalized = g_variant_get_normal_form (metadata);
      normalized_size = g_variant_get_size (normalized);
      data = g_variant_get_data (normalized);
    }

  if (data == NULL)
    data = (guint8*)"";

  if (!glnx_file_replace_contents_at (dest_dfd, pathbuf,
                                      data, normalized_size,
                                      0, cancellable, error))
    {
      g_prefix_error (error, "Unable to write detached metadata: ");
      return FALSE;
    }

  return TRUE;
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

      /* Should have been validated earlier, but be paranoid */
      g_assert (ot_util_filename_validate (name, NULL));

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

OstreeRepoCommitFilterResult
_ostree_repo_commit_modifier_apply (OstreeRepo               *self,
                                    OstreeRepoCommitModifier *modifier,
                                    const char               *path,
                                    GFileInfo                *file_info,
                                    GFileInfo               **out_modified_info)
{
  OstreeRepoCommitFilterResult result = OSTREE_REPO_COMMIT_FILTER_ALLOW;
  GFileInfo *modified_info;

  if (modifier == NULL ||
      (modifier->filter == NULL &&
       (modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS) == 0))
    {
      *out_modified_info = g_object_ref (file_info);
      return OSTREE_REPO_COMMIT_FILTER_ALLOW;
    }

  modified_info = g_file_info_dup (file_info);
  if (modifier->filter)
    result = modifier->filter (self, path, modified_info, modifier->user_data);

  if ((modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS) != 0)
    {

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
        {
          guint current_mode = g_file_info_get_attribute_uint32 (modified_info, "unix::mode");
          g_file_info_set_attribute_uint32 (modified_info, "unix::mode", current_mode | 0744);
        }
      g_file_info_set_attribute_uint32 (modified_info, "unix::uid", 0);
      g_file_info_set_attribute_uint32 (modified_info, "unix::gid", 0);
    }

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
  if (modifier == NULL ||
      (modifier->filter == NULL &&
       (modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS) == 0))
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
  g_autoptr(GVariant) ret_xattrs = NULL;

  if (modifier && modifier->xattr_callback)
    {
      ret_xattrs = modifier->xattr_callback (self, relpath, file_info,
                                             modifier->xattr_user_data);
    }
  else if (!(modifier && (modifier->flags & (OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS |
                                             OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS)) > 0)
           && !self->disable_xattrs)
    {
      if (path && OSTREE_IS_REPO_FILE (path))
        {
          if (!ostree_repo_file_get_xattrs (OSTREE_REPO_FILE (path),
                                            &ret_xattrs,
                                            cancellable,
                                            error))
            return FALSE;
        }
      else if (path)
        {
          if (!glnx_dfd_name_get_all_xattrs (AT_FDCWD, gs_file_get_path_cached (path),
                                             &ret_xattrs, cancellable, error))
            return FALSE;
        }
      else if (dfd_subpath == NULL)
        {
          g_assert (dfd != -1);
          if (!glnx_fd_get_all_xattrs (dfd, &ret_xattrs,
                                     cancellable, error))
            return FALSE;
        }
      else
        {
          g_assert (dfd != -1);
          if (!glnx_dfd_name_get_all_xattrs (dfd, dfd_subpath, &ret_xattrs,
                                               cancellable, error))
            return FALSE;
        }
    }

  if (modifier && modifier->sepolicy)
    {
      g_autofree char *label = NULL;

      if (!ostree_sepolicy_get_label (modifier->sepolicy, relpath,
                                      g_file_info_get_attribute_uint32 (file_info, "unix::mode"),
                                      &label, cancellable, error))
        return FALSE;

      if (!label && (modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_ERROR_ON_UNLABELED) > 0)
        {
          return glnx_throw (error, "Failed to look up SELinux label for '%s'", relpath);
        }
      else if (label)
        {
          g_autoptr(GVariantBuilder) builder = NULL;

          /* ret_xattrs may be NULL */
          builder = ot_util_variant_builder_from_variant (ret_xattrs,
                                                          G_VARIANT_TYPE ("a(ayay)"));

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

  if (out_xattrs)
    *out_xattrs = g_steal_pointer (&ret_xattrs);
  return TRUE;
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
                                  GLnxDirFdIterator           *src_dfd_iter,
                                  OstreeMutableTree           *mtree,
                                  OstreeRepoCommitModifier    *modifier,
                                  GPtrArray                   *path,
                                  GCancellable                *cancellable,
                                  GError                     **error);

static gboolean
write_directory_content_to_mtree_internal (OstreeRepo                  *self,
                                           OstreeRepoFile              *repo_dir,
                                           GFileEnumerator             *dir_enum,
                                           GLnxDirFdIterator           *dfd_iter,
                                           GFileInfo                   *child_info,
                                           OstreeMutableTree           *mtree,
                                           OstreeRepoCommitModifier    *modifier,
                                           GPtrArray                   *path,
                                           GCancellable                *cancellable,
                                           GError                     **error)
{
  g_autoptr(GFile) child = NULL;
  g_autoptr(GFileInfo) modified_info = NULL;
  glnx_unref_object OstreeMutableTree *child_mtree = NULL;
  g_autofree char *child_relpath = NULL;
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
      /* Note: early return */
      return TRUE;
    }

  file_type = g_file_info_get_file_type (child_info);
  switch (file_type)
    {
    case G_FILE_TYPE_DIRECTORY:
    case G_FILE_TYPE_SYMBOLIC_LINK:
    case G_FILE_TYPE_REGULAR:
      break;
    default:
      return glnx_throw (error, "Unsupported file type: '%s'",
                         gs_file_get_path_cached (child));
    }

  if (dir_enum != NULL)
    child = g_file_enumerator_get_child (dir_enum, child_info);

  if (file_type == G_FILE_TYPE_DIRECTORY)
    {
      if (!ostree_mutable_tree_ensure_dir (mtree, name, &child_mtree, error))
        return FALSE;

      if (dir_enum != NULL)
        {
          if (!write_directory_to_mtree_internal (self, child, child_mtree,
                                                  modifier, path,
                                                  cancellable, error))
            return FALSE;
        }
      else
        {
          g_auto(GLnxDirFdIterator) child_dfd_iter = { 0, };

          if (!glnx_dirfd_iterator_init_at (dfd_iter->fd, name, FALSE, &child_dfd_iter, error))
            return FALSE;

          if (!write_dfd_iter_to_mtree_internal (self, &child_dfd_iter, child_mtree,
                                                 modifier, path,
                                                 cancellable, error))
            return FALSE;
        }
    }
  else if (repo_dir)
    {
      g_assert (dir_enum != NULL);
      g_debug ("Adding: %s", gs_file_get_path_cached (child));
      if (!ostree_mutable_tree_replace_file (mtree, name,
                                             ostree_repo_file_get_checksum ((OstreeRepoFile*) child),
                                             error))
        return FALSE;
    }
  else
    {
      guint64 file_obj_length;
      const char *loose_checksum;
      g_autoptr(GInputStream) file_input = NULL;
      g_autoptr(GVariant) xattrs = NULL;
      g_autoptr(GInputStream) file_object_input = NULL;
      g_autofree guchar *child_file_csum = NULL;
      g_autofree char *tmp_checksum = NULL;

      loose_checksum = devino_cache_lookup (self, modifier,
                                            g_file_info_get_attribute_uint32 (child_info, "unix::device"),
                                            g_file_info_get_attribute_uint64 (child_info, "unix::inode"));

      if (loose_checksum)
        {
          if (!ostree_mutable_tree_replace_file (mtree, name, loose_checksum,
                                                 error))
            return FALSE;
        }
      else
        {
          if (g_file_info_get_file_type (modified_info) == G_FILE_TYPE_REGULAR)
            {
              if (child != NULL)
                {
                  file_input = (GInputStream*)g_file_read (child, cancellable, error);
                  if (!file_input)
                    return FALSE;
                }
              else
                {
                  if (!ot_openat_read_stream (dfd_iter->fd, name, FALSE,
                                              &file_input, cancellable, error))
                    return FALSE;
                }
            }

          if (!get_modified_xattrs (self, modifier,
                                    child_relpath, child_info, child, dfd_iter != NULL ? dfd_iter->fd : -1, name,
                                    &xattrs,
                                    cancellable, error))
            return FALSE;

          if (!ostree_raw_file_to_content_stream (file_input,
                                                  modified_info, xattrs,
                                                  &file_object_input, &file_obj_length,
                                                  cancellable, error))
            return FALSE;
          if (!ostree_repo_write_content (self, NULL, file_object_input, file_obj_length,
                                          &child_file_csum, cancellable, error))
            return FALSE;

          g_free (tmp_checksum);
          tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
          if (!ostree_mutable_tree_replace_file (mtree, name, tmp_checksum,
                                                 error))
            return FALSE;
        }
    }

  g_ptr_array_remove_index (path, path->len - 1);

  return TRUE;
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
  OstreeRepoCommitFilterResult filter_result;
  OstreeRepoFile *repo_dir = NULL;

  if (dir)
    g_debug ("Examining: %s", gs_file_get_path_cached (dir));

  /* If the directory is already in the repository, we can try to
   * reuse checksums to skip checksumming. */
  if (dir && OSTREE_IS_REPO_FILE (dir) && modifier == NULL)
    repo_dir = (OstreeRepoFile *) dir;

  if (repo_dir)
    {
      if (!ostree_repo_file_ensure_resolved (repo_dir, error))
        return FALSE;

      ostree_mutable_tree_set_metadata_checksum (mtree, ostree_repo_file_tree_get_metadata_checksum (repo_dir));

      filter_result = OSTREE_REPO_COMMIT_FILTER_ALLOW;
    }
  else
    {
      g_autoptr(GVariant) xattrs = NULL;

      g_autoptr(GFileInfo) child_info =
        g_file_query_info (dir, OSTREE_GIO_FAST_QUERYINFO,
                           G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                           cancellable, error);
      if (!child_info)
        return FALSE;

      g_autofree char *relpath = NULL;
      if (modifier != NULL)
        relpath = ptrarray_path_join (path);

      g_autoptr(GFileInfo) modified_info = NULL;
      filter_result = apply_commit_filter (self, modifier, relpath, child_info, &modified_info);

      if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
        {
          if (!get_modified_xattrs (self, modifier, relpath, child_info,
                                    dir, -1, NULL,
                                    &xattrs,
                                    cancellable, error))
            return FALSE;

          g_autofree guchar *child_file_csum = NULL;
          if (!_ostree_repo_write_directory_meta (self, modified_info, xattrs, &child_file_csum,
                                                  cancellable, error))
            return FALSE;

          g_autofree char *tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
          ostree_mutable_tree_set_metadata_checksum (mtree, tmp_checksum);
        }
    }

  if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
    {
      g_autoptr(GFileEnumerator) dir_enum = NULL;

      dir_enum = g_file_enumerate_children ((GFile*)dir, OSTREE_GIO_FAST_QUERYINFO,
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            cancellable,
                                            error);
      if (!dir_enum)
        return FALSE;

      while (TRUE)
        {
          GFileInfo *child_info;

          if (!g_file_enumerator_iterate (dir_enum, &child_info, NULL,
                                          cancellable, error))
            return FALSE;
          if (child_info == NULL)
            break;

          if (!write_directory_content_to_mtree_internal (self, repo_dir, dir_enum, NULL,
                                                          child_info,
                                                          mtree, modifier, path,
                                                          cancellable, error))
            return FALSE;
        }
    }

  return TRUE;
}

static gboolean
write_dfd_iter_to_mtree_internal (OstreeRepo                  *self,
                                  GLnxDirFdIterator           *src_dfd_iter,
                                  OstreeMutableTree           *mtree,
                                  OstreeRepoCommitModifier    *modifier,
                                  GPtrArray                   *path,
                                  GCancellable                *cancellable,
                                  GError                     **error)
{
  g_autoptr(GFileInfo) child_info = NULL;
  g_autoptr(GFileInfo) modified_info = NULL;
  g_autoptr(GVariant) xattrs = NULL;
  g_autofree guchar *child_file_csum = NULL;
  g_autofree char *tmp_checksum = NULL;
  g_autofree char *relpath = NULL;
  OstreeRepoCommitFilterResult filter_result;
  struct stat dir_stbuf;

  if (fstat (src_dfd_iter->fd, &dir_stbuf) != 0)
    return glnx_throw_errno (error);

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
        return FALSE;

      if (!_ostree_repo_write_directory_meta (self, modified_info, xattrs, &child_file_csum,
                                              cancellable, error))
        return FALSE;

      g_free (tmp_checksum);
      tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
      ostree_mutable_tree_set_metadata_checksum (mtree, tmp_checksum);
    }

  if (filter_result != OSTREE_REPO_COMMIT_FILTER_ALLOW)
    {
      /* Note - early return */
      return TRUE;
    }

  while (TRUE)
    {
      struct dirent *dent;
      struct stat stbuf;
      g_autoptr(GFileInfo) child_info = NULL;
      const char *loose_checksum;
      if (!glnx_dirfd_iterator_next_dent (src_dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (fstatat (src_dfd_iter->fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
        return glnx_throw_errno (error);

      loose_checksum = devino_cache_lookup (self, modifier, stbuf.st_dev, stbuf.st_ino);
      if (loose_checksum)
        {
          if (!ostree_mutable_tree_replace_file (mtree, dent->d_name, loose_checksum,
                                                 error))
            return FALSE;

          continue;
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
            return FALSE;
        }
      else if (S_ISDIR (stbuf.st_mode))
        ;
      else
        {
          return glnx_throw (error, "Not a regular file or symlink: %s",
                             dent->d_name);
        }

      if (!write_directory_content_to_mtree_internal (self, NULL, NULL, src_dfd_iter,
                                                      child_info,
                                                      mtree, modifier, path,
                                                      cancellable, error))
        return FALSE;
    }

  return TRUE;
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

  /* Short cut local files */
  if (g_file_is_native (dir))
    {
      if (!ostree_repo_write_dfd_to_mtree (self, AT_FDCWD, gs_file_get_path_cached (dir),
                                           mtree, modifier, cancellable, error))
        return FALSE;
    }
  else
    {
      if (modifier && modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_GENERATE_SIZES)
        self->generate_sizes = TRUE;

      g_autoptr(GPtrArray) path = g_ptr_array_new ();
      if (!write_directory_to_mtree_internal (self, dir, mtree, modifier, path,
                                              cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/**
 * ostree_repo_write_dfd_to_mtree:
 * @self: Repo
 * @dfd: Directory file descriptor
 * @path: Path
 * @mtree: Overlay directory contents into this tree
 * @modifier: (allow-none): Optional modifier
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store as objects all contents of the directory referred to by @dfd
 * and @path all children into the repository @self, overlaying the
 * resulting filesystem hierarchy into @mtree.
 */
gboolean
ostree_repo_write_dfd_to_mtree (OstreeRepo                *self,
                                int                        dfd,
                                const char                *path,
                                OstreeMutableTree         *mtree,
                                OstreeRepoCommitModifier  *modifier,
                                GCancellable              *cancellable,
                                GError                   **error)
{
  if (modifier && modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_GENERATE_SIZES)
    self->generate_sizes = TRUE;

  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  if (!glnx_dirfd_iterator_init_at (dfd, path, FALSE, &dfd_iter, error))
    return FALSE;

  g_autoptr(GPtrArray) pathbuilder = g_ptr_array_new ();
  if (!write_dfd_iter_to_mtree_internal (self, &dfd_iter, mtree, modifier, pathbuilder,
                                         cancellable, error))
    return FALSE;

  return TRUE;
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
  const char *contents_checksum, *metadata_checksum;
  g_autoptr(GFile) ret_file = NULL;

  metadata_checksum = ostree_mutable_tree_get_metadata_checksum (mtree);
  if (!metadata_checksum)
    return glnx_throw (error, "Can't commit an empty tree");

  contents_checksum = ostree_mutable_tree_get_contents_checksum (mtree);
  if (contents_checksum)
    {
      ret_file = G_FILE (_ostree_repo_file_new_root (self, contents_checksum, metadata_checksum));
    }
  else
    {
      g_autoptr(GHashTable) dir_metadata_checksums = NULL;
      g_autoptr(GHashTable) dir_contents_checksums = NULL;
      g_autoptr(GVariant) serialized_tree = NULL;
      g_autofree guchar *contents_csum = NULL;
      char contents_checksum_buf[OSTREE_SHA256_STRING_LEN+1];

      dir_contents_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free, (GDestroyNotify)g_free);
      dir_metadata_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free, (GDestroyNotify)g_free);

      GHashTableIter hash_iter;
      gpointer key, value;
      g_hash_table_iter_init (&hash_iter, ostree_mutable_tree_get_subdirs (mtree));
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *name = key;
          g_autoptr(GFile) child_file = NULL;
          OstreeMutableTree *child_dir = value;

          if (!ostree_repo_write_mtree (self, child_dir, &child_file,
                                        cancellable, error))
            return FALSE;

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
        return FALSE;

      ostree_checksum_inplace_from_bytes (contents_csum, contents_checksum_buf);
      ostree_mutable_tree_set_contents_checksum (mtree, contents_checksum_buf);

      ret_file = G_FILE (_ostree_repo_file_new_root (self, contents_checksum_buf, metadata_checksum));
    }

  if (out_file)
    *out_file = g_steal_pointer (&ret_file);
  return TRUE;
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
  gint refcount = g_atomic_int_add (&modifier->refcount, 1);
  g_assert (refcount > 0);
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
  g_clear_pointer (&modifier->devino_cache, (GDestroyNotify)g_hash_table_unref);

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

/**
 * ostree_repo_commit_modifier_set_devino_cache:
 * @modifier: Modifier
 * @cache: A hash table caching device,inode to checksums
 *
 * See the documentation for
 * `ostree_repo_devino_cache_new()`.  This function can
 * then be used for later calls to
 * `ostree_repo_write_directory_to_mtree()` to optimize commits.
 *
 * Note if your process has multiple writers, you should use separate
 * `OSTreeRepo` instances if you want to also use this API.
 *
 * This function will add a reference to @cache without copying - you
 * should avoid further mutation of the cache.
 */
void
ostree_repo_commit_modifier_set_devino_cache (OstreeRepoCommitModifier              *modifier,
                                              OstreeRepoDevInoCache                 *cache)
{
  modifier->devino_cache = g_hash_table_ref ((GHashTable*)cache);
}

OstreeRepoDevInoCache *
ostree_repo_devino_cache_ref (OstreeRepoDevInoCache *cache)
{
  g_hash_table_ref ((GHashTable*)cache);
  return cache;
}

void
ostree_repo_devino_cache_unref (OstreeRepoDevInoCache *cache)
{
  g_hash_table_unref ((GHashTable*)cache);
}

G_DEFINE_BOXED_TYPE(OstreeRepoDevInoCache, ostree_repo_devino_cache,
                    ostree_repo_devino_cache_ref,
                    ostree_repo_devino_cache_unref);

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
