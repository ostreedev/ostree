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
#include <gio/gunixoutputstream.h>

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-lzma-compressor.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-diff.h"
#include "ostree-rollsum.h"
#include "otutil.h"
#include "ostree-varint.h"

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
  guint64 max_chunk_size_bytes;
  guint64 rollsum_size;
} OstreeStaticDeltaBuilder;

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
      guint8 csum[32];
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
  gs_unref_variant GVariant *modev
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
  gs_unref_object GInputStream *content_stream = NULL;
  gs_unref_object GFileInfo *content_finfo = NULL;
  gs_unref_variant GVariant *content_xattrs = NULL;
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
  OstreeRollsumMatches *matches;
  GBytes *tmp_to;
} ContentRollsum;

static void
content_rollsums_free (ContentRollsum  *rollsum)
{
  g_free (rollsum->from_checksum);
  _ostree_rollsum_matches_free (rollsum->matches);
  g_bytes_unref (rollsum->tmp_to);
  g_free (rollsum);
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
  gs_free char *tmpname = g_strdup ("tmpostree-deltaobj-XXXXXX");
  gs_fd_close int fd = -1;
  gs_unref_bytes GBytes *ret_content = NULL;
  gs_unref_object GInputStream *istream = NULL;
  gs_unref_object GFileInfo *ret_finfo = NULL;
  gs_unref_object GOutputStream *out = NULL;

  fd = g_mkstemp (tmpname);
  if (fd == -1)
    {
      gs_set_error_from_errno (error, errno);
      goto out;
    }
  /* Doesn't need a name */
  (void) unlink (tmpname);

  if (!ostree_repo_load_file (repo, checksum, &istream, &ret_finfo, NULL,
                              cancellable, error))
    goto out;

  if (g_file_info_get_file_type (ret_finfo) != G_FILE_TYPE_REGULAR)
    {
      ret = TRUE;
      goto out;
    }
  
  out = g_unix_output_stream_new (fd, FALSE);
  if (g_output_stream_splice (out, istream, G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                              cancellable, error) < 0)
    goto out;
  
  { GMappedFile *mfile = g_mapped_file_new_from_fd (fd, FALSE, error);
    ret_content = g_mapped_file_get_bytes (mfile);
    g_mapped_file_unref (mfile);
  }

  ret = TRUE;
  gs_transfer_out_value (out_content, &ret_content);
 out:
  return ret;
}

