/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013,2014 Colin Walters <walters@verbum.org>
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

#include <string.h>
#include <stdlib.h>
#include <gio/gunixoutputstream.h>
#include <gio/gmemoryoutputstream.h>

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-lzma-compressor.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-diff.h"
#include "ostree-rollsum.h"
#include "otutil.h"
#include "ostree-varint.h"
#include "bsdiff/bsdiff.h"

#define CONTENT_SIZE_SIMILARITY_THRESHOLD_PERCENT (30)

typedef struct {
  guint64 uncompressed_size;
  GPtrArray *objects;
  GString *payload;
  GString *operations;
  GHashTable *mode_set; /* GVariant(uuu) -> offset */
  GPtrArray *modes;
  GHashTable *xattr_set; /* GVariant(ayay) -> offset */
  GPtrArray *xattrs;
} OstreeStaticDeltaPartBuilder;

typedef struct {
  GPtrArray *parts;
  GPtrArray *fallback_objects;
  guint64 loose_compressed_size;
  guint64 min_fallback_size_bytes;
  guint64 max_bsdiff_size_bytes;
  guint64 max_chunk_size_bytes;
  guint64 rollsum_size;
  guint n_rollsum;
  guint n_bsdiff;
  guint n_fallback;
  gboolean swap_endian;
} OstreeStaticDeltaBuilder;

typedef enum {
  DELTAOPT_FLAG_NONE = (1 << 0),
  DELTAOPT_FLAG_DISABLE_BSDIFF = (1 << 1),
  DELTAOPT_FLAG_VERBOSE = (1 << 2)
} DeltaOpts;

static void
ostree_static_delta_part_builder_unref (OstreeStaticDeltaPartBuilder *part_builder)
{
  if (part_builder->objects)
    g_ptr_array_unref (part_builder->objects);
  if (part_builder->payload)
    g_string_free (part_builder->payload, TRUE);
  if (part_builder->operations)
    g_string_free (part_builder->operations, TRUE);
  g_hash_table_unref (part_builder->mode_set);
  g_ptr_array_unref (part_builder->modes);
  g_hash_table_unref (part_builder->xattr_set);
  g_ptr_array_unref (part_builder->xattrs);
  g_free (part_builder);
}

static guint
mode_chunk_hash (const void *vp)
{
  GVariant *v = (GVariant*)vp;
  guint uid, gid, mode;
  g_variant_get (v, "(uuu)", &uid, &gid, &mode);
  return uid + gid + mode;
}

static gboolean
mode_chunk_equals (const void *one, const void *two)
{
  GVariant *v1 = (GVariant*)one;
  GVariant *v2 = (GVariant*)two;
  guint uid1, gid1, mode1;
  guint uid2, gid2, mode2;

  g_variant_get (v1, "(uuu)", &uid1, &gid1, &mode1);
  g_variant_get (v2, "(uuu)", &uid2, &gid2, &mode2);

  return uid1 == uid2 && gid1 == gid2 && mode1 == mode2;
}

static guint
bufhash (const void *b, gsize len)
{
  const signed char *p, *e;
  guint32 h = 5381;

  for (p = (signed char *)b, e = (signed char *)b + len; p != e; p++)
    h = (h << 5) + h + *p;

  return h;
}

static guint
xattr_chunk_hash (const void *vp)
{
  GVariant *v = (GVariant*)vp;
  gsize n = g_variant_n_children (v);
  guint i;
  guint32 h = 5381;

  for (i = 0; i < n; i++)
    {
      const guint8* name;
      const guint8* value_data;
      GVariant *value = NULL;
      gsize value_len;

      g_variant_get_child (v, i, "(^&ay@ay)",
                           &name, &value);
      value_data = g_variant_get_fixed_array (value, &value_len, 1);
      
      h += g_str_hash (name);
      h += bufhash (value_data, value_len);
    }
      
  return h;
}

static gboolean
xattr_chunk_equals (const void *one, const void *two)
{
  GVariant *v1 = (GVariant*)one;
  GVariant *v2 = (GVariant*)two;
  gsize l1 = g_variant_get_size (v1);
  gsize l2 = g_variant_get_size (v2);

  if (l1 != l2)
    return FALSE;

  return memcmp (g_variant_get_data (v1), g_variant_get_data (v2), l1) == 0;
}

static OstreeStaticDeltaPartBuilder *
allocate_part (OstreeStaticDeltaBuilder *builder)
{
  OstreeStaticDeltaPartBuilder *part = g_new0 (OstreeStaticDeltaPartBuilder, 1);
  part->objects = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
  part->payload = g_string_new (NULL);
  part->operations = g_string_new (NULL);
  part->uncompressed_size = 0;
  part->mode_set = g_hash_table_new_full (mode_chunk_hash, mode_chunk_equals,
                                          (GDestroyNotify)g_variant_unref, NULL);
  part->modes = g_ptr_array_new ();
  part->xattr_set = g_hash_table_new_full (xattr_chunk_hash, xattr_chunk_equals,
                                           (GDestroyNotify)g_variant_unref, NULL);
  part->xattrs = g_ptr_array_new ();
  g_ptr_array_add (builder->parts, part);
  return part;
}

static gsize
allocate_part_buffer_space (OstreeStaticDeltaPartBuilder  *current_part,
                            guint                          len)
{
  gsize empty_space;
  gsize old_len;

  old_len = current_part->payload->len;
  empty_space = current_part->payload->allocated_len - current_part->payload->len;

  if (empty_space < len)
    {
      gsize origlen;
      origlen = current_part->payload->len;
      g_string_set_size (current_part->payload, current_part->payload->allocated_len + (len - empty_space));
      current_part->payload->len = origlen;
    }

  return old_len;
}

static gsize
write_unique_variant_chunk (OstreeStaticDeltaPartBuilder *current_part,
                            GHashTable                   *hash,
                            GPtrArray                    *ordered,
                            GVariant                     *key)
{
  gpointer target_offsetp;
  gsize offset;

  if (g_hash_table_lookup_extended (hash, key, NULL, &target_offsetp))
    return GPOINTER_TO_UINT (target_offsetp);

  offset = ordered->len;
  target_offsetp = GUINT_TO_POINTER (offset);
  g_hash_table_insert (hash, g_variant_ref (key), target_offsetp);
  g_ptr_array_add (ordered, key);

  return offset;
}

static GBytes *
objtype_checksum_array_new (GPtrArray *objects)
{
  guint i;
  GByteArray *ret = g_byte_array_new ();

  for (i = 0; i < objects->len; i++)
    {
      GVariant *serialized_key = objects->pdata[i];
      OstreeObjectType objtype;
      const char *checksum;
      guint8 csum[OSTREE_SHA256_DIGEST_LEN];
      guint8 objtype_v;
        
      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);
      objtype_v = (guint8) objtype;

      ostree_checksum_inplace_to_bytes (checksum, csum);

      g_byte_array_append (ret, &objtype_v, 1);
      g_byte_array_append (ret, csum, sizeof (csum));
    }
  return g_byte_array_free_to_bytes (ret);
}

