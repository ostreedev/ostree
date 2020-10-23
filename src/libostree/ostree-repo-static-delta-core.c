/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
 */

#include "config.h"

#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <gio/gfiledescriptorbased.h>
#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-lzma-decompressor.h"
#include "ostree-cmdprivate.h"
#include "ostree-checksum-input-stream.h"
#include "ostree-repo-static-delta-private.h"
#include "otutil.h"

gboolean
_ostree_static_delta_parse_checksum_array (GVariant      *array,
                                           guint8       **out_checksums_array,
                                           guint         *out_n_checksums,
                                           GError       **error)
{
  const gsize n = g_variant_n_children (array);
  const guint n_checksums = n / OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN;

  if (G_UNLIKELY(n > (G_MAXUINT32/OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN) ||
                 (n_checksums * OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN) != n))
    {
      return glnx_throw (error,
                        "Invalid checksum array length %" G_GSIZE_FORMAT, n);
    }

  *out_checksums_array = (gpointer)g_variant_get_data (array);
  *out_n_checksums = n_checksums;

  return TRUE;
}

GVariant *
_ostree_repo_static_delta_superblock_digest (OstreeRepo    *repo,
                                             const char    *from,
                                             const char    *to,
                                             GCancellable  *cancellable,
                                             GError       **error)
{
  g_autofree char *superblock = _ostree_get_relative_static_delta_superblock_path ((from && from[0]) ? from : NULL, to);
  glnx_autofd int superblock_file_fd = -1;
  guint8 digest[OSTREE_SHA256_DIGEST_LEN];

  if (!glnx_openat_rdonly (repo->repo_dir_fd, superblock, TRUE, &superblock_file_fd, error))
    return NULL;

  g_autoptr(GBytes) superblock_content = ot_fd_readall_or_mmap (superblock_file_fd, 0, error);
  if (!superblock_content)
    return NULL;

  ot_checksum_bytes (superblock_content, digest);

  return ot_gvariant_new_bytearray (digest, sizeof (digest));
}

/**
 * ostree_repo_list_static_delta_names:
 * @self: Repo
 * @out_deltas: (out) (element-type utf8) (transfer container): String name of deltas (checksum-checksum.delta)
 * @cancellable: Cancellable
 * @error: Error
 *
 * This function synchronously enumerates all static deltas in the
 * repository, returning its result in @out_deltas.
 */
gboolean
ostree_repo_list_static_delta_names (OstreeRepo                  *self,
                                     GPtrArray                  **out_deltas,
                                     GCancellable                *cancellable,
                                     GError                     **error)
{
  g_autoptr(GPtrArray) ret_deltas = g_ptr_array_new_with_free_func (g_free);

  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  gboolean exists;
  if (!ot_dfd_iter_init_allow_noent (self->repo_dir_fd, "deltas", &dfd_iter,
                                     &exists, error))
    return FALSE;
  if (!exists)
    {
      /* Note early return */
      ot_transfer_out_value (out_deltas, &ret_deltas);
      return TRUE;
    }

  while (TRUE)
    {
      g_auto(GLnxDirFdIterator) sub_dfd_iter = { 0, };
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;
      if (dent->d_type != DT_DIR)
        continue;

      if (!glnx_dirfd_iterator_init_at (dfd_iter.fd, dent->d_name, FALSE,
                                        &sub_dfd_iter, error))
        return FALSE;

      while (TRUE)
        {
          struct dirent *sub_dent;
          if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&sub_dfd_iter, &sub_dent,
                                                           cancellable, error))
            return FALSE;
          if (sub_dent == NULL)
            break;
          if (sub_dent->d_type != DT_DIR)
            continue;

          const char *name1 = dent->d_name;
          const char *name2 = sub_dent->d_name;

          g_autofree char *superblock_subpath = g_strconcat (name2, "/superblock", NULL);
          if (!glnx_fstatat_allow_noent (sub_dfd_iter.fd, superblock_subpath, NULL, 0, error))
            return FALSE;
          if (errno == ENOENT)
            continue;

          g_autofree char *buf = g_strconcat (name1, name2, NULL);
          GString *out = g_string_new ("");
          char checksum[OSTREE_SHA256_STRING_LEN+1];
          guchar csum[OSTREE_SHA256_DIGEST_LEN];
          const char *dash = strchr (buf, '-');

          ostree_checksum_b64_inplace_to_bytes (buf, csum);
          ostree_checksum_inplace_from_bytes (csum, checksum);
          g_string_append (out, checksum);
          if (dash)
            {
              g_string_append_c (out, '-');
              ostree_checksum_b64_inplace_to_bytes (dash+1, csum);
              ostree_checksum_inplace_from_bytes (csum, checksum);
              g_string_append (out, checksum);
            }

          g_ptr_array_add (ret_deltas, g_string_free (out, FALSE));
        }
    }

  ot_transfer_out_value (out_deltas, &ret_deltas);
  return TRUE;
}

/**
 * ostree_repo_list_static_delta_indexes:
 * @self: Repo
 * @out_indexes: (out) (element-type utf8) (transfer container): String name of delta indexes (checksum)
 * @cancellable: Cancellable
 * @error: Error
 *
 * This function synchronously enumerates all static delta indexes in the
 * repository, returning its result in @out_indexes.
 *
 * Since: 2020.7
 */
gboolean
ostree_repo_list_static_delta_indexes (OstreeRepo                  *self,
                                       GPtrArray                  **out_indexes,
                                       GCancellable                *cancellable,
                                       GError                     **error)
{
  g_autoptr(GPtrArray) ret_indexes = g_ptr_array_new_with_free_func (g_free);

  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  gboolean exists;
  if (!ot_dfd_iter_init_allow_noent (self->repo_dir_fd, "delta-indexes", &dfd_iter,
                                     &exists, error))
    return FALSE;
  if (!exists)
    {
      /* Note early return */
      ot_transfer_out_value (out_indexes, &ret_indexes);
      return TRUE;
    }

  while (TRUE)
    {
      g_auto(GLnxDirFdIterator) sub_dfd_iter = { 0, };
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;
      if (dent->d_type != DT_DIR)
        continue;
      if (strlen (dent->d_name) != 2)
        continue;

      if (!glnx_dirfd_iterator_init_at (dfd_iter.fd, dent->d_name, FALSE,
                                        &sub_dfd_iter, error))
        return FALSE;

      while (TRUE)
        {
          struct dirent *sub_dent;

          if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&sub_dfd_iter, &sub_dent,
                                                           cancellable, error))
            return FALSE;
          if (sub_dent == NULL)
            break;
          if (sub_dent->d_type != DT_REG)
            continue;

          const char *name1 = dent->d_name;
          const char *name2 = sub_dent->d_name;

          /* base64 len is 43, but 2 chars are in the parent dir name */
          if (strlen (name2) != 41 + strlen(".index") ||
              !g_str_has_suffix (name2, ".index"))
            continue;

          g_autoptr(GString) out = g_string_new (name1);
          g_string_append_len (out, name2, 41);

          char checksum[OSTREE_SHA256_STRING_LEN+1];
          guchar csum[OSTREE_SHA256_DIGEST_LEN];

          ostree_checksum_b64_inplace_to_bytes (out->str, csum);
          ostree_checksum_inplace_from_bytes (csum, checksum);

          g_ptr_array_add (ret_indexes, g_strdup (checksum));
        }
    }

  ot_transfer_out_value (out_indexes, &ret_indexes);
  return TRUE;
}

