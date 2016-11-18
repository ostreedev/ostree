/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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
  gsize n = g_variant_n_children (array);
  guint n_checksums;

  n_checksums = n / OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN;

  if (G_UNLIKELY(n > (G_MAXUINT32/OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN) ||
                 (n_checksums * OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN) != n))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid checksum array length %" G_GSIZE_FORMAT, n);
      return FALSE;
    }

  *out_checksums_array = (gpointer)g_variant_get_data (array);
  *out_n_checksums = n_checksums;

  return TRUE;
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
  gboolean ret = FALSE;
  g_autoptr(GPtrArray) ret_deltas = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;

  ret_deltas = g_ptr_array_new_with_free_func (g_free);

  if (g_file_query_exists (self->deltas_dir, NULL))
    {
      dir_enum = g_file_enumerate_children (self->deltas_dir, OSTREE_GIO_FAST_QUERYINFO,
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            NULL, error);
      if (!dir_enum)
        goto out;

      while (TRUE)
        {
          g_autoptr(GFileEnumerator) dir_enum2 = NULL;
          GFileInfo *file_info;
          GFile *child;

          if (!g_file_enumerator_iterate (dir_enum, &file_info, &child,
                                          NULL, error))
            goto out;
          if (file_info == NULL)
            break;

          if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
            continue;


          dir_enum2 = g_file_enumerate_children (child, OSTREE_GIO_FAST_QUERYINFO,
                                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                 NULL, error);
          if (!dir_enum2)
            goto out;

          while (TRUE)
            {
              GFileInfo *file_info2;
              GFile *child2;
              const char *name1;
              const char *name2;

              if (!g_file_enumerator_iterate (dir_enum2, &file_info2, &child2,
                                              NULL, error))
                goto out;
              if (file_info2 == NULL)
                break;

              if (g_file_info_get_file_type (file_info2) != G_FILE_TYPE_DIRECTORY)
                continue;

              name1 = g_file_info_get_name (file_info);
              name2 = g_file_info_get_name (file_info2);

              {
                g_autoptr(GFile) meta_path = g_file_get_child (child2, "superblock");

                if (g_file_query_exists (meta_path, NULL))
                  {
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
            }
        }
    }

  ret = TRUE;
  if (out_deltas)
    *out_deltas = g_steal_pointer (&ret_deltas);
 out:
  return ret;
}

gboolean
_ostree_repo_static_delta_part_have_all_objects (OstreeRepo             *repo,
                                                 GVariant               *checksum_array,
                                                 gboolean               *out_have_all,
                                                 GCancellable           *cancellable,
                                                 GError                **error)
{
  gboolean ret = FALSE;
  guint8 *checksums_data;
  guint i,n_checksums;
  gboolean have_object = TRUE;

  if (!_ostree_static_delta_parse_checksum_array (checksum_array,
                                                  &checksums_data,
                                                  &n_checksums,
                                                  error))
    goto out;

  for (i = 0; i < n_checksums; i++)
    {
      guint8 objtype = *checksums_data;
      const guint8 *csum = checksums_data + 1;
      char tmp_checksum[OSTREE_SHA256_STRING_LEN+1];

      if (G_UNLIKELY(!ostree_validate_structureof_objtype (objtype, error)))
        goto out;

      ostree_checksum_inplace_from_bytes (csum, tmp_checksum);

      if (!ostree_repo_has_object (repo, (OstreeObjectType) objtype, tmp_checksum,
                                   &have_object, cancellable, error))
        goto out;

      if (!have_object)
        break;

      checksums_data += OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN;
    }

  ret = TRUE;
  *out_have_all = have_object;
 out:
  return ret;
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
  gboolean ret = FALSE;
  guint i, n;
  const char *dir_or_file_path = NULL;
  glnx_fd_close int meta_fd = -1;
  glnx_fd_close int dfd = -1;
  g_autoptr(GVariant) meta = NULL;
  g_autoptr(GVariant) headers = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(GVariant) fallback = NULL;
  g_autofree char *to_checksum = NULL;
  g_autofree char *from_checksum = NULL;
  g_autofree char *basename = NULL;

  dir_or_file_path = gs_file_get_path_cached (dir_or_file);

  /* First, try opening it as a directory */
  dfd = glnx_opendirat_with_errno (AT_FDCWD, dir_or_file_path, TRUE);
  if (dfd < 0)
    {
      if (errno != ENOTDIR)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
      else
        {
          g_autofree char *dir = dirname (g_strdup (dir_or_file_path));
          basename = g_path_get_basename (dir_or_file_path);

          if (!glnx_opendirat (AT_FDCWD, dir, TRUE, &dfd, error))
            goto out;
        }
    }
  else
    basename = g_strdup ("superblock");

  meta_fd = openat (dfd, basename, O_RDONLY | O_CLOEXEC);
  if (meta_fd < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }
  
  if (!ot_util_variant_map_fd (meta_fd, 0, G_VARIANT_TYPE (OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT),
                               FALSE, &meta, error))
    goto out;

  /* Parsing OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT */

  metadata = g_variant_get_child_value (meta, 0);

  /* Write the to-commit object */
  {
    g_autoptr(GVariant) to_csum_v = NULL;
    g_autoptr(GVariant) from_csum_v = NULL;
    g_autoptr(GVariant) to_commit = NULL;
    gboolean have_to_commit;
    gboolean have_from_commit;

    to_csum_v = g_variant_get_child_value (meta, 3);
    if (!ostree_validate_structureof_csum_v (to_csum_v, error))
      goto out;
    to_checksum = ostree_checksum_from_bytes_v (to_csum_v);

    from_csum_v = g_variant_get_child_value (meta, 2);
    if (g_variant_n_children (from_csum_v) > 0)
      {
        if (!ostree_validate_structureof_csum_v (from_csum_v, error))
          goto out;
        from_checksum = ostree_checksum_from_bytes_v (from_csum_v);

        if (!ostree_repo_has_object (self, OSTREE_OBJECT_TYPE_COMMIT, from_checksum,
                                     &have_from_commit, cancellable, error))
          goto out;

        if (!have_from_commit)
          {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Commit %s, which is the delta source, is not in repository", from_checksum);
            goto out;
          }
      }

    if (!ostree_repo_has_object (self, OSTREE_OBJECT_TYPE_COMMIT, to_checksum,
                                 &have_to_commit, cancellable, error))
      goto out;
    
    if (!have_to_commit)
      {
        g_autofree char *detached_path = _ostree_get_relative_static_delta_path (from_checksum, to_checksum, "commitmeta");
        g_autoptr(GVariant) detached_data = NULL;

        detached_data = g_variant_lookup_value (metadata, detached_path, G_VARIANT_TYPE("a{sv}"));
        if (detached_data && !ostree_repo_write_commit_detached_metadata (self,
                                                                          to_checksum,
                                                                          detached_data,
                                                                          cancellable,
                                                                          error))
          goto out;

        to_commit = g_variant_get_child_value (meta, 4);
        if (!ostree_repo_write_metadata (self, OSTREE_OBJECT_TYPE_COMMIT,
                                         to_checksum, to_commit, NULL,
                                         cancellable, error))
          goto out;
      }
  }

  fallback = g_variant_get_child_value (meta, 7);
  if (g_variant_n_children (fallback) > 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Cannot execute delta offline: contains nonempty http fallback entries");
      goto out;
    }

  headers = g_variant_get_child_value (meta, 6);
  n = g_variant_n_children (headers);
  for (i = 0; i < n; i++)
    {
      guint32 version;
      guint64 size;
      guint64 usize;
      const guchar *csum;
      char checksum[OSTREE_SHA256_STRING_LEN+1];
      gboolean have_all;
      g_autoptr(GInputStream) part_in = NULL;
      g_autoptr(GVariant) inline_part_data = NULL;
      g_autoptr(GVariant) header = NULL;
      g_autoptr(GVariant) csum_v = NULL;
      g_autoptr(GVariant) objects = NULL;
      g_autoptr(GVariant) part = NULL;
      g_autofree char *deltapart_path = NULL;
      OstreeStaticDeltaOpenFlags delta_open_flags = 
        skip_validation ? OSTREE_STATIC_DELTA_OPEN_FLAGS_SKIP_CHECKSUM : 0;

      header = g_variant_get_child_value (headers, i);
      g_variant_get (header, "(u@aytt@ay)", &version, &csum_v, &size, &usize, &objects);

      if (version > OSTREE_DELTAPART_VERSION)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Delta part has too new version %u", version);
          goto out;
        }

      if (!_ostree_repo_static_delta_part_have_all_objects (self, objects, &have_all,
                                                            cancellable, error))
        goto out;

      /* If we already have these objects, don't bother executing the
       * static delta.
       */
      if (have_all)
        continue;

      csum = ostree_checksum_bytes_peek_validate (csum_v, error);
      if (!csum)
        goto out;
      ostree_checksum_inplace_from_bytes (csum, checksum);

      deltapart_path =
        _ostree_get_relative_static_delta_part_path (from_checksum, to_checksum, i);

      inline_part_data = g_variant_lookup_value (metadata, deltapart_path, G_VARIANT_TYPE("(yay)"));
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
            goto out;
        }
      else
        {
          g_autofree char *relpath = g_strdup_printf ("%u", i); /* TODO avoid malloc here */
          glnx_fd_close int part_fd = openat (dfd, relpath, O_RDONLY | O_CLOEXEC);
          if (part_fd < 0)
            {
              glnx_set_error_from_errno (error);
              g_prefix_error (error, "Opening deltapart '%s': ", deltapart_path);
              goto out;
            }

          part_in = g_unix_input_stream_new (part_fd, FALSE);

          if (!_ostree_static_delta_part_open (part_in, NULL, 
                                               delta_open_flags,
                                               checksum,
                                               &part,
                                               cancellable, error))
            goto out;
        }

      if (!_ostree_static_delta_part_execute (self, objects, part, skip_validation,
                                              FALSE, NULL,
                                              cancellable, error))
        {
          g_prefix_error (error, "Executing delta part %i: ", i);
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
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
  gboolean ret = FALSE;
  const gboolean trusted = (flags & OSTREE_STATIC_DELTA_OPEN_FLAGS_VARIANT_TRUSTED) > 0;
  const gboolean skip_checksum = (flags & OSTREE_STATIC_DELTA_OPEN_FLAGS_SKIP_CHECKSUM) > 0;
  gsize bytes_read;
  guint8 comptype;
  g_autoptr(GChecksum) checksum = NULL;
  g_autoptr(GInputStream) checksum_in = NULL;
  g_autoptr(GVariant) ret_part = NULL;
  GInputStream *source_in;

  /* We either take a fd or a GBytes reference */
  g_return_val_if_fail (G_IS_FILE_DESCRIPTOR_BASED (part_in) || inline_part_bytes != NULL, FALSE);
  g_return_val_if_fail (skip_checksum || expected_checksum != NULL, FALSE);

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

  { guint8 buf[1];
    /* First byte is compression type */
    if (!g_input_stream_read_all (source_in, buf, sizeof(buf), &bytes_read,
                                  cancellable, error))
      {
        g_prefix_error (error, "Reading initial compression flag byte: ");
      goto out;
      }
    comptype = buf[0];
  }

  switch (comptype)
    {
    case 0:
      if (!inline_part_bytes)
        {
          int part_fd = g_file_descriptor_based_get_fd ((GFileDescriptorBased*)part_in);

          /* No compression, no checksums - a fast path */
          if (!ot_util_variant_map_fd (part_fd, 1, G_VARIANT_TYPE (OSTREE_STATIC_DELTA_PART_PAYLOAD_FORMAT_V0),
                                       trusted, &ret_part, error))
            goto out;
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
        g_autofree char *tmppath = g_strdup ("/var/tmp/ostree-delta-XXXXXX");
        g_autoptr(GConverter) decomp = (GConverter*) _ostree_lzma_decompressor_new ();
        g_autoptr(GInputStream) convin = g_converter_input_stream_new (source_in, decomp);
        g_autoptr(GOutputStream) unpacked_out = NULL;
        glnx_fd_close int unpacked_fd = -1;
        gssize n_bytes_written;

        unpacked_fd = g_mkstemp_full (tmppath, O_RDWR | O_CLOEXEC, 0640);
        if (unpacked_fd < 0)
          {
            glnx_set_error_from_errno (error);
            goto out;
          }
        
        /* Now make it autocleanup on process exit - in the future, we
         * should consider caching unpacked deltas as well.
         */
        if (unlink (tmppath) < 0)
          {
            glnx_set_error_from_errno (error);
            goto out;
          }
        
        unpacked_out = g_unix_output_stream_new (unpacked_fd, FALSE);

        n_bytes_written = g_output_stream_splice (unpacked_out, convin,
                                                  G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                                  G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                                  cancellable, error);
        if (n_bytes_written < 0)
          goto out;

        if (!ot_util_variant_map_fd (unpacked_fd, 0, G_VARIANT_TYPE (OSTREE_STATIC_DELTA_PART_PAYLOAD_FORMAT_V0),
                                     trusted, &ret_part, error))
          goto out;
      }
      break;
    default:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid compression type '%u'", comptype);
      goto out;
    }

  if (checksum)
    {
      const char *actual_checksum = g_checksum_get_string (checksum);
      g_assert (expected_checksum != NULL);
      if (strcmp (actual_checksum, expected_checksum) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Checksum mismatch in static delta part; expected=%s actual=%s",
                       expected_checksum, actual_checksum);
          goto out;
        }
    }
        
  ret = TRUE;
  *out_part = g_steal_pointer (&ret_part);
 out:
  return ret;
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
  gboolean ret = FALSE;
  guint32 version;
  guint64 size, usize;
  g_autoptr(GVariant) objects = NULL;
  g_autoptr(GInputStream) part_in = NULL;
  g_autoptr(GVariant) part = NULL;
  g_autofree char *part_path = _ostree_get_relative_static_delta_part_path (from, to, i);
  gint part_fd = -1;

  g_variant_get_child (meta_entries, i, "(u@aytt@ay)", &version, NULL, &size, &usize, &objects);
  size = maybe_swap_endian_u64 (swap_endian, size);
  usize = maybe_swap_endian_u64 (swap_endian, usize);
  *total_size_ref += size;
  *total_usize_ref += usize;
  g_print ("PartMeta%u: nobjects=%u size=%" G_GUINT64_FORMAT " usize=%" G_GUINT64_FORMAT "\n",
           i, (guint)(g_variant_get_size (objects) / OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN), size, usize);

  part_fd = openat (self->repo_dir_fd, part_path, O_RDONLY | O_CLOEXEC);
  if (part_fd < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  part_in = g_unix_input_stream_new (part_fd, FALSE);
  
  if (!_ostree_static_delta_part_open (part_in, NULL, 
                                       OSTREE_STATIC_DELTA_OPEN_FLAGS_SKIP_CHECKSUM,
                                       NULL,
                                       &part,
                                       cancellable, error))
    goto out;

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
                                            part, TRUE, TRUE,
                                            &stats, cancellable, error))
      goto out;

    { const guint *n_ops = stats.n_ops_executed;
      g_print ("PartPayloadOps%u: openspliceclose=%u open=%u write=%u setread=%u "
               "unsetread=%u close=%u bspatch=%u\n",
               i, n_ops[0], n_ops[1], n_ops[2], n_ops[3], n_ops[4], n_ops[5], n_ops[6]);
    }
  }
    
  ret = TRUE;
 out:
  return ret;
}