static gboolean
splice_stream_to_payload (OstreeStaticDeltaPartBuilder  *current_part,
                          GInputStream                  *istream,
                          GCancellable                  *cancellable,
                          GError                       **error)
{
  gboolean ret = FALSE;
  const guint readlen = 4096;
  gsize bytes_read;

  while (TRUE)
    {
      allocate_part_buffer_space (current_part, readlen);

      if (!g_input_stream_read_all (istream,
                                    current_part->payload->str + current_part->payload->len,
                                    readlen,
                                    &bytes_read,
                                    cancellable, error))
        goto out;
      if (bytes_read == 0)
        break;
          
      current_part->payload->len += bytes_read;
    }

  ret = TRUE;
 out:
  return ret;
}

static void
write_content_mode_xattrs (OstreeRepo                       *repo,
                           OstreeStaticDeltaPartBuilder     *current_part,
                           GFileInfo                        *content_finfo,
                           GVariant                         *content_xattrs,
                           gsize                            *out_mode_offset,
                           gsize                            *out_xattr_offset)
{
  guint32 uid =
    g_file_info_get_attribute_uint32 (content_finfo, "unix::uid");
  guint32 gid =
    g_file_info_get_attribute_uint32 (content_finfo, "unix::gid");
  guint32 mode =
    g_file_info_get_attribute_uint32 (content_finfo, "unix::mode");
  g_autoptr(GVariant) modev
    = g_variant_ref_sink (g_variant_new ("(uuu)", 
                                         GUINT32_TO_BE (uid),
                                         GUINT32_TO_BE (gid),
                                         GUINT32_TO_BE (mode)));

  *out_mode_offset = write_unique_variant_chunk (current_part,
                                                 current_part->mode_set,
                                                 current_part->modes,
                                                 modev);
  *out_xattr_offset = write_unique_variant_chunk (current_part,
                                                  current_part->xattr_set,
                                                  current_part->xattrs,
                                                  content_xattrs);
}

static gboolean
process_one_object (OstreeRepo                       *repo,
                    OstreeStaticDeltaBuilder         *builder,
                    OstreeStaticDeltaPartBuilder    **current_part_val,
                    const char                       *checksum,
                    OstreeObjectType                  objtype,
                    GCancellable                     *cancellable,
                    GError                          **error)
{
  gboolean ret = FALSE;
  guint64 content_size;
  g_autoptr(GInputStream) content_stream = NULL;
  g_autoptr(GFileInfo) content_finfo = NULL;
  g_autoptr(GVariant) content_xattrs = NULL;
  guint64 compressed_size;
  OstreeStaticDeltaPartBuilder *current_part = *current_part_val;

  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    {
      if (!ostree_repo_load_object_stream (repo, objtype, checksum,
                                           &content_stream, &content_size,
                                           cancellable, error))
        goto out;
    }
  else
    {
      if (!ostree_repo_load_file (repo, checksum, &content_stream,
                                  &content_finfo, &content_xattrs,
                                  cancellable, error))
        goto out;
      content_size = g_file_info_get_size (content_finfo);
    }
  
  /* Check to see if this delta is maximum size */
  if (current_part->objects->len > 0 &&
      current_part->payload->len + content_size > builder->max_chunk_size_bytes)
    {
      *current_part_val = current_part = allocate_part (builder);
    } 

  if (!ostree_repo_query_object_storage_size (repo, objtype, checksum,
                                              &compressed_size,
                                              cancellable, error))
    goto out;
  builder->loose_compressed_size += compressed_size;

  current_part->uncompressed_size += content_size;

  g_ptr_array_add (current_part->objects, ostree_object_name_serialize (checksum, objtype));

  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    {
      gsize object_payload_start;

      object_payload_start = current_part->payload->len;

      if (!splice_stream_to_payload (current_part, content_stream,
                                     cancellable, error))
        goto out;

      g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_OPEN_SPLICE_AND_CLOSE);
      _ostree_write_varuint64 (current_part->operations, content_size);
      _ostree_write_varuint64 (current_part->operations, object_payload_start);
    }
  else
    {
      gsize mode_offset, xattr_offset, content_offset;
      guint32 mode;

      mode = g_file_info_get_attribute_uint32 (content_finfo, "unix::mode");

      write_content_mode_xattrs (repo, current_part, content_finfo, content_xattrs,
                                 &mode_offset, &xattr_offset);

      if (S_ISLNK (mode))
        {
          const char *target;

          g_assert (content_stream == NULL);

          target = g_file_info_get_symlink_target (content_finfo);
          content_stream = 
            g_memory_input_stream_new_from_data (target, strlen (target), NULL);
          content_size = strlen (target);
        }
      else
        {
          g_assert (S_ISREG (mode));
        }

      content_offset = current_part->payload->len;
      if (!splice_stream_to_payload (current_part, content_stream,
                                     cancellable, error))
        goto out;

      g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_OPEN_SPLICE_AND_CLOSE);
      _ostree_write_varuint64 (current_part->operations, mode_offset);
      _ostree_write_varuint64 (current_part->operations, xattr_offset);
      _ostree_write_varuint64 (current_part->operations, content_size);
      _ostree_write_varuint64 (current_part->operations, content_offset);
    }

  ret = TRUE;
 out:
  return ret;
}

typedef struct {
  char *from_checksum;
  GBytes *tmp_from;
  GBytes *tmp_to;
} ContentBsdiff;

typedef struct {
  char *from_checksum;
  OstreeRollsumMatches *matches;
  GBytes *tmp_from;
  GBytes *tmp_to;
} ContentRollsum;

static void
content_rollsums_free (ContentRollsum  *rollsum)
{
  g_free (rollsum->from_checksum);
  _ostree_rollsum_matches_free (rollsum->matches);
  g_bytes_unref (rollsum->tmp_from);
  g_bytes_unref (rollsum->tmp_to);
  g_free (rollsum);
}

static void
content_bsdiffs_free (ContentBsdiff  *bsdiff)
{
  g_free (bsdiff->from_checksum);
  g_bytes_unref (bsdiff->tmp_from);
  g_bytes_unref (bsdiff->tmp_to);
  g_free (bsdiff);
}

/* Load a content object, uncompressing it to an unlinked tmpfile
   that's mmap()'d and suitable for seeking.
 */