gboolean
_ostree_repo_static_delta_part_have_all_objects (OstreeRepo             *repo,
                                                 GVariant               *checksum_array,
                                                 gboolean               *out_have_all,
                                                 GCancellable           *cancellable,
                                                 GError                **error)
{
  guint8 *checksums_data = NULL;
  guint n_checksums = 0;
  gboolean have_object = TRUE;

  if (!_ostree_static_delta_parse_checksum_array (checksum_array,
                                                  &checksums_data,
                                                  &n_checksums,
                                                  error))
    return FALSE;

  for (guint i = 0; i < n_checksums; i++)
    {
      guint8 objtype = *checksums_data;
      const guint8 *csum = checksums_data + 1;
      char tmp_checksum[OSTREE_SHA256_STRING_LEN+1];

      if (G_UNLIKELY(!ostree_validate_structureof_objtype (objtype, error)))
        return FALSE;

      ostree_checksum_inplace_from_bytes (csum, tmp_checksum);

      if (!ostree_repo_has_object (repo, (OstreeObjectType) objtype, tmp_checksum,
                                   &have_object, cancellable, error))
        return FALSE;

      if (!have_object)
        break;

      checksums_data += OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN;
    }

  *out_have_all = have_object;
  return TRUE;
}

static gboolean
_ostree_repo_static_delta_is_signed (OstreeRepo       *self,
                                     int               fd,
                                     GPtrArray       **out_value,
                                     GError          **error)
{
  g_autoptr(GVariant) delta = NULL;
  g_autoptr(GVariant) delta_sign_magic = NULL;
  g_autoptr(GVariant) delta_sign = NULL;
  GVariantIter iter;
  GVariant *item;
  g_autoptr(GPtrArray) signatures = NULL;
  gboolean ret = FALSE;

  if (out_value)
    *out_value = NULL;

  if (!ot_variant_read_fd (fd, 0, (GVariantType*)OSTREE_STATIC_DELTA_SIGNED_FORMAT, TRUE, &delta, error))
    return FALSE;

  delta_sign_magic = g_variant_get_child_value (delta, 0);
  if (delta_sign_magic == NULL)
    return glnx_throw (error, "no signatures in static-delta");

  if (GUINT64_FROM_BE (g_variant_get_uint64 (delta_sign_magic)) != OSTREE_STATIC_DELTA_SIGNED_MAGIC)
    return glnx_throw (error, "no signatures in static-delta");

  delta_sign = g_variant_get_child_value (delta, 2);
  if (delta_sign == NULL)
    return glnx_throw (error, "no signatures in static-delta");

  if (out_value)
    signatures = g_ptr_array_new_with_free_func (g_free);

  /* Check if there are signatures in the superblock */
  g_variant_iter_init (&iter, delta_sign);
  while ((item = g_variant_iter_next_value (&iter)))
    {
      g_autoptr(GVariant) key_v = g_variant_get_child_value (item, 0);
      const char *str = g_variant_get_string (key_v, NULL);
      if (g_str_has_prefix (str, "ostree.sign."))
        {
          ret = TRUE;
          if (signatures)
            g_ptr_array_add (signatures, g_strdup (str + strlen ("ostree.sign.")));
        }
      g_variant_unref (item);
    }

  if (out_value && ret)
    ot_transfer_out_value (out_value, &signatures);

  return ret;
}

static gboolean
_ostree_repo_static_delta_verify_signature (OstreeRepo       *self,
                                            int               fd,
                                            OstreeSign       *sign,
                                            char            **out_success_message,
                                            GError          **error)
{
  g_autoptr(GVariant) delta = NULL;

  if (!ot_variant_read_fd (fd, 0,
                           (GVariantType*)OSTREE_STATIC_DELTA_SIGNED_FORMAT,
                           TRUE, &delta, error))
    return FALSE;

  /* Check if there are signatures for signature engine */
  const gchar *signature_key = ostree_sign_metadata_key(sign);
  GVariantType *signature_format = (GVariantType *) ostree_sign_metadata_format(sign);
  g_autoptr(GVariant) delta_meta = g_variant_get_child_value (delta, 2);
  if (delta_meta == NULL)
    return glnx_throw (error, "no metadata in static-delta superblock");
  g_autoptr(GVariant) signatures = g_variant_lookup_value (delta_meta,
                                                           signature_key,
                                                           signature_format);
  if (!signatures)
    return glnx_throw (error, "no signature for '%s' in static-delta superblock", signature_key);

  /* Get static delta superblock */
  g_autoptr(GVariant) child = g_variant_get_child_value (delta, 1);
  if (child == NULL)
    return glnx_throw (error, "no metadata in static-delta superblock");
  g_autoptr(GBytes) signed_data = g_variant_get_data_as_bytes(child);

  return ostree_sign_data_verify (sign, signed_data, signatures, out_success_message, error);
}

/**
 * ostree_repo_static_delta_execute_offline_with_signature:
 * @self: Repo
 * @dir_or_file: Path to a directory containing static delta data, or directly to the superblock
 * @sign: Signature engine used to check superblock
 * @skip_validation: If %TRUE, assume data integrity
 * @cancellable: Cancellable
 * @error: Error
 *
 * Given a directory representing an already-downloaded static delta
 * on disk, apply it, generating a new commit.
 * If sign is passed, the static delta signature is verified.
 * If sign-verify-deltas configuration option is set and static delta is signed,
 * signature verification will be mandatory before apply the static delta.
 * The directory must be named with the form "FROM-TO", where both are
 * checksums, and it must contain a file named "superblock", along with at least
 * one part.
 *
 * Since: 2020.7
 */