static gboolean
try_content_rollsum (OstreeRepo                       *repo,
                     const char                       *from,
                     const char                       *to,
                     ContentRollsum                  **out_rollsum,
                     GCancellable                     *cancellable,
                     GError                          **error)
{
  gboolean ret = FALSE;
  gs_unref_hashtable GHashTable *from_rollsum = NULL;
  gs_unref_hashtable GHashTable *to_rollsum = NULL;
  gs_unref_bytes GBytes *tmp_from = NULL;
  gs_unref_bytes GBytes *tmp_to = NULL;
  gs_unref_object GFileInfo *from_finfo = NULL;
  gs_unref_object GFileInfo *to_finfo = NULL;
  OstreeRollsumMatches *matches;
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

  /* Only try to rollsum regular files obviously */ 
  if (!(tmp_from && tmp_to))
    {
      ret = TRUE;
      goto out;
    }

  matches = _ostree_compute_rollsum_matches (tmp_from, tmp_to);

  { guint match_ratio = (matches->bufmatches*100)/matches->total;

    /* Only proceed if the file contains (arbitrary) more than 25% of
     * the previous chunks.
     */
    if (match_ratio < 25)
      {
        ret = TRUE;
        goto out;
      }
  }

  g_printerr ("rollsum for %s; crcs=%u bufs=%u total=%u matchsize=%llu\n",
              to, matches->crcmatches,
              matches->bufmatches,
              matches->total, (unsigned long long)matches->match_size);

  ret_rollsum = g_new0 (ContentRollsum, 1);
  ret_rollsum->from_checksum = g_strdup (from);
  ret_rollsum->matches = matches; matches = NULL;
  ret_rollsum->tmp_to = tmp_to; tmp_to = NULL;
  
  ret = TRUE;
  gs_transfer_out_value (out_rollsum, &ret_rollsum);
 out:
  if (matches)
    _ostree_rollsum_matches_free (matches);
  return ret;
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
  gs_unref_object GInputStream *content_stream = NULL;
  gs_unref_object GFileInfo *content_finfo = NULL;
  gs_unref_variant GVariant *content_xattrs = NULL;
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
    guchar source_csum[32];
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
        {
          g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_UNSET_READ_SOURCE);
          reading_payload = TRUE;
        }
      
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
generate_delta_lowlatency (OstreeRepo                       *repo,
                           const char                       *from,
                           const char                       *to,
                           OstreeStaticDeltaBuilder         *builder,
                           GCancellable                     *cancellable,
                           GError                          **error)
{
  gboolean ret = FALSE;
  GHashTableIter hashiter;
  gpointer key, value;
  guint i;
  OstreeStaticDeltaPartBuilder *current_part = NULL;
  gs_unref_object GFile *root_from = NULL;
  gs_unref_object GFile *root_to = NULL;
  gs_unref_ptrarray GPtrArray *modified = NULL;
  gs_unref_ptrarray GPtrArray *removed = NULL;
  gs_unref_ptrarray GPtrArray *added = NULL;
  gs_unref_hashtable GHashTable *to_reachable_objects = NULL;
  gs_unref_hashtable GHashTable *from_reachable_objects = NULL;
  gs_unref_hashtable GHashTable *new_reachable_metadata = NULL;
  gs_unref_hashtable GHashTable *new_reachable_content = NULL;
  gs_unref_hashtable GHashTable *modified_content_objects = NULL;
  gs_unref_hashtable GHashTable *rollsum_optimized_content_objects = NULL;
  gs_unref_hashtable GHashTable *content_object_to_size = NULL;

  if (from != NULL)
    {
      if (!ostree_repo_read_commit (repo, from, &root_from, NULL,
                                    cancellable, error))
        goto out;
    }
  if (!ostree_repo_read_commit (repo, to, &root_to, NULL,
                                cancellable, error))
    goto out;

  /* Gather a filesystem level diff; when we do heuristics to ship
   * just parts of changed files, we can make use of this data.
   */
  modified = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_diff_item_unref);
  removed = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  added = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  if (!ostree_diff_dirs (OSTREE_DIFF_FLAGS_NONE, root_from, root_to, modified, removed, added,
                         cancellable, error))
    goto out;

  modified_content_objects = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free, g_free);
  for (i = 0; i < modified->len; i++)
    {
      OstreeDiffItem *diffitem = modified->pdata[i];
      /* Theoretically, a target file could replace multiple source
       * files.  That could happen if say a project changed from having
       * multiple binaries to one binary.
       *
       * In that case, we have last one wins behavior.  For ELF rollsum
       * tends to be useless unless there's a large static data blob.
       */
      g_hash_table_replace (modified_content_objects,
                            g_strdup (diffitem->target_checksum),
                            g_strdup (diffitem->src_checksum));
    }

  if (from)
    {
      if (!ostree_repo_traverse_commit (repo, from, 0, &from_reachable_objects,
                                        cancellable, error))
        goto out;
    }

  if (!ostree_repo_traverse_commit (repo, to, 0, &to_reachable_objects,
                                    cancellable, error))
    goto out;

  new_reachable_metadata = ostree_repo_traverse_new_reachable ();
  new_reachable_content = ostree_repo_traverse_new_reachable ();

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
        g_hash_table_add (new_reachable_content, serialized_key);
    }
  
  g_printerr ("modified: %u removed: %u added: %u\n",
              modified->len, removed->len, added->len);
  g_printerr ("new reachable: metadata=%u content=%u\n",
              g_hash_table_size (new_reachable_metadata),
              g_hash_table_size (new_reachable_content));

  /* We already ship the to commit in the superblock, don't ship it twice */
  g_hash_table_remove (new_reachable_metadata,
                       ostree_object_name_serialize (to, OSTREE_OBJECT_TYPE_COMMIT));

  rollsum_optimized_content_objects = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                             g_free,
                                                             (GDestroyNotify) content_rollsums_free);

  g_hash_table_iter_init (&hashiter, modified_content_objects);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      const char *to_checksum = key;
      const char *from_checksum = value;
      ContentRollsum *rollsum;

      if (!try_content_rollsum (repo, from_checksum, to_checksum,
                                &rollsum, cancellable, error))
        goto out;

      if (!rollsum)
        continue;

      g_hash_table_insert (rollsum_optimized_content_objects, g_strdup (to_checksum), rollsum);
      builder->rollsum_size += rollsum->matches->match_size;
    }

  g_printerr ("rollsum for %u/%u modified\n",
              g_hash_table_size (rollsum_optimized_content_objects),
              g_hash_table_size (modified_content_objects));

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
    }

  /* Scan for large objects, so we can fall back to plain HTTP-based
   * fetch.
   */
  g_hash_table_iter_init (&hashiter, new_reachable_content);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;
      guint64 uncompressed_size;
      gboolean fallback = FALSE;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      /* Skip content objects we rollsum'd */
      if (g_hash_table_contains (rollsum_optimized_content_objects, checksum))
        continue;

      if (!ostree_repo_load_object_stream (repo, objtype, checksum,
                                           NULL, &uncompressed_size,
                                           cancellable, error))
        goto out;
      if (uncompressed_size > builder->min_fallback_size_bytes)
        fallback = TRUE;
  
      if (fallback)
        {
          gs_free char *size = g_format_size (uncompressed_size);
          g_printerr ("fallback for %s (%s)\n",
                      ostree_object_to_string (checksum, objtype), size);
          g_ptr_array_add (builder->fallback_objects, 
                           g_variant_ref (serialized_key));
          g_hash_table_iter_remove (&hashiter);
        }
    }

  /* Now non-rollsummed content */
  g_hash_table_iter_init (&hashiter, new_reachable_content);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      /* Skip content objects we rollsum'd */
      if (g_hash_table_contains (rollsum_optimized_content_objects, checksum))
        continue;

      if (!process_one_object (repo, builder, &current_part,
                               checksum, objtype,
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
  gs_unref_variant GVariant *ret_headers = NULL;
  gs_unref_variant_builder GVariantBuilder *fallback_builder = NULL;

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
          gs_unref_object GFileInfo *file_info = NULL;

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
                                                  compressed_size, uncompressed_size));
    }

  ret_headers = g_variant_ref_sink (g_variant_builder_end (fallback_builder));

  ret = TRUE;
  gs_transfer_out_value (out_headers, &ret_headers);
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
 *   - min-fallback-size: u: Minimume uncompressed size in megabytes to use fallback
 *   - max-chunk-size: u: Maximum size in megabytes of a delta part
 *   - compression: y: Compression type: 0=none, x=lzma, g=gzip
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
  guint max_chunk_size;
  GVariant *metadata_source;
  guint64 total_compressed_size = 0;
  guint64 total_uncompressed_size = 0;
  gs_unref_variant_builder GVariantBuilder *part_headers = NULL;
  gs_unref_ptrarray GPtrArray *part_tempfiles = NULL;
  gs_unref_variant GVariant *delta_descriptor = NULL;
  gs_unref_variant GVariant *to_commit = NULL;
  gs_free char *descriptor_relpath = NULL;
  gs_unref_object GFile *descriptor_path = NULL;
  gs_unref_object GFile *descriptor_dir = NULL;
  gs_unref_variant GVariant *tmp_metadata = NULL;
  gs_unref_variant GVariant *fallback_headers = NULL;

  builder.parts = g_ptr_array_new_with_free_func ((GDestroyNotify)ostree_static_delta_part_builder_unref);
  builder.fallback_objects = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  if (!g_variant_lookup (params, "min-fallback-size", "u", &min_fallback_size))
    min_fallback_size = 4;
  builder.min_fallback_size_bytes = ((guint64)min_fallback_size) * 1000 * 1000;

  if (!g_variant_lookup (params, "max-chunk-size", "u", &max_chunk_size))
    max_chunk_size = 32;
  builder.max_chunk_size_bytes = ((guint64)max_chunk_size) * 1000 * 1000;

  if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_COMMIT, to,
                                 &to_commit, error))
    goto out;

  /* Ignore optimization flags */
  if (!generate_delta_lowlatency (self, from, to, &builder,
                                  cancellable, error))
    goto out;

  part_headers = g_variant_builder_new (G_VARIANT_TYPE ("a" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT));
  part_tempfiles = g_ptr_array_new_with_free_func (g_object_unref);
  for (i = 0; i < builder.parts->len; i++)
    {
      OstreeStaticDeltaPartBuilder *part_builder = builder.parts->pdata[i];
      GBytes *payload_b;
      GBytes *operations_b;
      gs_free guchar *part_checksum = NULL;
      gs_free_checksum GChecksum *checksum = NULL;
      gs_unref_bytes GBytes *objtype_checksum_array = NULL;
      gs_unref_bytes GBytes *checksum_bytes = NULL;
      gs_unref_object GFile *part_tempfile = NULL;
      gs_unref_object GOutputStream *part_temp_outstream = NULL;
      gs_unref_object GInputStream *part_in = NULL;
      gs_unref_object GInputStream *part_payload_in = NULL;
      gs_unref_object GMemoryOutputStream *part_payload_out = NULL;
      gs_unref_object GConverterOutputStream *part_payload_compressor = NULL;
      gs_unref_object GConverter *compressor = NULL;
      gs_unref_variant GVariant *delta_part_content = NULL;
      gs_unref_variant GVariant *delta_part = NULL;
      gs_unref_variant GVariant *delta_part_header = NULL;
      GVariantBuilder *mode_builder = g_variant_builder_new (G_VARIANT_TYPE ("a(uuu)"));
      GVariantBuilder *xattr_builder = g_variant_builder_new (G_VARIANT_TYPE ("aa(ayay)"));
      guint8 compression_type_char;

      { guint j;
        for (j = 0; j < part_builder->modes->len; j++)
          g_variant_builder_add_value (mode_builder, part_builder->modes->pdata[j]);
        
        for (j = 0; j < part_builder->xattrs->len; j++)
          g_variant_builder_add_value (xattr_builder, part_builder->xattrs->pdata[j]);
      }
        
      payload_b = g_string_free_to_bytes (part_builder->payload);
      part_builder->payload = NULL;
      
      operations_b = g_string_free_to_bytes (part_builder->operations);
      part_builder->operations = NULL;
      /* FIXME - avoid duplicating memory here */
      delta_part_content = g_variant_new ("(a(uuu)aa(ayay)@ay@ay)",
                                          mode_builder, xattr_builder,
                                          ot_gvariant_new_ay_bytes (payload_b),
                                          ot_gvariant_new_ay_bytes (operations_b));
      g_variant_ref_sink (delta_part_content);

      /* Hardcode xz for now */
      compressor = (GConverter*)_ostree_lzma_compressor_new (NULL);
      compression_type_char = 'x';
      part_payload_in = ot_variant_read (delta_part_content);
      part_payload_out = (GMemoryOutputStream*)g_memory_output_stream_new (NULL, 0, g_realloc, g_free);
      part_payload_compressor = (GConverterOutputStream*)g_converter_output_stream_new ((GOutputStream*)part_payload_out, compressor);

      if (0 > g_output_stream_splice ((GOutputStream*)part_payload_compressor, part_payload_in,
                                      G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET | G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                      cancellable, error))
        goto out;

      /* FIXME - avoid duplicating memory here */
      delta_part = g_variant_new ("(y@ay)",
                                  compression_type_char,
                                  ot_gvariant_new_ay_bytes (g_memory_output_stream_steal_as_bytes (part_payload_out)));

      if (!gs_file_open_in_tmpdir (self->tmp_dir, 0644,
                                   &part_tempfile, &part_temp_outstream,
                                   cancellable, error))
        goto out;
      part_in = ot_variant_read (delta_part);
      if (!ot_gio_splice_get_checksum (part_temp_outstream, part_in,
                                       &part_checksum,
                                       cancellable, error))
        goto out;

      checksum_bytes = g_bytes_new (part_checksum, 32);
      objtype_checksum_array = objtype_checksum_array_new (part_builder->objects);
      delta_part_header = g_variant_new ("(u@aytt@ay)",
                                         OSTREE_DELTAPART_VERSION,
                                         ot_gvariant_new_ay_bytes (checksum_bytes),
                                         g_variant_get_size (delta_part),
                                         part_builder->uncompressed_size,
                                         ot_gvariant_new_ay_bytes (objtype_checksum_array));
      g_variant_builder_add_value (part_headers, g_variant_ref (delta_part_header));
      g_ptr_array_add (part_tempfiles, g_object_ref (part_tempfile));
      
      total_compressed_size += g_variant_get_size (delta_part);
      total_uncompressed_size += part_builder->uncompressed_size;

      g_printerr ("part %u n:%u compressed:%" G_GUINT64_FORMAT " uncompressed:%" G_GUINT64_FORMAT "\n",
                  i, part_builder->objects->len,
                  (guint64)g_variant_get_size (delta_part),
                  part_builder->uncompressed_size);
    }

  descriptor_relpath = _ostree_get_relative_static_delta_path (from, to);
  descriptor_path = g_file_resolve_relative_path (self->repodir, descriptor_relpath);
  descriptor_dir = g_file_get_parent (descriptor_path);

  if (!gs_file_ensure_directory (descriptor_dir, TRUE, cancellable, error))
    goto out;

  for (i = 0; i < builder.parts->len; i++)
    {
      GFile *tempfile = part_tempfiles->pdata[i];
      gs_free char *part_relpath = _ostree_get_relative_static_delta_part_path (from, to, i);
      gs_unref_object GFile *part_path = g_file_resolve_relative_path (self->repodir, part_relpath);

      if (!gs_file_rename (tempfile, part_path, cancellable, error))
        goto out;
    }

  if (metadata != NULL)
    metadata_source = metadata;
  else
    {
      metadata_source = ot_gvariant_new_empty_string_dict ();
    }

  if (!get_fallback_headers (self, &builder, &fallback_headers,
                             cancellable, error))
    goto out;

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
                                      metadata_source,
                                      GUINT64_TO_BE (g_date_time_to_unix (now)),
                                      from_csum_v,
                                      to_csum_v,
                                      to_commit,
                                      g_variant_builder_new (G_VARIANT_TYPE ("ay")),
                                      part_headers,
                                      fallback_headers);
    g_date_time_unref (now);
  }

  g_printerr ("uncompressed=%" G_GUINT64_FORMAT " compressed=%" G_GUINT64_FORMAT " loose=%" G_GUINT64_FORMAT "\n",
              total_uncompressed_size,
              total_compressed_size,
              builder.loose_compressed_size);
  g_printerr ("rollsum=%" G_GUINT64_FORMAT "\n", builder.rollsum_size);

  if (!ot_util_variant_save (descriptor_path, delta_descriptor, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_pointer (&builder.parts, g_ptr_array_unref);
  g_clear_pointer (&builder.fallback_objects, g_ptr_array_unref);
  return ret;
}