static gboolean
get_unpacked_unlinked_content (OstreeRepo       *repo,
                               const char       *checksum,
                               GBytes          **out_content,
                               GFileInfo       **out_finfo,
                               GCancellable     *cancellable,
                               GError          **error)
{
  gboolean ret = FALSE;
  g_autofree char *tmpname = g_strdup ("/var/tmp/tmpostree-deltaobj-XXXXXX");
  glnx_fd_close int fd = -1;
  g_autoptr(GBytes) ret_content = NULL;
  g_autoptr(GInputStream) istream = NULL;
  g_autoptr(GFileInfo) ret_finfo = NULL;
  g_autoptr(GOutputStream) out = NULL;

  fd = g_mkstemp (tmpname);
  if (fd == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }
  /* Doesn't need a name */
  (void) unlink (tmpname);

  if (!ostree_repo_load_file (repo, checksum, &istream, &ret_finfo, NULL,
                              cancellable, error))
    goto out;

  g_assert (g_file_info_get_file_type (ret_finfo) == G_FILE_TYPE_REGULAR);
  
  out = g_unix_output_stream_new (fd, FALSE);
  if (g_output_stream_splice (out, istream, G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                              cancellable, error) < 0)
    goto out;
  
  { GMappedFile *mfile = g_mapped_file_new_from_fd (fd, FALSE, error);
    ret_content = g_mapped_file_get_bytes (mfile);
    g_mapped_file_unref (mfile);
  }

  ret = TRUE;
  if (out_content)
    *out_content = g_steal_pointer (&ret_content);
 out:
  return ret;
}

static gboolean
try_content_bsdiff (OstreeRepo                       *repo,
                    const char                       *from,
                    const char                       *to,
                    ContentBsdiff                    **out_bsdiff,
                    guint64                          max_bsdiff_size_bytes,
                    GCancellable                     *cancellable,
                    GError                           **error)
{
  gboolean ret = FALSE;
  g_autoptr(GHashTable) from_bsdiff = NULL;
  g_autoptr(GHashTable) to_bsdiff = NULL;
  g_autoptr(GBytes) tmp_from = NULL;
  g_autoptr(GBytes) tmp_to = NULL;
  g_autoptr(GFileInfo) from_finfo = NULL;
  g_autoptr(GFileInfo) to_finfo = NULL;
  ContentBsdiff *ret_bsdiff = NULL;

  *out_bsdiff = NULL;

  if (!get_unpacked_unlinked_content (repo, from, &tmp_from, &from_finfo,
                                      cancellable, error))
    goto out;
  if (!get_unpacked_unlinked_content (repo, to, &tmp_to, &to_finfo,
                                      cancellable, error))
    goto out;

  if (g_bytes_get_size (tmp_to) + g_bytes_get_size (tmp_from) > max_bsdiff_size_bytes)
    {
      ret = TRUE;
      goto out;
    }

  ret_bsdiff = g_new0 (ContentBsdiff, 1);
  ret_bsdiff->from_checksum = g_strdup (from);
  ret_bsdiff->tmp_from = tmp_from; tmp_from = NULL;
  ret_bsdiff->tmp_to = tmp_to; tmp_to = NULL;

  ret = TRUE;
  if (out_bsdiff)
    *out_bsdiff = g_steal_pointer (&ret_bsdiff);
 out:
  return ret;
}

static gboolean
try_content_rollsum (OstreeRepo                       *repo,
                     DeltaOpts                        opts,
                     const char                       *from,
                     const char                       *to,
                     ContentRollsum                  **out_rollsum,
                     GCancellable                     *cancellable,
                     GError                          **error)
{
  gboolean ret = FALSE;
  g_autoptr(GHashTable) from_rollsum = NULL;
  g_autoptr(GHashTable) to_rollsum = NULL;
  g_autoptr(GBytes) tmp_from = NULL;
  g_autoptr(GBytes) tmp_to = NULL;
  g_autoptr(GFileInfo) from_finfo = NULL;
  g_autoptr(GFileInfo) to_finfo = NULL;
  OstreeRollsumMatches *matches = NULL;
  ContentRollsum *ret_rollsum = NULL;

  *out_rollsum = NULL;

  /* Load the content objects, splice them to uncompressed temporary files that
   * we can just mmap() and seek around in conveniently.
   */
  if (!get_unpacked_unlinked_content (repo, from, &tmp_from, &from_finfo,
                                      cancellable, error))
    goto out;
  if (!get_unpacked_unlinked_content (repo, to, &tmp_to, &to_finfo,
                                      cancellable, error))
    goto out;

  matches = _ostree_compute_rollsum_matches (tmp_from, tmp_to);

  { guint match_ratio = (matches->bufmatches*100)/matches->total;

    /* Only proceed if the file contains (arbitrary) more than 50% of
     * the previous chunks.
     */
    if (match_ratio < 50)
      {
        ret = TRUE;
        goto out;
      }
  }

  if (opts & DELTAOPT_FLAG_VERBOSE)
    {
      g_printerr ("rollsum for %s; crcs=%u bufs=%u total=%u matchsize=%llu\n",
                  to, matches->crcmatches,
                  matches->bufmatches,
                  matches->total, (unsigned long long)matches->match_size);
    }

  ret_rollsum = g_new0 (ContentRollsum, 1);
  ret_rollsum->from_checksum = g_strdup (from);
  ret_rollsum->matches = matches; matches = NULL;
  ret_rollsum->tmp_from = tmp_from; tmp_from = NULL;
  ret_rollsum->tmp_to = tmp_to; tmp_to = NULL;
  
  ret = TRUE;
  if (out_rollsum)
    *out_rollsum = g_steal_pointer (&ret_rollsum);
 out:
  if (matches)
    _ostree_rollsum_matches_free (matches);
  return ret;
}

struct bzdiff_opaque_s
{
  GOutputStream *out;
  GCancellable *cancellable;
  GError **error;
};

static int
bzdiff_write (struct bsdiff_stream* stream, const void* buffer, int size)
{
  struct bzdiff_opaque_s *op = stream->opaque;
  if (!g_output_stream_write (op->out,
                              buffer,
                              size,
                              op->cancellable,
                              op->error))
    return -1;

  return 0;
}

static void
append_payload_chunk_and_write (OstreeStaticDeltaPartBuilder    *current_part,
                                const guint8                    *buf,
                                guint64                          offset)
{
  guint64 payload_start;

  payload_start = current_part->payload->len;
  g_string_append_len (current_part->payload, (char*)buf, offset);
  g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_WRITE);
  _ostree_write_varuint64 (current_part->operations, offset);
  _ostree_write_varuint64 (current_part->operations, payload_start);
}