OstreeDeltaEndianness
_ostree_delta_get_endianness (GVariant *superblock,
                              gboolean *out_was_heuristic)
{
  guint8 endianness_char;
  g_autoptr(GVariant) delta_meta = NULL;
  g_autoptr(GVariantDict) delta_metadict = NULL;
  guint64 total_size = 0;
  guint64 total_usize = 0;
  guint total_objects = 0;

  delta_meta = g_variant_get_child_value (superblock, 0);
  delta_metadict = g_variant_dict_new (delta_meta);

  if (out_was_heuristic)
    *out_was_heuristic = FALSE;

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

  { g_autoptr(GVariant) meta_entries = NULL;
    guint n_parts;
    guint i;
    gboolean is_byteswapped = FALSE;

    g_variant_get_child (superblock, 6, "@a" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT, &meta_entries);
    n_parts = g_variant_n_children (meta_entries);

    for (i = 0; i < n_parts; i++)
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
        if ((total_size / total_objects) > G_MAXUINT32)
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
  gboolean ret = FALSE;
  g_autofree char *from = NULL;
  g_autofree char *to = NULL;
  g_autofree char *deltadir = NULL;
  struct stat buf;

  if (!_ostree_parse_delta_name (delta_id, &from, &to, error))
    goto out;

  deltadir = _ostree_get_relative_static_delta_path (from, to, NULL);

  if (fstatat (self->repo_dir_fd, deltadir, &buf, 0) != 0)
    {
      if (errno == ENOENT)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "Can't find delta %s", delta_id);
      else
        glnx_set_error_from_errno (error);

      goto out;
    }

  if (!glnx_shutil_rm_rf_at (self->repo_dir_fd, deltadir,
                             cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
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
  g_autofree char *superblock_path = NULL;
  struct stat stbuf;

  if (!_ostree_parse_delta_name (delta_id, &from, &to, error))
    return FALSE;

  superblock_path = _ostree_get_relative_static_delta_superblock_path (from, to);

  if (fstatat (self->repo_dir_fd, superblock_path, &stbuf, 0) < 0)
    {
      if (errno == ENOENT)
        {
          *out_exists = FALSE;
          return TRUE;
        }
      else
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }
  *out_exists = TRUE;
  return TRUE;
}

gboolean
_ostree_repo_static_delta_dump (OstreeRepo                    *self,
                                const char                    *delta_id,
                                GCancellable                  *cancellable,
                                GError                       **error)
{
  gboolean ret = FALSE;
  g_autofree char *from = NULL; 
  g_autofree char *to = NULL;
  g_autofree char *superblock_path = NULL;
  g_autoptr(GVariant) delta_superblock = NULL;
  guint64 total_size = 0, total_usize = 0;
  guint64 total_fallback_size = 0, total_fallback_usize = 0;
  guint i;
  OstreeDeltaEndianness endianness;
  gboolean swap_endian = FALSE;

  if (!_ostree_parse_delta_name (delta_id, &from, &to, error))
    goto out;

  superblock_path = _ostree_get_relative_static_delta_superblock_path (from, to);

  if (!ot_util_variant_map_at (self->repo_dir_fd, superblock_path,
                               (GVariantType*)OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT,
                               OT_VARIANT_MAP_TRUSTED, &delta_superblock, error))
    goto out;

  { g_autofree char *variant_string = g_variant_print (delta_superblock, 1);
    g_print ("%s\n", variant_string);
  }

  g_print ("Delta: %s\n", delta_id);
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
  { guint64 ts;
    g_variant_get_child (delta_superblock, 1, "t", &ts);
    g_print ("Timestamp: %" G_GUINT64_FORMAT "\n", GUINT64_FROM_BE (ts));
  }
  { g_autoptr(GVariant) recurse = NULL;
    g_variant_get_child (delta_superblock, 5, "@ay", &recurse);
    g_print ("Number of parents: %u\n", (guint)(g_variant_get_size (recurse) / (OSTREE_SHA256_DIGEST_LEN * 2)));
  }
  { g_autoptr(GVariant) fallback = NULL;
    guint n_fallback;

    g_variant_get_child (delta_superblock, 7, "@a" OSTREE_STATIC_DELTA_FALLBACK_FORMAT, &fallback);
    n_fallback = g_variant_n_children (fallback);

    g_print ("Number of fallback entries: %u\n", n_fallback);

    for (i = 0; i < n_fallback; i++)
      {
        guint64 size, usize;
        g_variant_get_child (fallback, i, "(y@aytt)", NULL, NULL, &size, &usize);
        size = maybe_swap_endian_u64 (swap_endian, size);
        usize = maybe_swap_endian_u64 (swap_endian, usize);
        total_fallback_size += size;
        total_fallback_usize += usize;
      }
    { g_autofree char *sizestr = g_format_size (total_fallback_size);
      g_autofree char *usizestr = g_format_size (total_fallback_usize);
      g_print ("Total Fallback Size: %" G_GUINT64_FORMAT " (%s)\n", total_fallback_size, sizestr);
      g_print ("Total Fallback Uncompressed Size: %" G_GUINT64_FORMAT " (%s)\n", total_fallback_usize, usizestr);
    }
  }
  { g_autoptr(GVariant) meta_entries = NULL;
    guint n_parts;

    g_variant_get_child (delta_superblock, 6, "@a" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT, &meta_entries);
    n_parts = g_variant_n_children (meta_entries);
    g_print ("Number of parts: %u\n", n_parts);

    for (i = 0; i < n_parts; i++)
      {
        if (!show_one_part (self, swap_endian, from, to, meta_entries, i,
                            &total_size, &total_usize,
                            cancellable, error))
          goto out;
      }
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

  ret = TRUE;
 out:
  return ret;
}