gboolean
ostree_repo_static_delta_execute_offline_with_signature (OstreeRepo   *self,
                                                         GFile        *dir_or_file,
                                                         OstreeSign   *sign,
                                                         gboolean     skip_validation,
                                                         GCancellable *cancellable,
                                                         GError       **error)
{
  g_autofree char *basename = NULL;
  g_autoptr(GVariant) meta = NULL;

  const char *dir_or_file_path = gs_file_get_path_cached (dir_or_file);

  /* First, try opening it as a directory */
  glnx_autofd int dfd = glnx_opendirat_with_errno (AT_FDCWD, dir_or_file_path, TRUE);
  if (dfd < 0)
    {
      if (errno != ENOTDIR)
        return glnx_throw_errno_prefix (error, "openat(O_DIRECTORY)");
      else
        {
          g_autofree char *dir = dirname (g_strdup (dir_or_file_path));
          basename = g_path_get_basename (dir_or_file_path);

          if (!glnx_opendirat (AT_FDCWD, dir, TRUE, &dfd, error))
            return FALSE;
        }
    }
  else
    basename = g_strdup ("superblock");

  glnx_autofd int meta_fd = openat (dfd, basename, O_RDONLY | O_CLOEXEC);
  if (meta_fd < 0)
    return glnx_throw_errno_prefix (error, "openat(%s)", basename);

  gboolean is_signed = _ostree_repo_static_delta_is_signed (self, meta_fd, NULL, NULL);
  if (is_signed)
    {
      gboolean verify_deltas;
      gboolean verified;

      if (!ot_keyfile_get_boolean_with_default (self->config, "core", "sign-verify-deltas",
                                                FALSE, &verify_deltas, error))
        return FALSE;

      if (verify_deltas && !sign)
        return glnx_throw (error, "Key is mandatory to check delta signature");

      if (sign)
        {
          verified = _ostree_repo_static_delta_verify_signature (self, meta_fd, sign, NULL, error);
          if (*error)
            return FALSE;
          if (!verified)
            return glnx_throw (error, "Delta signature verification failed");
        }

      g_autoptr(GVariant) delta = NULL;
      if (!ot_variant_read_fd (meta_fd, 0, (GVariantType*)OSTREE_STATIC_DELTA_SIGNED_FORMAT,
                               TRUE, &delta, error))
        return FALSE;

      g_autoptr(GVariant) child = g_variant_get_child_value (delta, 1);
      g_autoptr(GBytes) bytes = g_variant_get_data_as_bytes (child);
      meta = g_variant_new_from_bytes ((GVariantType*)OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT,
                                       bytes, FALSE);
    }
  else
    {
      if (!ot_variant_read_fd (meta_fd, 0, G_VARIANT_TYPE (OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT),
                               FALSE, &meta, error))
        return FALSE;
    }

  /* Parsing OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT */

  g_autoptr(GVariant) metadata = g_variant_get_child_value (meta, 0);

  g_autofree char *to_checksum = NULL;
  g_autofree char *from_checksum = NULL;
  /* Write the to-commit object */
  {
    g_autoptr(GVariant) to_csum_v = NULL;
    g_autoptr(GVariant) from_csum_v = NULL;
    g_autoptr(GVariant) to_commit = NULL;
    gboolean have_to_commit;
    gboolean have_from_commit;

    to_csum_v = g_variant_get_child_value (meta, 3);
    if (!ostree_validate_structureof_csum_v (to_csum_v, error))
      return FALSE;
    to_checksum = ostree_checksum_from_bytes_v (to_csum_v);

    from_csum_v = g_variant_get_child_value (meta, 2);
    if (g_variant_n_children (from_csum_v) > 0)
      {
        if (!ostree_validate_structureof_csum_v (from_csum_v, error))
          return FALSE;
        from_checksum = ostree_checksum_from_bytes_v (from_csum_v);

        if (!ostree_repo_has_object (self, OSTREE_OBJECT_TYPE_COMMIT, from_checksum,
                                     &have_from_commit, cancellable, error))
          return FALSE;

        if (!have_from_commit)
          return glnx_throw (error, "Commit %s, which is the delta source, is not in repository", from_checksum);
      }

    if (!ostree_repo_has_object (self, OSTREE_OBJECT_TYPE_COMMIT, to_checksum,
                                 &have_to_commit, cancellable, error))
      return FALSE;

    if (!have_to_commit)
      {
        g_autofree char *detached_path = _ostree_get_relative_static_delta_path (from_checksum, to_checksum, "commitmeta");
        g_autoptr(GVariant) detached_data =
          g_variant_lookup_value (metadata, detached_path, G_VARIANT_TYPE("a{sv}"));
        if (detached_data && !ostree_repo_write_commit_detached_metadata (self,
                                                                          to_checksum,
                                                                          detached_data,
                                                                          cancellable,
                                                                          error))
          return FALSE;

        to_commit = g_variant_get_child_value (meta, 4);
        if (!ostree_repo_write_metadata (self, OSTREE_OBJECT_TYPE_COMMIT,
                                         to_checksum, to_commit, NULL,
                                         cancellable, error))
          return FALSE;
      }
  }

  g_autoptr(GVariant) fallback = g_variant_get_child_value (meta, 7);
  if (g_variant_n_children (fallback) > 0)
    return glnx_throw (error, "Cannot execute delta offline: contains nonempty http fallback entries");

  g_autoptr(GVariant) headers = g_variant_get_child_value (meta, 6);
  const guint n = g_variant_n_children (headers);
  for (guint i = 0; i < n; i++)
    {
      guint32 version;
      guint64 size;
      guint64 usize;
      char checksum[OSTREE_SHA256_STRING_LEN+1];
      g_autoptr(GVariant) csum_v = NULL;
      g_autoptr(GVariant) objects = NULL;
      g_autoptr(GVariant) part = NULL;
      OstreeStaticDeltaOpenFlags delta_open_flags =
        skip_validation ? OSTREE_STATIC_DELTA_OPEN_FLAGS_SKIP_CHECKSUM : 0;
      g_autoptr(GVariant) header = g_variant_get_child_value (headers, i);
      g_variant_get (header, "(u@aytt@ay)", &version, &csum_v, &size, &usize, &objects);

      if (version > OSTREE_DELTAPART_VERSION)
        return glnx_throw (error, "Delta part has too new version %u", version);

      gboolean have_all;
      if (!_ostree_repo_static_delta_part_have_all_objects (self, objects, &have_all,
                                                            cancellable, error))
        return FALSE;

      /* If we already have these objects, don't bother executing the
       * static delta.
       */
      if (have_all)
        continue;

      const guchar *csum = ostree_checksum_bytes_peek_validate (csum_v, error);
      if (!csum)
        return FALSE;
      ostree_checksum_inplace_from_bytes (csum, checksum);

      g_autofree char *deltapart_path =
        _ostree_get_relative_static_delta_part_path (from_checksum, to_checksum, i);

      g_autoptr(GInputStream) part_in = NULL;
      g_autoptr(GVariant) inline_part_data = g_variant_lookup_value (metadata, deltapart_path, G_VARIANT_TYPE("(yay)"));
      if (inline_part_data)
        {
          g_autoptr(GBytes) inline_part_bytes = g_variant_get_data_as_bytes (inline_part_data);
          part_in = g_memory_input_stream_new_from_bytes (inline_part_bytes);

          /* For inline parts, we don't checksum, because it's
           * included with the metadata, so we're not trying to
           * protect against MITM or such.  Non-security related
           * checksums should be done at the underlying storage layer.
           */
          delta_open_flags |= OSTREE_STATIC_DELTA_OPEN_FLAGS_SKIP_CHECKSUM;

          if (!_ostree_static_delta_part_open (part_in, inline_part_bytes,
                                               delta_open_flags,
                                               NULL,
                                               &part,
                                               cancellable, error))
            return FALSE;
        }
      else
        {
          g_autofree char *relpath = g_strdup_printf ("%u", i); /* TODO avoid malloc here */
          glnx_autofd int part_fd = openat (dfd, relpath, O_RDONLY | O_CLOEXEC);
          if (part_fd < 0)
            return glnx_throw_errno_prefix (error, "Opening deltapart '%s'", relpath);

          part_in = g_unix_input_stream_new (part_fd, FALSE);

          if (!_ostree_static_delta_part_open (part_in, NULL,
                                               delta_open_flags,
                                               checksum,
                                               &part,
                                               cancellable, error))
            return FALSE;
        }

      if (!_ostree_static_delta_part_execute (self, objects, part, skip_validation,
                                              NULL, cancellable, error))
        return glnx_prefix_error (error, "Executing delta part %i", i);
    }

  return TRUE;
}