static gboolean
process_one_rollsum (OstreeRepo                       *repo,
                     OstreeStaticDeltaBuilder         *builder,
                     OstreeStaticDeltaPartBuilder    **current_part_val,
                     const char                       *to_checksum,
                     ContentRollsum                   *rollsum,
                     GCancellable                     *cancellable,
                     GError                          **error)
{
  gboolean ret = FALSE;
  guint64 content_size;
  g_autoptr(GInputStream) content_stream = NULL;
  g_autoptr(GFileInfo) content_finfo = NULL;
  g_autoptr(GVariant) content_xattrs = NULL;
  OstreeStaticDeltaPartBuilder *current_part = *current_part_val;
  const guint8 *tmp_to_buf;
  gsize tmp_to_len;

  /* Check to see if this delta has gone over maximum size */
  if (current_part->objects->len > 0 &&
      current_part->payload->len > builder->max_chunk_size_bytes)
    {
      *current_part_val = current_part = allocate_part (builder);
    }

  tmp_to_buf = g_bytes_get_data (rollsum->tmp_to, &tmp_to_len);

  if (!ostree_repo_load_file (repo, to_checksum, &content_stream,
                              &content_finfo, &content_xattrs,
                              cancellable, error))
    goto out;
  content_size = g_file_info_get_size (content_finfo);
  g_assert_cmpint (tmp_to_len, ==, content_size);

  current_part->uncompressed_size += content_size;

  g_ptr_array_add (current_part->objects, ostree_object_name_serialize (to_checksum, OSTREE_OBJECT_TYPE_FILE));

  { gsize mode_offset, xattr_offset, from_csum_offset;
    gboolean reading_payload = TRUE;
    guchar source_csum[OSTREE_SHA256_DIGEST_LEN];
    guint i;

    write_content_mode_xattrs (repo, current_part, content_finfo, content_xattrs,
                               &mode_offset, &xattr_offset);

    /* Write the origin checksum */
    ostree_checksum_inplace_to_bytes (rollsum->from_checksum, source_csum);
    from_csum_offset = current_part->payload->len;
    g_string_append_len (current_part->payload, (char*)source_csum, sizeof (source_csum));

    g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_OPEN);
    _ostree_write_varuint64 (current_part->operations, mode_offset);
    _ostree_write_varuint64 (current_part->operations, xattr_offset);
    _ostree_write_varuint64 (current_part->operations, content_size);

    { guint64 writing_offset = 0;
      guint64 offset = 0, to_start = 0, from_start = 0;
      GPtrArray *matchlist = rollsum->matches->matches;

      g_assert (matchlist->len > 0);
      for (i = 0; i < matchlist->len; i++)
        {
          GVariant *match = matchlist->pdata[i];
          guint32 crc;
          guint64 prefix;

          g_variant_get (match, "(uttt)", &crc, &offset, &to_start, &from_start);

          prefix = to_start - writing_offset;

          if (prefix > 0)
            {
              if (!reading_payload)
                {
                  g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_UNSET_READ_SOURCE);
                  reading_payload = TRUE;
                }

              g_assert_cmpint (writing_offset + prefix, <=, tmp_to_len);
              append_payload_chunk_and_write (current_part, tmp_to_buf + writing_offset, prefix);
              writing_offset += prefix;
            }

          if (reading_payload)
            {
              g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_SET_READ_SOURCE);
              _ostree_write_varuint64 (current_part->operations, from_csum_offset);
              reading_payload = FALSE;
            }

          g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_WRITE);
          _ostree_write_varuint64 (current_part->operations, offset);
          _ostree_write_varuint64 (current_part->operations, from_start);
          writing_offset += offset;
        }

      if (!reading_payload)
        g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_UNSET_READ_SOURCE);

      { guint64 remainder = tmp_to_len - writing_offset;
        if (remainder > 0)
          append_payload_chunk_and_write (current_part, tmp_to_buf + writing_offset, remainder);
        writing_offset += remainder;
        g_assert_cmpint (writing_offset, ==, tmp_to_len);
      }

      g_assert_cmpint (writing_offset, ==, content_size);
    }


    g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_CLOSE);
  }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
process_one_bsdiff (OstreeRepo                       *repo,
                    OstreeStaticDeltaBuilder         *builder,
                    OstreeStaticDeltaPartBuilder    **current_part_val,
                    const char                       *to_checksum,
                    ContentBsdiff                   *bsdiff_content,
                    GCancellable                     *cancellable,
                    GError                          **error)
{
  gboolean ret = FALSE;
  guint64 content_size;
  g_autoptr(GInputStream) content_stream = NULL;
  g_autoptr(GFileInfo) content_finfo = NULL;
  g_autoptr(GVariant) content_xattrs = NULL;
  OstreeStaticDeltaPartBuilder *current_part = *current_part_val;
  const guint8 *tmp_to_buf;
  gsize tmp_to_len;
  const guint8 *tmp_from_buf;
  gsize tmp_from_len;

  /* Check to see if this delta has gone over maximum size */
  if (current_part->objects->len > 0 &&
      current_part->payload->len > builder->max_chunk_size_bytes)
    {
      *current_part_val = current_part = allocate_part (builder);
    }

  tmp_to_buf = g_bytes_get_data (bsdiff_content->tmp_to, &tmp_to_len);
  tmp_from_buf = g_bytes_get_data (bsdiff_content->tmp_from, &tmp_from_len);

  if (!ostree_repo_load_file (repo, to_checksum, &content_stream,
                              &content_finfo, &content_xattrs,
                              cancellable, error))
    goto out;
  content_size = g_file_info_get_size (content_finfo);
  g_assert_cmpint (tmp_to_len, ==, content_size);

  current_part->uncompressed_size += content_size;

  g_ptr_array_add (current_part->objects, ostree_object_name_serialize (to_checksum, OSTREE_OBJECT_TYPE_FILE));

  { gsize mode_offset, xattr_offset;
    guchar source_csum[OSTREE_SHA256_DIGEST_LEN];

    write_content_mode_xattrs (repo, current_part, content_finfo, content_xattrs,
                               &mode_offset, &xattr_offset);

    /* Write the origin checksum */
    ostree_checksum_inplace_to_bytes (bsdiff_content->from_checksum, source_csum);

    g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_SET_READ_SOURCE);
    _ostree_write_varuint64 (current_part->operations, current_part->payload->len);
    g_string_append_len (current_part->payload, (char*)source_csum, sizeof (source_csum));

    g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_OPEN);
    _ostree_write_varuint64 (current_part->operations, mode_offset);
    _ostree_write_varuint64 (current_part->operations, xattr_offset);
    _ostree_write_varuint64 (current_part->operations, content_size);

    {
      struct bsdiff_stream stream;
      struct bzdiff_opaque_s op;
      const gchar *payload;
      gssize payload_size;
      g_autoptr(GOutputStream) out = g_memory_output_stream_new_resizable ();
      stream.malloc = malloc;
      stream.free = free;
      stream.write = bzdiff_write;
      op.out = out;
      op.cancellable = cancellable;
      op.error = error;
      stream.opaque = &op;
      if (bsdiff (tmp_from_buf, tmp_from_len, tmp_to_buf, tmp_to_len, &stream) < 0) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "bsdiff generation failed");
        goto out;
      }

      payload = g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (out));
      payload_size = g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (out));

      g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_BSPATCH);
      _ostree_write_varuint64 (current_part->operations, current_part->payload->len);
      _ostree_write_varuint64 (current_part->operations, payload_size);

      g_string_append_len (current_part->payload, payload, payload_size);
    }
    g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_CLOSE);
  }

  g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_UNSET_READ_SOURCE);

  ret = TRUE;
 out:
  return ret;
}

