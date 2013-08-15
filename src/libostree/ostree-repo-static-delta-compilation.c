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

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-diff.h"
#include "otutil.h"
#include "ostree-varint.h"

typedef struct {
  guint64 uncompressed_size;
  GPtrArray *objects;
  GString *payload;
  GString *operations;
} OstreeStaticDeltaPartBuilder;

typedef struct {
  GPtrArray *parts;
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
  g_free (part_builder);
}

static OstreeStaticDeltaPartBuilder *
allocate_part (OstreeStaticDeltaBuilder *builder)
{
  OstreeStaticDeltaPartBuilder *part = g_new0 (OstreeStaticDeltaPartBuilder, 1);
  part->objects = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);
  part->payload = g_string_new (NULL);
  part->operations = g_string_new (NULL);
  part->uncompressed_size = 0;
  g_ptr_array_add (builder->parts, part);
  return part;
}

static GBytes *
objtype_checksum_array_new (GPtrArray *objects)
{
  guint i;
  GByteArray *ret = g_byte_array_new ();

  g_assert (objects->len > 0);
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
  OstreeStaticDeltaPartBuilder *current_part = NULL;
  gs_unref_object GFile *root_from = NULL;
  gs_unref_object GFile *root_to = NULL;
  gs_unref_ptrarray GPtrArray *modified = NULL;
  gs_unref_ptrarray GPtrArray *removed = NULL;
  gs_unref_ptrarray GPtrArray *added = NULL;
  gs_unref_hashtable GHashTable *to_reachable_objects = NULL;
  gs_unref_hashtable GHashTable *from_reachable_objects = NULL;
  gs_unref_hashtable GHashTable *new_reachable_objects = NULL;

  if (!ostree_repo_read_commit (repo, from, &root_from, NULL,
                                cancellable, error))
    goto out;
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

  if (!ostree_repo_traverse_commit (repo, from, -1, &from_reachable_objects,
                                    cancellable, error))
    goto out;

  if (!ostree_repo_traverse_commit (repo, to, -1, &to_reachable_objects,
                                    cancellable, error))
    goto out;

  new_reachable_objects = ostree_repo_traverse_new_reachable ();

  g_hash_table_iter_init (&hashiter, to_reachable_objects);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      GVariant *serialized_key = key;

      if (g_hash_table_contains (from_reachable_objects, serialized_key))
        continue;

      g_hash_table_insert (new_reachable_objects, g_variant_ref (serialized_key), serialized_key);
    }

  current_part = allocate_part (builder);

  g_hash_table_iter_init (&hashiter, new_reachable_objects);
  while (g_hash_table_iter_next (&hashiter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;
      guint64 content_size;
      gsize object_payload_start;
      gs_unref_object GInputStream *content_stream = NULL;
      gsize bytes_read;
      const guint readlen = 4096;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      if (!ostree_repo_load_object_stream (repo, objtype, checksum,
                                           &content_stream, &content_size,
                                           cancellable, error))
        goto out;

      current_part->uncompressed_size += content_size;

      /* Ensure we have at least one object per delta, even if a given
       * object is larger.
       */
      if (current_part->objects->len > 0 &&
          current_part->payload->len + content_size > OSTREE_STATIC_DELTA_PART_MAX_SIZE_BYTES)
        {
          current_part = allocate_part (builder);
        } 

      g_ptr_array_add (current_part->objects, g_variant_ref (serialized_key));

      object_payload_start = current_part->payload->len;

      while (TRUE)
        {
          gsize empty_space;

          empty_space = current_part->payload->allocated_len - current_part->payload->len;
          if (empty_space < readlen)
            {
              gsize origlen;
              origlen = current_part->payload->len;
              g_string_set_size (current_part->payload, current_part->payload->allocated_len + (readlen - empty_space));
              current_part->payload->len = origlen;
            }

          if (!g_input_stream_read_all (content_stream,
                                        current_part->payload->str + current_part->payload->len,
                                        readlen,
                                        &bytes_read,
                                        cancellable, error))
            goto out;
          if (bytes_read == 0)
            break;
          
          current_part->payload->len += bytes_read;
        }
      
      g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_WRITE);
      _ostree_write_varuint64 (current_part->operations, object_payload_start);
      _ostree_write_varuint64 (current_part->operations, content_size);
      g_printerr ("write %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT "\n", (guint64) object_payload_start, (guint64)(content_size));
      g_string_append_c (current_part->operations, (gchar)OSTREE_STATIC_DELTA_OP_CLOSE);
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_repo_static_delta_generate:
 * @self: Repo
 * @opt: High level optimization choice
 * @from: ASCII SHA256 checksum of origin
 * @to: ASCII SHA256 checksum of target
 * @metadata: (allow-none): Optional metadata
 * @cancellable: Cancellable
 * @error: Error
 *
 * Generate a lookaside "static delta" from @from which can generate
 * the objects in @to.  This delta is an optimization over fetching
 * individual objects, and can be conveniently stored and applied
 * offline.
 */
gboolean
ostree_repo_static_delta_generate (OstreeRepo                   *self,
                                   OstreeStaticDeltaGenerateOpt  opt,
                                   const char                   *from,
                                   const char                   *to,
                                   GVariant                     *metadata,
                                   GCancellable                 *cancellable,
                                   GError                      **error)
{
  gboolean ret = FALSE;
  OstreeStaticDeltaBuilder builder = { 0, };
  guint i;
  GVariant *metadata_source;
  gs_unref_variant_builder GVariantBuilder *part_headers = NULL;
  gs_unref_ptrarray GPtrArray *part_tempfiles = NULL;
  gs_unref_variant GVariant *delta_descriptor = NULL;
  gs_free char *descriptor_relpath = NULL;
  gs_unref_object GFile *descriptor_path = NULL;
  gs_unref_object GFile *descriptor_dir = NULL;
  gs_unref_variant GVariant *tmp_metadata = NULL;

  builder.parts = g_ptr_array_new_with_free_func ((GDestroyNotify)ostree_static_delta_part_builder_unref);

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
      gs_unref_object GConverter *zlib_compressor = NULL;
      gs_unref_variant GVariant *delta_part_content = NULL;
      gs_unref_variant GVariant *delta_part = NULL;
      gs_unref_variant GVariant *delta_part_header = NULL;

      payload_b = g_string_free_to_bytes (part_builder->payload);
      part_builder->payload = NULL;
      
      operations_b = g_string_free_to_bytes (part_builder->operations);
      part_builder->operations = NULL;
      /* FIXME - avoid duplicating memory here */
      delta_part_content = g_variant_new ("(@ay@ay)",
                                          ot_gvariant_new_ay_bytes (payload_b),
                                          ot_gvariant_new_ay_bytes (operations_b));
      g_variant_ref_sink (delta_part_content);

      /* Hardcode gzip for now */
      zlib_compressor = (GConverter*)g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW, 9);
      part_payload_in = ot_variant_read (delta_part_content);
      part_payload_out = (GMemoryOutputStream*)g_memory_output_stream_new (NULL, 0, g_realloc, g_free);
      part_payload_compressor = (GConverterOutputStream*)g_converter_output_stream_new ((GOutputStream*)part_payload_out, zlib_compressor);

      if (0 > g_output_stream_splice ((GOutputStream*)part_payload_compressor, part_payload_in,
                                      G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET | G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                      cancellable, error))
        goto out;

      /* FIXME - avoid duplicating memory here */
      delta_part = g_variant_new ("(y@ay)",
                                  (guint8)'g',
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
      delta_part_header = g_variant_new ("(@aytt@ay)",
                                         ot_gvariant_new_ay_bytes (checksum_bytes),
                                         g_variant_get_size (delta_part),
                                         part_builder->uncompressed_size,
                                         ot_gvariant_new_ay_bytes (objtype_checksum_array));
      g_variant_builder_add_value (part_headers, g_variant_ref (delta_part_header));
      g_ptr_array_add (part_tempfiles, g_object_ref (part_tempfile));
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
      GVariantBuilder tmpbuilder;
      g_variant_builder_init (&tmpbuilder, G_VARIANT_TYPE ("(a(ss)a(say))"));
      g_variant_builder_add (&tmpbuilder, "a(ss)", NULL);
      g_variant_builder_add (&tmpbuilder, "a(say)", NULL);
      tmp_metadata = g_variant_builder_end (&tmpbuilder);
      g_variant_ref_sink (tmp_metadata);
      metadata_source = tmp_metadata;
    }

  delta_descriptor = g_variant_new ("(@(a(ss)a(say))aya(ayttay))",
                                    metadata_source,
                                    g_variant_builder_new (G_VARIANT_TYPE ("ay")),
                                    part_headers);

  if (!ot_util_variant_save (descriptor_path, delta_descriptor, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  g_clear_pointer (&builder.parts, g_ptr_array_unref);
  return ret;
}