/**
 * ostree_repo_static_delta_execute_offline:
 * @self: Repo
 * @dir_or_file: Path to a directory containing static delta data, or directly to the superblock
 * @skip_validation: If %TRUE, assume data integrity
 * @cancellable: Cancellable
 * @error: Error
 *
 * Given a directory representing an already-downloaded static delta
 * on disk, apply it, generating a new commit.  The directory must be
 * named with the form "FROM-TO", where both are checksums, and it
 * must contain a file named "superblock", along with at least one part.
 */
gboolean
ostree_repo_static_delta_execute_offline (OstreeRepo                    *self,
                                          GFile                         *dir_or_file,
                                          gboolean                       skip_validation,
                                          GCancellable                  *cancellable,
                                          GError                      **error)
{
  return ostree_repo_static_delta_execute_offline_with_signature(self, dir_or_file, NULL,
                                                                 skip_validation,
                                                                 cancellable,
                                                                 error);
}

gboolean
_ostree_static_delta_part_open (GInputStream   *part_in,
                                GBytes         *inline_part_bytes,
                                OstreeStaticDeltaOpenFlags flags,
                                const char     *expected_checksum,
                                GVariant    **out_part,
                                GCancellable *cancellable,
                                GError      **error)
{
  const gboolean trusted = (flags & OSTREE_STATIC_DELTA_OPEN_FLAGS_VARIANT_TRUSTED) > 0;
  const gboolean skip_checksum = (flags & OSTREE_STATIC_DELTA_OPEN_FLAGS_SKIP_CHECKSUM) > 0;

  /* We either take a fd or a GBytes reference */
  g_return_val_if_fail (G_IS_FILE_DESCRIPTOR_BASED (part_in) || inline_part_bytes != NULL, FALSE);
  g_return_val_if_fail (skip_checksum || expected_checksum != NULL, FALSE);

  g_autoptr(GChecksum) checksum = NULL;
  g_autoptr(GInputStream) checksum_in = NULL;
  GInputStream *source_in;
  if (!skip_checksum)
    {
      checksum = g_checksum_new (G_CHECKSUM_SHA256);
      checksum_in = (GInputStream*)ostree_checksum_input_stream_new (part_in, checksum);
      source_in = checksum_in;
    }
  else
    {
      source_in = part_in;
    }

  guint8 comptype;
  { guint8 buf[1];
    gsize bytes_read;
    /* First byte is compression type */
    if (!g_input_stream_read_all (source_in, buf, sizeof(buf), &bytes_read,
                                  cancellable, error))
      return glnx_prefix_error (error, "Reading initial compression flag byte");
    comptype = buf[0];
  }

  g_autoptr(GVariant) ret_part = NULL;
  switch (comptype)
    {
    case 0:
      if (!inline_part_bytes)
        {
          int part_fd = g_file_descriptor_based_get_fd ((GFileDescriptorBased*)part_in);

          /* No compression, no checksums - a fast path */
          if (!ot_variant_read_fd (part_fd, 1, G_VARIANT_TYPE (OSTREE_STATIC_DELTA_PART_PAYLOAD_FORMAT_V0),
                                   trusted, &ret_part, error))
            return FALSE;
        }
      else
        {
          g_autoptr(GBytes) content_bytes = g_bytes_new_from_bytes (inline_part_bytes, 1,
                                                                    g_bytes_get_size (inline_part_bytes) - 1);
          ret_part = g_variant_new_from_bytes (G_VARIANT_TYPE (OSTREE_STATIC_DELTA_PART_PAYLOAD_FORMAT_V0),
                                               content_bytes, trusted);
          g_variant_ref_sink (ret_part);
        }

      if (!skip_checksum)
        g_checksum_update (checksum, g_variant_get_data (ret_part),
                           g_variant_get_size (ret_part));

      break;
    case 'x':
      {
        g_autoptr(GConverter) decomp = (GConverter*) _ostree_lzma_decompressor_new ();
        g_autoptr(GInputStream) convin = g_converter_input_stream_new (source_in, decomp);
        g_autoptr(GBytes) buf = ot_map_anonymous_tmpfile_from_content (convin, cancellable, error);
        if (!buf)
          return FALSE;

        ret_part = g_variant_new_from_bytes (G_VARIANT_TYPE (OSTREE_STATIC_DELTA_PART_PAYLOAD_FORMAT_V0),
                                             buf, FALSE);
      }
      break;
    default:
      return glnx_throw (error, "Invalid compression type '%u'", comptype);
    }

  if (checksum)
    {
      const char *actual_checksum = g_checksum_get_string (checksum);
      g_assert (expected_checksum != NULL);
      if (strcmp (actual_checksum, expected_checksum) != 0)
        return glnx_throw (error, "Checksum mismatch in static delta part; expected=%s actual=%s",
                           expected_checksum, actual_checksum);
    }

  *out_part = g_steal_pointer (&ret_part);
  return TRUE;
}