static gboolean 
generate_delta_lowlatency (OstreeRepo                       *repo,
                           const char                       *from,
                           const char                       *to,
                           DeltaOpts                         opts,
                           OstreeStaticDeltaBuilder         *builder,
                           GCancellable                     *cancellable,
                           GError                          **error)
{
  gboolean ret = FALSE;
  GHashTableIter hashiter;
  gpointer key, value;
  OstreeStaticDeltaPartBuilder *current_part = NULL;
  g_autoptr(GFile) root_from = NULL;
  g_autoptr(GVariant) from_commit = NULL;
  g_autoptr(GFile) root_to = NULL;
  g_autoptr(GVariant) to_commit = NULL;
  g_autoptr(GHashTable) to_reachable_objects = NULL;
  g_autoptr(GHashTable) from_reachable_objects = NULL;
  g_autoptr(GHashTable) from_regfile_content = NULL;
  g_autoptr(GHashTable) new_reachable_metadata = NULL;
  g_autoptr(GHashTable) new_reachable_regfile_content = NULL;
  g_autoptr(GHashTable) new_reachable_symlink_content = NULL;
  g_autoptr(GHashTable) modified_regfile_content = NULL;
  g_autoptr(GHashTable) rollsum_optimized_content_objects = NULL;
  g_autoptr(GHashTable) bsdiff_optimized_content_objects = NULL;
  g_autoptr(GHashTable) content_object_to_size = NULL;

  if (from != NULL)
    {
      if (!ostree_repo_read_commit (repo, from, &root_from, NULL,
                                    cancellable, error))
        goto out;

      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, from,
                                     &from_commit, error))
        goto out;

      if (!ostree_repo_traverse_commit (repo, from, 0, &from_reachable_objects,
                                        cancellable, error))
        goto out;
    }

  if (!ostree_repo_read_commit (repo, to, &root_to, NULL,
                                cancellable, error))
    goto out;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, to,
                                 &to_commit, error))
    goto out;

  if (!ostree_repo_traverse_commit (repo, to, 0, &to_reachable_objects,
                                    cancellable, error))
    goto out;

  new_reachable_metadata = ostree_repo_traverse_new_reachable ();
  new_reachable_regfile_content = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
  new_reachable_symlink_content = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);

  g_hash_table_iter_init (&hashiter, to_reachable_objects);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;

      if (from_reachable_objects && g_hash_table_contains (from_reachable_objects, serialized_key))
        continue;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      g_variant_ref (serialized_key);
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        g_hash_table_add (new_reachable_metadata, serialized_key);
      else
        {
          g_autoptr(GFileInfo) finfo = NULL;
          GFileType ftype;

          if (!ostree_repo_load_file (repo, checksum, NULL, &finfo, NULL,
                                      cancellable, error))
            goto out;

          ftype = g_file_info_get_file_type (finfo);
          if (ftype == G_FILE_TYPE_REGULAR)
            g_hash_table_add (new_reachable_regfile_content, g_strdup (checksum));
          else if (ftype == G_FILE_TYPE_SYMBOLIC_LINK)
            g_hash_table_add (new_reachable_symlink_content, g_strdup (checksum));
          else
            g_assert_not_reached ();
        }
    }

  if (from_commit)
    {
      if (!_ostree_delta_compute_similar_objects (repo, from_commit, to_commit,
                                                  new_reachable_regfile_content,
                                                  CONTENT_SIZE_SIMILARITY_THRESHOLD_PERCENT,
                                                  &modified_regfile_content,
                                                  cancellable, error))
        goto out;
    }
  else
    modified_regfile_content = g_hash_table_new (g_str_hash, g_str_equal);

  if (opts & DELTAOPT_FLAG_VERBOSE)
    {
      g_printerr ("modified: %u\n", g_hash_table_size (modified_regfile_content));
      g_printerr ("new reachable: metadata=%u content regular=%u symlink=%u\n",
                  g_hash_table_size (new_reachable_metadata),
                  g_hash_table_size (new_reachable_regfile_content),
                  g_hash_table_size (new_reachable_symlink_content));
    }

  /* We already ship the to commit in the superblock, don't ship it twice */
  g_hash_table_remove (new_reachable_metadata,
                       ostree_object_name_serialize (to, OSTREE_OBJECT_TYPE_COMMIT));

  rollsum_optimized_content_objects = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                             g_free,
                                                             (GDestroyNotify) content_rollsums_free);

  bsdiff_optimized_content_objects = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                            g_free,
                                                            (GDestroyNotify) content_bsdiffs_free);

  g_hash_table_iter_init (&hashiter, modified_regfile_content);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      const char *to_checksum = key;
      const char *from_checksum = value;
      ContentRollsum *rollsum;
      ContentBsdiff *bsdiff;

      if (!try_content_rollsum (repo, opts, from_checksum, to_checksum,
                                &rollsum, cancellable, error))
        goto out;

      if (rollsum)
        {
          g_hash_table_insert (rollsum_optimized_content_objects, g_strdup (to_checksum), rollsum);
          builder->rollsum_size += rollsum->matches->match_size;
          continue;
        }

      if (!(opts & DELTAOPT_FLAG_DISABLE_BSDIFF))
        {
          if (!try_content_bsdiff (repo, from_checksum, to_checksum,
                                   &bsdiff, builder->max_bsdiff_size_bytes,
                                   cancellable, error))
            goto out;

          if (bsdiff)
            g_hash_table_insert (bsdiff_optimized_content_objects, g_strdup (to_checksum), bsdiff);
        }
    }

  if (opts & DELTAOPT_FLAG_VERBOSE)
    {
      g_printerr ("rollsum for %u/%u modified\n",
                  g_hash_table_size (rollsum_optimized_content_objects),
                  g_hash_table_size (modified_regfile_content));
    }

  current_part = allocate_part (builder);

  /* Pack the metadata first */
  g_hash_table_iter_init (&hashiter, new_reachable_metadata);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      if (!process_one_object (repo, builder, &current_part,
                               checksum, objtype,
                               cancellable, error))
        goto out;
    }

  /* Now do rollsummed objects */

  g_hash_table_iter_init (&hashiter, rollsum_optimized_content_objects);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      const char *checksum = key;
      ContentRollsum *rollsum = value;

      if (!process_one_rollsum (repo, builder, &current_part,
                                checksum, rollsum,
                                cancellable, error))
        goto out;

      builder->n_rollsum++;
    }

  /* Now do bsdiff'ed objects */

  g_hash_table_iter_init (&hashiter, bsdiff_optimized_content_objects);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      const char *checksum = key;
      ContentBsdiff *bsdiff = value;

      if (!process_one_bsdiff (repo, builder, &current_part,
                               checksum, bsdiff,
                               cancellable, error))
        goto out;

      builder->n_bsdiff++;
    }

  /* Scan for large objects, so we can fall back to plain HTTP-based
   * fetch.
   */
  g_hash_table_iter_init (&hashiter, new_reachable_regfile_content);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      const char *checksum = key;
      guint64 uncompressed_size;
      gboolean fallback = FALSE;

      /* Skip content objects we rollsum'd or bsdiff'ed */
      if (g_hash_table_contains (rollsum_optimized_content_objects, checksum) ||
          g_hash_table_contains (bsdiff_optimized_content_objects, checksum))
        continue;

      if (!ostree_repo_load_object_stream (repo, OSTREE_OBJECT_TYPE_FILE, checksum,
                                           NULL, &uncompressed_size,
                                           cancellable, error))
        goto out;
      if (builder->min_fallback_size_bytes > 0 &&
          uncompressed_size > builder->min_fallback_size_bytes)
        fallback = TRUE;
  
      if (fallback)
        {
          g_autofree char *size = g_format_size (uncompressed_size);

          if (opts & DELTAOPT_FLAG_VERBOSE)
            g_printerr ("fallback for %s (%s)\n", checksum, size);

          g_ptr_array_add (builder->fallback_objects, 
                           ostree_object_name_serialize (checksum, OSTREE_OBJECT_TYPE_FILE));
          g_hash_table_iter_remove (&hashiter);
          builder->n_fallback++;
        }
    }

  /* Now non-rollsummed or bsdiff'ed regular file content */
  g_hash_table_iter_init (&hashiter, new_reachable_regfile_content);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      const char *checksum = key;

      /* Skip content objects we rollsum'd */
      if (g_hash_table_contains (rollsum_optimized_content_objects, checksum) ||
          g_hash_table_contains (bsdiff_optimized_content_objects, checksum))
        continue;

      if (!process_one_object (repo, builder, &current_part,
                               checksum, OSTREE_OBJECT_TYPE_FILE,
                               cancellable, error))
        goto out;
    }

  /* Now symlinks */
  g_hash_table_iter_init (&hashiter, new_reachable_symlink_content);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      const char *checksum = key;

      if (!process_one_object (repo, builder, &current_part,
                               checksum, OSTREE_OBJECT_TYPE_FILE,
                               cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
get_fallback_headers (OstreeRepo               *self,
                      OstreeStaticDeltaBuilder *builder,
                      GVariant                **out_headers,
                      GCancellable             *cancellable,
                      GError                  **error)
{
  gboolean ret = FALSE;
  guint i;
  g_autoptr(GVariant) ret_headers = NULL;
  g_autoptr(GVariantBuilder) fallback_builder = NULL;

  fallback_builder = g_variant_builder_new (G_VARIANT_TYPE ("a" OSTREE_STATIC_DELTA_FALLBACK_FORMAT));

  for (i = 0; i < builder->fallback_objects->len; i++)
    {
      GVariant *serialized = builder->fallback_objects->pdata[i];
      const char *checksum;
      OstreeObjectType objtype;
      guint64 compressed_size;
      guint64 uncompressed_size;

      ostree_object_name_deserialize (serialized, &checksum, &objtype);

      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        {
          if (!ostree_repo_load_object_stream (self, objtype, checksum,
                                               NULL, &uncompressed_size,
                                               cancellable, error))
            goto out;
          compressed_size = uncompressed_size;
        }
      else
        {
          g_autoptr(GFileInfo) file_info = NULL;

          if (!ostree_repo_query_object_storage_size (self, OSTREE_OBJECT_TYPE_FILE,
                                                      checksum,
                                                      &compressed_size,
                                                      cancellable, error))
            goto out;

          if (!ostree_repo_load_file (self, checksum,
                                      NULL, &file_info, NULL,
                                      cancellable, error))
            goto out;

          uncompressed_size = g_file_info_get_size (file_info);
        }

      g_variant_builder_add_value (fallback_builder,
                                   g_variant_new ("(y@aytt)",
                                                  objtype,
                                                  ostree_checksum_to_bytes_v (checksum),
                                                  maybe_swap_endian_u64 (builder->swap_endian, compressed_size),
                                                  maybe_swap_endian_u64 (builder->swap_endian, uncompressed_size)));
    }

  ret_headers = g_variant_ref_sink (g_variant_builder_end (fallback_builder));

  ret = TRUE;
  if (out_headers)
    *out_headers = g_steal_pointer (&ret_headers);
 out:
  return ret;
}

/**
 * ostree_repo_static_delta_generate:
 * @self: Repo
 * @opt: High level optimization choice
 * @from: ASCII SHA256 checksum of origin, or %NULL
 * @to: ASCII SHA256 checksum of target
 * @metadata: (allow-none): Optional metadata
 * @params: (allow-none): Parameters, see below
 * @cancellable: Cancellable
 * @error: Error
 *
 * Generate a lookaside "static delta" from @from (%NULL means
 * from-empty) which can generate the objects in @to.  This delta is
 * an optimization over fetching individual objects, and can be
 * conveniently stored and applied offline.
 *
 * The @params argument should be an a{sv}.  The following attributes
 * are known:
 *   - min-fallback-size: u: Minimum uncompressed size in megabytes to use fallback, 0 to disable fallbacks
 *   - max-chunk-size: u: Maximum size in megabytes of a delta part
 *   - max-bsdiff-size: u: Maximum size in megabytes to consider bsdiff compression
 *   for input files
 *   - compression: y: Compression type: 0=none, x=lzma, g=gzip
 *   - bsdiff-enabled: b: Enable bsdiff compression.  Default TRUE.
 *   - inline-parts: b: Put part data in header, to get a single file delta.  Default FALSE.
 *   - verbose: b: Print diagnostic messages.  Default FALSE.
 *   - endianness: b: Deltas use host byte order by default; this option allows choosing (G_BIG_ENDIAN or G_LITTLE_ENDIAN)
 *   - filename: ay: Save delta superblock to this filename, and parts in the same directory.  Default saves to repository.
 */
gboolean
ostree_repo_static_delta_generate (OstreeRepo                   *self,
                                   OstreeStaticDeltaGenerateOpt  opt,
                                   const char                   *from,
                                   const char                   *to,
                                   GVariant                     *metadata,
                                   GVariant                     *params,
                                   GCancellable                 *cancellable,
                                   GError                      **error)
{
  gboolean ret = FALSE;
  OstreeStaticDeltaBuilder builder = { 0, };
  guint i;
  guint min_fallback_size;
  guint max_bsdiff_size;
  guint max_chunk_size;
  g_auto(GVariantBuilder) metadata_builder = {{0,}};
  DeltaOpts delta_opts = DELTAOPT_FLAG_NONE;
  guint64 total_compressed_size = 0;
  guint64 total_uncompressed_size = 0;
  g_autoptr(GVariantBuilder) part_headers = NULL;
  g_autoptr(GPtrArray) part_tempfiles = NULL;
  g_autoptr(GVariant) delta_descriptor = NULL;
  g_autoptr(GVariant) to_commit = NULL;
  const char *opt_filename;
  g_autofree char *descriptor_relpath = NULL;
  g_autoptr(GFile) descriptor_path = NULL;
  g_autoptr(GFile) descriptor_dir = NULL;
  g_autoptr(GVariant) tmp_metadata = NULL;
  g_autoptr(GVariant) fallback_headers = NULL;
  g_autoptr(GVariant) detached = NULL;
  gboolean inline_parts;
  guint endianness = G_BYTE_ORDER; 
  g_autoptr(GFile) tmp_dir = NULL;
  builder.parts = g_ptr_array_new_with_free_func ((GDestroyNotify)ostree_static_delta_part_builder_unref);
  builder.fallback_objects = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  if (!g_variant_lookup (params, "min-fallback-size", "u", &min_fallback_size))
    min_fallback_size = 4;
  builder.min_fallback_size_bytes = ((guint64)min_fallback_size) * 1000 * 1000;

  if (!g_variant_lookup (params, "max-bsdiff-size", "u", &max_bsdiff_size))
    max_bsdiff_size = 128;
  builder.max_bsdiff_size_bytes = ((guint64)max_bsdiff_size) * 1000 * 1000;
  if (!g_variant_lookup (params, "max-chunk-size", "u", &max_chunk_size))
    max_chunk_size = 32;
  builder.max_chunk_size_bytes = ((guint64)max_chunk_size) * 1000 * 1000;

  (void) g_variant_lookup (params, "endianness", "u", &endianness);
  g_return_val_if_fail (endianness == G_BIG_ENDIAN || endianness == G_LITTLE_ENDIAN, FALSE);

  builder.swap_endian = endianness != G_BYTE_ORDER;

  { gboolean use_bsdiff;
    if (!g_variant_lookup (params, "bsdiff-enabled", "b", &use_bsdiff))
      use_bsdiff = TRUE;
    if (!use_bsdiff)
      delta_opts |= DELTAOPT_FLAG_DISABLE_BSDIFF;
  }

  { gboolean verbose;
    if (!g_variant_lookup (params, "verbose", "b", &verbose))
      verbose = FALSE;
    if (verbose)
      delta_opts |= DELTAOPT_FLAG_VERBOSE;
  }

  if (!g_variant_lookup (params, "inline-parts", "b", &inline_parts))
    inline_parts = FALSE;

  if (!g_variant_lookup (params, "filename", "^&ay", &opt_filename))
    opt_filename = NULL;

  if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_COMMIT, to,
                                 &to_commit, error))
    goto out;

  /* Ignore optimization flags */
  if (!generate_delta_lowlatency (self, from, to, delta_opts, &builder,
                                  cancellable, error))
    goto out;

  /* NOTE: Add user-supplied metadata first.  This is used by at least
   * xdg-app as a way to provide MIME content sniffing, since the
   * metadata appears first in the file.
   */
  g_variant_builder_init (&metadata_builder, G_VARIANT_TYPE ("a{sv}"));
  if (metadata != NULL)
    {
      GVariantIter iter;
      GVariant *item;

      g_variant_iter_init (&iter, metadata);
      while ((item = g_variant_iter_next_value (&iter)))
        {
          g_variant_builder_add (&metadata_builder, "@{sv}", item);
          g_variant_unref (item);
        }
    }

  { guint8 endianness_char;
    
    switch (endianness)
      {
      case G_LITTLE_ENDIAN:
        endianness_char = 'l';
        break;
      case G_BIG_ENDIAN:
        endianness_char = 'B';
        break;
      default:
        g_assert_not_reached ();
      }
    g_variant_builder_add (&metadata_builder, "{sv}", "ostree.endianness", g_variant_new_byte (endianness_char));
  }

  if (opt_filename)
    {
      g_autoptr(GFile) f = g_file_new_for_path (opt_filename);
      tmp_dir = g_file_get_parent (f);
    }
  else
    {
      tmp_dir = g_object_ref (self->tmp_dir);
    }

  part_headers = g_variant_builder_new (G_VARIANT_TYPE ("a" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT));
  part_tempfiles = g_ptr_array_new_with_free_func (g_object_unref);
  for (i = 0; i < builder.parts->len; i++)
    {
      OstreeStaticDeltaPartBuilder *part_builder = builder.parts->pdata[i];
      GBytes *payload_b;
      GBytes *operations_b;
      g_autofree guchar *part_checksum = NULL;
      g_autoptr(GChecksum) checksum = NULL;
      g_autoptr(GBytes) objtype_checksum_array = NULL;
      g_autoptr(GBytes) checksum_bytes = NULL;
      g_autoptr(GFile) part_tempfile = NULL;
      g_autoptr(GOutputStream) part_temp_outstream = NULL;
      g_autoptr(GInputStream) part_in = NULL;
      g_autoptr(GInputStream) part_payload_in = NULL;
      g_autoptr(GMemoryOutputStream) part_payload_out = NULL;
      g_autoptr(GConverterOutputStream) part_payload_compressor = NULL;
      g_autoptr(GConverter) compressor = NULL;
      g_autoptr(GVariant) delta_part_content = NULL;
      g_autoptr(GVariant) delta_part = NULL;
      g_autoptr(GVariant) delta_part_header = NULL;
      g_auto(GVariantBuilder) mode_builder = {{0,}};
      g_auto(GVariantBuilder) xattr_builder = {{0,}};
      guint8 compression_type_char;

      g_variant_builder_init (&mode_builder, G_VARIANT_TYPE ("a(uuu)"));
      g_variant_builder_init (&xattr_builder, G_VARIANT_TYPE ("aa(ayay)"));
      { guint j;
        for (j = 0; j < part_builder->modes->len; j++)
          g_variant_builder_add_value (&mode_builder, part_builder->modes->pdata[j]);
        
        for (j = 0; j < part_builder->xattrs->len; j++)
          g_variant_builder_add_value (&xattr_builder, part_builder->xattrs->pdata[j]);
      }
        
      payload_b = g_string_free_to_bytes (part_builder->payload);
      part_builder->payload = NULL;
      
      operations_b = g_string_free_to_bytes (part_builder->operations);
      part_builder->operations = NULL;
      /* FIXME - avoid duplicating memory here */
      delta_part_content = g_variant_new ("(a(uuu)aa(ayay)@ay@ay)",
                                          &mode_builder, &xattr_builder,
                                          ot_gvariant_new_ay_bytes (payload_b),
                                          ot_gvariant_new_ay_bytes (operations_b));
      g_variant_ref_sink (delta_part_content);

      /* Hardcode xz for now */
      compressor = (GConverter*)_ostree_lzma_compressor_new (NULL);
      compression_type_char = 'x';
      part_payload_in = ot_variant_read (delta_part_content);
      part_payload_out = (GMemoryOutputStream*)g_memory_output_stream_new (NULL, 0, g_realloc, g_free);
      part_payload_compressor = (GConverterOutputStream*)g_converter_output_stream_new ((GOutputStream*)part_payload_out, compressor);

      {
        gssize n_bytes_written = g_output_stream_splice ((GOutputStream*)part_payload_compressor, part_payload_in,
                                                         G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET | G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                                         cancellable, error);
        if (n_bytes_written < 0)
          goto out;
      }

      /* FIXME - avoid duplicating memory here */
      delta_part = g_variant_new ("(y@ay)",
                                  compression_type_char,
                                  ot_gvariant_new_ay_bytes (g_memory_output_stream_steal_as_bytes (part_payload_out)));
      g_variant_ref_sink (delta_part);

      if (inline_parts)
        {
          g_autofree char *part_relpath = _ostree_get_relative_static_delta_part_path (from, to, i);
          g_variant_builder_add (&metadata_builder, "{sv}", part_relpath, delta_part);
        }
      else if (!gs_file_open_in_tmpdir (tmp_dir, 0644,
                                        &part_tempfile, &part_temp_outstream,
                                        cancellable, error))
        goto out;
      part_in = ot_variant_read (delta_part);
      if (!ot_gio_splice_get_checksum (part_temp_outstream, part_in,
                                       &part_checksum,
                                       cancellable, error))
        goto out;

      checksum_bytes = g_bytes_new (part_checksum, OSTREE_SHA256_DIGEST_LEN);
      objtype_checksum_array = objtype_checksum_array_new (part_builder->objects);
      delta_part_header = g_variant_new ("(u@aytt@ay)",
                                         maybe_swap_endian_u32 (builder.swap_endian, OSTREE_DELTAPART_VERSION),
                                         ot_gvariant_new_ay_bytes (checksum_bytes),
                                         maybe_swap_endian_u64 (builder.swap_endian, (guint64) g_variant_get_size (delta_part)),
                                         maybe_swap_endian_u64 (builder.swap_endian, part_builder->uncompressed_size),
                                         ot_gvariant_new_ay_bytes (objtype_checksum_array));

      g_variant_builder_add_value (part_headers, g_variant_ref (delta_part_header));
      if (part_tempfile)
        g_ptr_array_add (part_tempfiles, g_object_ref (part_tempfile));
      
      total_compressed_size += g_variant_get_size (delta_part);
      total_uncompressed_size += part_builder->uncompressed_size;

      if (delta_opts & DELTAOPT_FLAG_VERBOSE)
        {
          g_printerr ("part %u n:%u compressed:%" G_GUINT64_FORMAT " uncompressed:%" G_GUINT64_FORMAT "\n",
                      i, part_builder->objects->len,
                      (guint64)g_variant_get_size (delta_part),
                      part_builder->uncompressed_size);
        }
    }

  if (opt_filename)
    {
      descriptor_path = g_file_new_for_path (opt_filename);
    }
  else
    {
      descriptor_relpath = _ostree_get_relative_static_delta_superblock_path (from, to);
      descriptor_path = g_file_resolve_relative_path (self->repodir, descriptor_relpath);
    }

  descriptor_dir = g_file_get_parent (descriptor_path);

  if (!glnx_shutil_mkdir_p_at (AT_FDCWD, gs_file_get_path_cached (descriptor_dir), 0755,
                               cancellable, error))
    goto out;

  for (i = 0; i < part_tempfiles->len; i++)
    {
      GFile *tempfile = part_tempfiles->pdata[i];
      g_autofree char *partstr = g_strdup_printf ("%u", i);
      g_autoptr(GFile) part_path = g_file_resolve_relative_path (descriptor_dir, partstr);

      if (!gs_file_rename (tempfile, part_path, cancellable, error))
        goto out;
    }

  if (!get_fallback_headers (self, &builder, &fallback_headers,
                             cancellable, error))
    goto out;

  if (!ostree_repo_read_commit_detached_metadata (self, to, &detached, cancellable, error))
    goto out;

  if (detached)
    {
      g_autofree char *detached_key = _ostree_get_relative_static_delta_path (from, to, "commitmeta");
      g_variant_builder_add (&metadata_builder, "{sv}", detached_key, detached);
    }

  /* Generate OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT */
  {
    GDateTime *now = g_date_time_new_now_utc ();
    /* floating */ GVariant *from_csum_v =
      from ? ostree_checksum_to_bytes_v (from) : ot_gvariant_new_bytearray ((guchar *)"", 0);
    /* floating */ GVariant *to_csum_v =
      ostree_checksum_to_bytes_v (to);

    delta_descriptor = g_variant_new ("(@a{sv}t@ay@ay@" OSTREE_COMMIT_GVARIANT_STRING "ay"
                                      "a" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT
                                      "@a" OSTREE_STATIC_DELTA_FALLBACK_FORMAT ")",
                                      g_variant_builder_end (&metadata_builder),
                                      GUINT64_TO_BE (g_date_time_to_unix (now)),
                                      from_csum_v,
                                      to_csum_v,
                                      to_commit,
                                      g_variant_builder_new (G_VARIANT_TYPE ("ay")),
                                      part_headers,
                                      fallback_headers);
    g_date_time_unref (now);
  }

  if (delta_opts & DELTAOPT_FLAG_VERBOSE)
    {
      g_printerr ("uncompressed=%" G_GUINT64_FORMAT " compressed=%" G_GUINT64_FORMAT " loose=%" G_GUINT64_FORMAT "\n",
                  total_uncompressed_size,
                  total_compressed_size,
                  builder.loose_compressed_size);
      g_printerr ("rollsum=%u objects, %" G_GUINT64_FORMAT " bytes\n",
                  builder.n_rollsum,
                  builder.rollsum_size);
      g_printerr ("bsdiff=%u objects\n", builder.n_bsdiff);
    }

  if (!ot_util_variant_save (descriptor_path, delta_descriptor, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_pointer (&builder.parts, g_ptr_array_unref);
  g_clear_pointer (&builder.fallback_objects, g_ptr_array_unref);
  return ret;
}