/*
 * Displaying static delta parts
 */

static gboolean
show_one_part (OstreeRepo                    *self,
               gboolean                       swap_endian,
               const char                    *from,
               const char                    *to,
               GVariant                      *meta_entries,
               guint                          i,
               guint64                       *total_size_ref,
               guint64                       *total_usize_ref,
               GCancellable                  *cancellable,
               GError                      **error)
{
  g_autofree char *part_path = _ostree_get_relative_static_delta_part_path (from, to, i);

  guint32 version;
  guint64 size, usize;
  g_autoptr(GVariant) objects = NULL;
  g_variant_get_child (meta_entries, i, "(u@aytt@ay)", &version, NULL, &size, &usize, &objects);
  size = maybe_swap_endian_u64 (swap_endian, size);
  usize = maybe_swap_endian_u64 (swap_endian, usize);
  *total_size_ref += size;
  *total_usize_ref += usize;
  g_print ("PartMeta%u: nobjects=%u size=%" G_GUINT64_FORMAT " usize=%" G_GUINT64_FORMAT "\n",
           i, (guint)(g_variant_get_size (objects) / OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN), size, usize);

  glnx_autofd int part_fd = openat (self->repo_dir_fd, part_path, O_RDONLY | O_CLOEXEC);
  if (part_fd < 0)
    return glnx_throw_errno_prefix (error, "openat(%s)", part_path);
  g_autoptr(GInputStream) part_in = g_unix_input_stream_new (part_fd, FALSE);

  g_autoptr(GVariant) part = NULL;
  if (!_ostree_static_delta_part_open (part_in, NULL,
                                       OSTREE_STATIC_DELTA_OPEN_FLAGS_SKIP_CHECKSUM,
                                       NULL,
                                       &part,
                                       cancellable, error))
    return FALSE;

  { g_autoptr(GVariant) modes = NULL;
    g_autoptr(GVariant) xattrs = NULL;
    g_autoptr(GVariant) blob = NULL;
    g_autoptr(GVariant) ops = NULL;
    OstreeDeltaExecuteStats stats = { { 0, }, };

    g_variant_get (part, "(@a(uuu)@aa(ayay)@ay@ay)",
                   &modes, &xattrs, &blob, &ops);

    g_print ("PartPayload%u: nmodes=%" G_GUINT64_FORMAT
             " nxattrs=%" G_GUINT64_FORMAT
             " blobsize=%" G_GUINT64_FORMAT
             " opsize=%" G_GUINT64_FORMAT
             "\n",
             i,
             (guint64)g_variant_n_children (modes),
             (guint64)g_variant_n_children (xattrs),
             (guint64)g_variant_n_children (blob),
             (guint64)g_variant_n_children (ops));

    if (!_ostree_static_delta_part_execute (self, objects,
                                            part, TRUE,
                                            &stats, cancellable, error))
      return FALSE;

    { const guint *n_ops = stats.n_ops_executed;
      g_print ("PartPayloadOps%u: openspliceclose=%u open=%u write=%u setread=%u "
               "unsetread=%u close=%u bspatch=%u\n",
               i, n_ops[0], n_ops[1], n_ops[2], n_ops[3], n_ops[4], n_ops[5], n_ops[6]);
    }
  }

  return TRUE;
}

OstreeDeltaEndianness
_ostree_delta_get_endianness (GVariant *superblock,
                              gboolean *out_was_heuristic)
{
  g_autoptr(GVariant) delta_meta = g_variant_get_child_value (superblock, 0);
  g_autoptr(GVariantDict) delta_metadict = g_variant_dict_new (delta_meta);

  if (out_was_heuristic)
    *out_was_heuristic = FALSE;

  guint8 endianness_char;
  if (g_variant_dict_lookup (delta_metadict, "ostree.endianness", "y", &endianness_char))
    {
      switch (endianness_char)
        {
        case 'l':
          return OSTREE_DELTA_ENDIAN_LITTLE;
        case 'B':
          return OSTREE_DELTA_ENDIAN_BIG;
        default:
          return OSTREE_DELTA_ENDIAN_INVALID;
        }
    }

  if (out_was_heuristic)
    *out_was_heuristic = TRUE;

  guint64 total_size = 0;
  guint64 total_usize = 0;
  guint total_objects = 0;
  { g_autoptr(GVariant) meta_entries = NULL;
    gboolean is_byteswapped = FALSE;

    g_variant_get_child (superblock, 6, "@a" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT, &meta_entries);
    const guint n_parts = g_variant_n_children (meta_entries);

    for (guint i = 0; i < n_parts; i++)
      {
        g_autoptr(GVariant) objects = NULL;
        guint64 size, usize;
        guint n_objects;

        g_variant_get_child (meta_entries, i, "(u@aytt@ay)", NULL, NULL, &size, &usize, &objects);
        n_objects = (guint)(g_variant_get_size (objects) / OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN);

        total_objects += n_objects;
        total_size += size;
        total_usize += usize;

        if (size > usize)
          {
            double ratio = ((double)size)/((double)usize);

            /* This should really never happen where compressing things makes it more than 50% bigger.
             */
            if (ratio > 1.2)
              {
                is_byteswapped = TRUE;
                break;
              }
          }
      }

    if (!is_byteswapped)
      {
        /* If the average object size is greater than 4GiB, let's assume
         * we're dealing with opposite endianness.  I'm fairly confident
         * no one is going to be shipping peta- or exa- byte size ostree
         * deltas, period.  Past the gigabyte scale you really want
         * bittorrent or something.
         */
        if (total_objects > 0 && (total_size / total_objects) > G_MAXUINT32)
          {
            is_byteswapped = TRUE;
          }
      }

    if (is_byteswapped)
      {
        switch (G_BYTE_ORDER)
          {
          case G_BIG_ENDIAN:
            return OSTREE_DELTA_ENDIAN_LITTLE;
          case G_LITTLE_ENDIAN:
            return OSTREE_DELTA_ENDIAN_BIG;
          default:
            g_assert_not_reached ();
          }
      }

    return OSTREE_DELTA_ENDIAN_INVALID;
  }
}

gboolean
_ostree_delta_needs_byteswap (GVariant *superblock)
{
  switch (_ostree_delta_get_endianness (superblock, NULL))
    {
    case OSTREE_DELTA_ENDIAN_BIG:
      return G_BYTE_ORDER == G_LITTLE_ENDIAN;
    case OSTREE_DELTA_ENDIAN_LITTLE:
      return G_BYTE_ORDER == G_BIG_ENDIAN;
    default:
      return FALSE;
    }
}

gboolean
_ostree_repo_static_delta_delete (OstreeRepo                    *self,
                                  const char                    *delta_id,
                                  GCancellable                  *cancellable,
                                  GError                      **error)
{
  g_autofree char *from = NULL;
  g_autofree char *to = NULL;
  if (!_ostree_parse_delta_name (delta_id, &from, &to, error))
    return FALSE;

  g_autofree char *deltadir = _ostree_get_relative_static_delta_path (from, to, NULL);
  struct stat buf;
  if (fstatat (self->repo_dir_fd, deltadir, &buf, 0) != 0)
    {
      if (errno == ENOENT)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "Can't find delta %s", delta_id);
          return FALSE;
        }
      else
        return glnx_throw_errno_prefix (error, "fstatat(%s)", deltadir);
    }

  if (!glnx_shutil_rm_rf_at (self->repo_dir_fd, deltadir,
                             cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
_ostree_repo_static_delta_query_exists (OstreeRepo                    *self,
                                        const char                    *delta_id,
                                        gboolean                      *out_exists,
                                        GCancellable                  *cancellable,
                                        GError                      **error)
{
  g_autofree char *from = NULL;
  g_autofree char *to = NULL;
  if (!_ostree_parse_delta_name (delta_id, &from, &to, error))
    return FALSE;

  g_autofree char *superblock_path = _ostree_get_relative_static_delta_superblock_path (from, to);
  if (!glnx_fstatat_allow_noent (self->repo_dir_fd, superblock_path, NULL, 0, error))
    return FALSE;

  *out_exists = (errno == 0);
  return TRUE;
}

gboolean
_ostree_repo_static_delta_dump (OstreeRepo                    *self,
                                const char                    *delta_id,
                                GCancellable                  *cancellable,
                                GError                       **error)
{
  glnx_autofd int superblock_fd = -1;
  g_autoptr(GVariant) delta = NULL;
  g_autoptr(GVariant) delta_superblock = NULL;

  if (strchr (delta_id, '/'))
    {
      if (!glnx_openat_rdonly (AT_FDCWD, delta_id, TRUE, &superblock_fd, error))
        return FALSE;
    }
  else
    {
      g_autofree char *from = NULL;
      g_autofree char *to = NULL;
      if (!_ostree_parse_delta_name (delta_id, &from, &to, error))
        return FALSE;

      g_autofree char *superblock_path = _ostree_get_relative_static_delta_superblock_path (from, to);
      if (!glnx_openat_rdonly (self->repo_dir_fd, superblock_path, TRUE, &superblock_fd, error))
        return FALSE;
    }

  gboolean is_signed = _ostree_repo_static_delta_is_signed(self, superblock_fd, NULL, NULL);
  if (is_signed)
    {
      if (!ot_variant_read_fd (superblock_fd, 0, (GVariantType*)OSTREE_STATIC_DELTA_SIGNED_FORMAT,
                               TRUE, &delta, error))
        return FALSE;

      g_autoptr(GVariant) child = g_variant_get_child_value (delta, 1);
      g_autoptr(GBytes) bytes = g_variant_get_data_as_bytes(child);
      delta_superblock = g_variant_new_from_bytes ((GVariantType*)OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT,
                                                   bytes, FALSE);
    }
  else
    {
      if (!ot_variant_read_fd (superblock_fd, 0,
                               (GVariantType*)OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT,
                               TRUE, &delta_superblock, error))
        return FALSE;
    }

  g_print ("Delta: %s\n", delta_id);
  g_print ("Signed: %s\n", is_signed ? "yes" : "no");
  g_autoptr(GVariant) from_commit_v = NULL;
  g_variant_get_child (delta_superblock, 2, "@ay", &from_commit_v);
  g_autofree char *from_commit = NULL;
  if (g_variant_n_children (from_commit_v) > 0)
    {
      if (!ostree_checksum_bytes_peek_validate (from_commit_v, error))
        return FALSE;
      from_commit = ostree_checksum_from_bytes_v (from_commit_v);
      g_print ("From: %s\n", from_commit);
    }
  else
    {
      g_print ("From <scratch>\n");
    }
  g_autoptr(GVariant) to_commit_v = NULL;
  g_variant_get_child (delta_superblock, 3, "@ay", &to_commit_v);
  if (!ostree_checksum_bytes_peek_validate (to_commit_v, error))
    return FALSE;
  g_autofree char *to_commit = ostree_checksum_from_bytes_v (to_commit_v);
  g_print ("To: %s\n", to_commit);

  gboolean swap_endian = FALSE;
  OstreeDeltaEndianness endianness;
  { const char *endianness_description;
    gboolean was_heuristic;

    endianness = _ostree_delta_get_endianness (delta_superblock, &was_heuristic);

    switch (endianness)
      {
      case OSTREE_DELTA_ENDIAN_BIG:
        if (was_heuristic)
          endianness_description = "big (heuristic)";
        else
          endianness_description = "big";
        if (G_BYTE_ORDER == G_LITTLE_ENDIAN)
          swap_endian = TRUE;
        break;
      case OSTREE_DELTA_ENDIAN_LITTLE:
        if (was_heuristic)
          endianness_description = "little (heuristic)";
        else
          endianness_description = "little";
        if (G_BYTE_ORDER == G_BIG_ENDIAN)
          swap_endian = TRUE;
        break;
      case OSTREE_DELTA_ENDIAN_INVALID:
        endianness_description = "invalid";
        break;
      default:
        g_assert_not_reached ();
      }

    g_print ("Endianness: %s\n", endianness_description);
  }

  guint64 ts;
  g_variant_get_child (delta_superblock, 1, "t", &ts);
  g_print ("Timestamp: %" G_GUINT64_FORMAT "\n", GUINT64_FROM_BE (ts));

  g_autoptr(GVariant) recurse = NULL;
  g_variant_get_child (delta_superblock, 5, "@ay", &recurse);
  g_print ("Number of parents: %u\n", (guint)(g_variant_get_size (recurse) / (OSTREE_SHA256_DIGEST_LEN * 2)));

  g_autoptr(GVariant) fallback = NULL;
  g_variant_get_child (delta_superblock, 7, "@a" OSTREE_STATIC_DELTA_FALLBACK_FORMAT, &fallback);
  const guint n_fallback = g_variant_n_children (fallback);

  g_print ("Number of fallback entries: %u\n", n_fallback);

  guint64 total_size = 0, total_usize = 0;
  guint64 total_fallback_size = 0, total_fallback_usize = 0;
  for (guint i = 0; i < n_fallback; i++)
    {
      guint64 size, usize;
      g_autoptr(GVariant) checksum_v = NULL;
      char checksum[OSTREE_SHA256_STRING_LEN+1];
      g_variant_get_child (fallback, i, "(y@aytt)", NULL, &checksum_v, &size, &usize);
      ostree_checksum_inplace_from_bytes (ostree_checksum_bytes_peek (checksum_v), checksum);
      size = maybe_swap_endian_u64 (swap_endian, size);
      usize = maybe_swap_endian_u64 (swap_endian, usize);
      g_print ("  %s\n", checksum);
      total_fallback_size += size;
      total_fallback_usize += usize;
    }
  { g_autofree char *sizestr = g_format_size (total_fallback_size);
    g_autofree char *usizestr = g_format_size (total_fallback_usize);
    g_print ("Total Fallback Size: %" G_GUINT64_FORMAT " (%s)\n", total_fallback_size, sizestr);
    g_print ("Total Fallback Uncompressed Size: %" G_GUINT64_FORMAT " (%s)\n", total_fallback_usize, usizestr);
  }

  g_autoptr(GVariant) meta_entries = NULL;
  guint n_parts;

  g_variant_get_child (delta_superblock, 6, "@a" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT, &meta_entries);
  n_parts = g_variant_n_children (meta_entries);
  g_print ("Number of parts: %u\n", n_parts);

  for (guint i = 0; i < n_parts; i++)
    {
      if (!show_one_part (self, swap_endian, from_commit, to_commit, meta_entries, i,
                          &total_size, &total_usize,
                          cancellable, error))
        return FALSE;
    }

  { g_autofree char *sizestr = g_format_size (total_size);
    g_autofree char *usizestr = g_format_size (total_usize);
    g_print ("Total Part Size: %" G_GUINT64_FORMAT " (%s)\n", total_size, sizestr);
    g_print ("Total Part Uncompressed Size: %" G_GUINT64_FORMAT " (%s)\n", total_usize, usizestr);
  }

  { guint64 overall_size = total_size + total_fallback_size;
    guint64 overall_usize = total_usize + total_fallback_usize;
    g_autofree char *sizestr = g_format_size (overall_size);
    g_autofree char *usizestr = g_format_size (overall_usize);
    g_print ("Total Size: %" G_GUINT64_FORMAT " (%s)\n", overall_size, sizestr);
    g_print ("Total Uncompressed Size: %" G_GUINT64_FORMAT " (%s)\n", overall_usize, usizestr);
  }

  return TRUE;
}

/**
 * ostree_repo_static_delta_verify_signature:
 * @self: Repo
 * @delta_id: delta path
 * @sign: Signature engine used to check superblock
 * @out_success_message: (out) (nullable) (optional): success message
 * @error: Error
 *
 * Verify static delta file signature.
 *
 * Returns: TRUE if the signature of static delta file is valid using the
 * signature engine provided, FALSE otherwise.
 *
 * Since: 2020.7
 */
gboolean
ostree_repo_static_delta_verify_signature (OstreeRepo       *self,
                                           const char       *delta_id,
                                           OstreeSign       *sign,
                                           char            **out_success_message,
                                           GError          **error)
{
  g_autoptr(GVariant) delta_meta = NULL;
  glnx_autofd int delta_fd = -1;

  if (strchr (delta_id, '/'))
    {
      if (!glnx_openat_rdonly (AT_FDCWD, delta_id, TRUE, &delta_fd, error))
        return FALSE;
    }
  else
    {
      g_autofree char *from = NULL;
      g_autofree char *to = NULL;
      if (!_ostree_parse_delta_name (delta_id, &from, &to, error))
        return FALSE;

      g_autofree char *delta_path = _ostree_get_relative_static_delta_superblock_path (from, to);
      if (!glnx_openat_rdonly (self->repo_dir_fd, delta_path, TRUE, &delta_fd, error))
        return FALSE;
    }

  if (!_ostree_repo_static_delta_is_signed (self, delta_fd, NULL, error))
    return FALSE;

  return _ostree_repo_static_delta_verify_signature (self, delta_fd, sign, out_success_message, error);
}

static void
null_or_ptr_array_unref (GPtrArray *array)
{
  if (array != NULL)
    g_ptr_array_unref (array);
}

static gboolean
file_has_content (OstreeRepo   *repo,
                  const char   *subpath,
                  GBytes       *data,
                  GCancellable *cancellable)
{
  struct stat stbuf;
  glnx_autofd int existing_fd = -1;

  if (!glnx_fstatat (repo->repo_dir_fd, subpath, &stbuf, 0, NULL))
    return FALSE;

  if (stbuf.st_size != g_bytes_get_size (data))
    return FALSE;

  if (!glnx_openat_rdonly (repo->repo_dir_fd, subpath, TRUE, &existing_fd, NULL))
    return FALSE;

  g_autoptr(GBytes) existing_data = glnx_fd_readall_bytes (existing_fd, cancellable, NULL);
  if (existing_data == NULL)
    return FALSE;

  return g_bytes_equal (existing_data, data);
}

/**
 * ostree_repo_static_delta_reindex:
 * @repo: Repo
 * @flags: Flags affecting the indexing operation
 * @opt_to_commit: ASCII SHA256 checksum of target commit, or %NULL to index all targets
 * @cancellable: Cancellable
 * @error: Error
 *
 * The delta index for a particular commit lists all the existing deltas that can be used
 * when downloading that commit. This operation regenerates these indexes, either for
 * a particular commit (if @opt_to_commit is non-%NULL), or for all commits that
 * are reachable by an existing delta (if @opt_to_commit is %NULL).
 *
 * This is normally called automatically when the summary is updated in ostree_repo_regenerate_summary().
 *
 * Locking: shared
 */
gboolean
ostree_repo_static_delta_reindex (OstreeRepo                 *repo,
                                  OstreeStaticDeltaIndexFlags flags,
                                  const char                 *opt_to_commit,
                                  GCancellable               *cancellable,
                                  GError                    **error)
{
  g_autoptr(GPtrArray) all_deltas = NULL;
  g_autoptr(GHashTable) deltas_to_commit_ht = NULL; /* map: to checksum -> ptrarray of from checksums (or NULL) */
  gboolean opt_indexed_deltas;

  /* Protect against parallel prune operation */
  g_autoptr(OstreeRepoAutoLock) lock =
    _ostree_repo_auto_lock_push (repo, OSTREE_REPO_LOCK_SHARED, cancellable, error);
  if (!lock)
    return FALSE;

  /* Enusre that the "indexed-deltas" option is set on the config, so we know this when pulling */
  if (!ot_keyfile_get_boolean_with_default (repo->config, "core",
                                            "indexed-deltas", FALSE,
                                            &opt_indexed_deltas, error))
    return FALSE;

  if (!opt_indexed_deltas)
    {
      g_autoptr(GKeyFile) config = ostree_repo_copy_config (repo);
      g_key_file_set_boolean (config, "core", "indexed-deltas", TRUE);
      if (!ostree_repo_write_config (repo, config, error))
        return FALSE;
    }

  deltas_to_commit_ht = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)null_or_ptr_array_unref);

  if (opt_to_commit == NULL)
    {
      g_autoptr(GPtrArray) old_indexes = NULL;

      /* To ensure all old index files either is regenerated, or
       * removed, we initialize all existing indexes to NULL in the
       * hashtable. */
      if (!ostree_repo_list_static_delta_indexes (repo, &old_indexes, cancellable, error))
        return FALSE;

      for (int i = 0; i < old_indexes->len; i++)
        {
          const char *old_index = g_ptr_array_index (old_indexes, i);
          g_hash_table_insert (deltas_to_commit_ht, g_strdup (old_index), NULL);
        }
    }
  else
    {
      if (!ostree_validate_checksum_string (opt_to_commit, error))
        return FALSE;

      /* We ensure the specific old index either is regenerated, or removed */
      g_hash_table_insert (deltas_to_commit_ht, g_strdup (opt_to_commit), NULL);
    }

  if (!ostree_repo_list_static_delta_names (repo, &all_deltas, cancellable, error))
    return FALSE;

  for (int i = 0; i < all_deltas->len; i++)
    {
      const char *delta_name = g_ptr_array_index (all_deltas, i);
      g_autofree char *from = NULL;
      g_autofree char *to = NULL;
      GPtrArray *deltas_to_commit = NULL;

      if (!_ostree_parse_delta_name (delta_name, &from, &to, error))
        return FALSE;

      if (opt_to_commit != NULL && strcmp (to, opt_to_commit) != 0)
        continue;

      deltas_to_commit = g_hash_table_lookup (deltas_to_commit_ht, to);
      if (deltas_to_commit == NULL)
        {
          deltas_to_commit = g_ptr_array_new_with_free_func (g_free);
          g_hash_table_insert (deltas_to_commit_ht, g_steal_pointer (&to), deltas_to_commit);
        }

      g_ptr_array_add (deltas_to_commit, g_steal_pointer (&from));
    }

  GLNX_HASH_TABLE_FOREACH_KV (deltas_to_commit_ht, const char*, to, GPtrArray*, froms)
    {
      g_autofree char *index_path = _ostree_get_relative_static_delta_index_path (to);

      if (froms == NULL)
        {
          /* No index to this checksum seen, delete if it exists */

          g_debug ("Removing delta index for %s", to);
          if (!ot_ensure_unlinked_at (repo->repo_dir_fd, index_path, error))
            return FALSE;
        }
      else
        {
          g_auto(GVariantDict) index_builder = OT_VARIANT_BUILDER_INITIALIZER;
          g_auto(GVariantDict) deltas_builder = OT_VARIANT_BUILDER_INITIALIZER;
          g_autoptr(GVariant) index_variant = NULL;
          g_autoptr(GBytes) index = NULL;

          /* We sort on from here so that the index file is reproducible */
          g_ptr_array_sort (froms, (GCompareFunc)g_strcmp0);

          g_variant_dict_init (&deltas_builder, NULL);

          for (int i = 0; i < froms->len; i++)
            {
              const char *from = g_ptr_array_index (froms, i);
              g_autofree char *delta_name = NULL;
              GVariant *digest;

              digest = _ostree_repo_static_delta_superblock_digest (repo, from, to, cancellable, error);
              if (digest == NULL)
                return FALSE;

              if (from != NULL)
                delta_name = g_strconcat (from, "-", to, NULL);
              else
                delta_name = g_strdup (to);

              g_variant_dict_insert_value (&deltas_builder, delta_name, digest);
            }

          /* The toplevel of the index is an a{sv} for extensibility, and we use same key name (and format) as when
           * storing deltas in the summary. */
          g_variant_dict_init (&index_builder, NULL);

          g_variant_dict_insert_value (&index_builder, OSTREE_SUMMARY_STATIC_DELTAS, g_variant_dict_end (&deltas_builder));

          index_variant = g_variant_ref_sink (g_variant_dict_end (&index_builder));
          index = g_variant_get_data_as_bytes (index_variant);

          g_autofree char *index_dirname = g_path_get_dirname (index_path);
          if (!glnx_shutil_mkdir_p_at (repo->repo_dir_fd, index_dirname, DEFAULT_DIRECTORY_MODE, cancellable, error))
            return FALSE;

          /* delta indexes are generally small and static, so reading it back and comparing is cheap, and it will
             lower the write load (and particular sync-load) on the disk during reindexing (i.e. summary updates), */
          if (file_has_content (repo, index_path, index, cancellable))
            continue;

          g_debug ("Updating delta index for %s", to);
          if (!glnx_file_replace_contents_at (repo->repo_dir_fd, index_path,
                                              g_bytes_get_data (index, NULL), g_bytes_get_size (index),
                                              0, cancellable, error))
            return FALSE;
        }
    }

  return TRUE;
}
