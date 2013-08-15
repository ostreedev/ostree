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

#include "ostree-repo-private.h"
#include "ostree-repo-static-delta-private.h"
#include "otutil.h"
#include "ostree-varint.h"

/* This should really always be true, but hey, let's just assert it */
G_STATIC_ASSERT (sizeof (guint) >= sizeof (guint32));

typedef struct {
  guint           checksum_index;
  const guint8   *checksums;
  guint           n_checksums;

  const guint8   *opdata;
  guint           oplen;

  OstreeObjectType output_objtype;
  const guint8   *output_target;
  GFile          *output_tmp_path;
  GOutputStream  *output_tmp_stream;
  const guint8   *input_target_csum;

  const guint8   *payload_data;
  guint64         payload_size; 
} StaticDeltaExecutionState;

typedef gboolean (*DispatchOpFunc) (OstreeRepo                 *repo,
                                    StaticDeltaExecutionState  *state,
                                    GCancellable               *cancellable,
                                    GError                    **error);

typedef struct  {
  const char *name;
  DispatchOpFunc func;
} OstreeStaticDeltaOperation;

#define OPPROTO(name) \
  static gboolean dispatch_##name (OstreeRepo                 *repo, \
                                   StaticDeltaExecutionState  *state, \
                                   GCancellable               *cancellable, \
                                   GError                    **error);

OPPROTO(fetch)
OPPROTO(write)
OPPROTO(gunzip)
OPPROTO(close)
#undef OPPROTO

static OstreeStaticDeltaOperation op_dispatch_table[] = {
  { "fetch", dispatch_fetch },
  { "write", dispatch_write },
  { "gunzip", dispatch_gunzip },
  { "close", dispatch_close },
  { NULL }
};

static gboolean
open_output_target_csum (OstreeRepo                  *repo,
                         StaticDeltaExecutionState   *state,
                         GCancellable                *cancellable,
                         GError                     **error)
{
  gboolean ret = FALSE;
  guint8 *objcsum;

  g_assert (state->checksums != NULL);
  g_assert (state->output_target == NULL);
  g_assert (state->output_tmp_path == NULL);
  g_assert (state->output_tmp_stream == NULL);
  g_assert (state->checksum_index < state->n_checksums);

  objcsum = (guint8*)state->checksums + (state->checksum_index * OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN);

  if (G_UNLIKELY(!ostree_validate_structureof_objtype (*objcsum, error)))
    goto out;

  state->output_objtype = (OstreeObjectType) *objcsum;
  state->output_target = objcsum + 1;
  if (!gs_file_open_in_tmpdir (repo->tmp_dir, 0644,
                               &state->output_tmp_path, &state->output_tmp_stream,
                               cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}


gboolean
_ostree_static_delta_part_execute (OstreeRepo      *repo,
                                   GVariant        *objects,
                                   GVariant        *part,
                                   GCancellable    *cancellable,
                                   GError         **error)
{
  gboolean ret = FALSE;
  guint8 *checksums_data;
  gs_unref_variant GVariant *checksums = NULL;
  gs_unref_variant GVariant *payload = NULL;
  gs_unref_variant GVariant *ops = NULL;
  StaticDeltaExecutionState statedata = { 0, };
  StaticDeltaExecutionState *state = &statedata;
  guint n_executed = 0;

  if (!_ostree_static_delta_parse_checksum_array (objects,
                                                  &checksums_data,
                                                  &state->n_checksums,
                                                  error))
    goto out;

  state->checksums = checksums_data;
  g_assert (state->n_checksums > 0);
  if (!open_output_target_csum (repo, state, cancellable, error))
    goto out;

  g_variant_get (part, "(@ay@ay)", &payload, &ops);

  state->payload_data = g_variant_get_data (payload);
  state->payload_size = g_variant_get_size (payload);

  state->oplen = g_variant_n_children (ops);
  state->opdata = g_variant_get_data (ops);
  while (state->oplen > 0)
    {
      guint8 opcode = state->opdata[0];
      OstreeStaticDeltaOperation *op;

      if (G_UNLIKELY (opcode == 0 || opcode >= G_N_ELEMENTS (op_dispatch_table)))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "Out of range opcode %u at offset %u", opcode, n_executed);
          goto out;
        }
      op = &op_dispatch_table[opcode-1];
      g_printerr ("dispatch %u\n", opcode-1);
      state->oplen--;
      state->opdata++;
      if (!op->func (repo, state, cancellable, error))
        goto out;

      n_executed++;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
dispatch_fetch (OstreeRepo    *repo,   
                StaticDeltaExecutionState  *state,
                GCancellable  *cancellable,  
                GError       **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "Static delta fetch opcode is not implemented in this version");
  return FALSE;
}

static gboolean
read_varuint64 (StaticDeltaExecutionState  *state,
                guint64                    *out_value,
                GError                    **error)
{
  gsize bytes_read;
  if (!_ostree_read_varuint64 (state->opdata, state->oplen, out_value, &bytes_read))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unexpected EOF reading varint");
      return FALSE;
    }
  state->opdata += bytes_read;
  state->oplen -= bytes_read;
  return TRUE;
}

static gboolean
validate_ofs (StaticDeltaExecutionState  *state,
              guint64                     offset,
              guint64                     length,
              GError                    **error)
{
  if (G_UNLIKELY (offset + length < offset ||
                  offset + length > state->payload_size))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Invalid offset/length %" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT,
                   offset, length);
      return FALSE;
    }
  return TRUE;
}
  
static gboolean
dispatch_write (OstreeRepo                 *repo,
                StaticDeltaExecutionState  *state,
                GCancellable               *cancellable,  
                GError                    **error)
{
  gboolean ret = FALSE;
  guint64 offset;
  guint64 length;
  gsize bytes_written;

  if (G_UNLIKELY(state->oplen < 2))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Expected at least 2 bytes for write op");
      goto out;
    }
  if (!read_varuint64 (state, &offset, error))
    goto out;
  if (!read_varuint64 (state, &length, error))
    goto out;
  
  if (!validate_ofs (state, offset, length, error))
    goto out;

  if (!g_output_stream_write_all (state->output_tmp_stream,
                                  state->payload_data + offset,
                                  length,
                                  &bytes_written,
                                  cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (!ret)
    g_prefix_error (error, "opcode write: ");
  return ret;
}

static gboolean
dispatch_gunzip (OstreeRepo                 *repo,
                 StaticDeltaExecutionState  *state,
                 GCancellable               *cancellable,  
                 GError                    **error)
{
  gboolean ret = FALSE;
  guint64 offset;
  guint64 length;
  gs_unref_object GConverter *zlib_decomp = NULL;
  gs_unref_object GInputStream *payload_in = NULL;
  gs_unref_object GInputStream *zlib_in = NULL;

  if (G_UNLIKELY(state->oplen < 2))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Expected at least 2 bytes for gunzip op");
      goto out;
    }
  if (!read_varuint64 (state, &offset, error))
    goto out;
  if (!read_varuint64 (state, &length, error))
    goto out;

  if (!validate_ofs (state, offset, length, error))
    goto out;

  payload_in = g_memory_input_stream_new_from_data (state->payload_data + offset, length, NULL);
  zlib_decomp = (GConverter*)g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW);
  zlib_in = g_converter_input_stream_new (payload_in, zlib_decomp);

  if (0 > g_output_stream_splice (state->output_tmp_stream, zlib_in,
                                  G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                  cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (!ret)
    g_prefix_error (error, "opcode gunzip: ");
  return ret;
}

static gboolean
dispatch_close (OstreeRepo                 *repo,
                StaticDeltaExecutionState  *state,
                GCancellable               *cancellable,  
                GError                    **error)
{
  gboolean ret = FALSE;
  char tmp_checksum[65];

  if (state->checksum_index == state->n_checksums)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Too many close operations");
      goto out;
    }

  g_assert (state->output_tmp_stream);
  g_assert (state->output_tmp_path);

  if (!g_output_stream_close (state->output_tmp_stream, cancellable, error))
    goto out;

  g_clear_object (&state->output_tmp_stream);

  ostree_checksum_inplace_from_bytes (state->output_target, tmp_checksum);

  if (OSTREE_OBJECT_TYPE_IS_META (state->output_objtype))
    {
      gs_unref_variant GVariant *metadata = NULL;

      if (!ot_util_variant_map (state->output_tmp_path,
                                ostree_metadata_variant_type (state->output_objtype),
                                TRUE, &metadata, error))
        goto out;

      if (!ostree_repo_write_metadata (repo, state->output_objtype, tmp_checksum,
                                       metadata, NULL, cancellable, error))
        goto out;

      g_print ("Wrote metadata object '%s'\n",
               tmp_checksum);
    }
  else
    {
      gs_unref_object GInputStream *in = NULL;
      gs_unref_object GFileInfo *info = NULL;

      in = (GInputStream*)g_file_read (state->output_tmp_path, cancellable, error);
      if (!in)
        goto out;

      info = g_file_input_stream_query_info ((GFileInputStream*)in, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                             cancellable, error);
      if (!info)
        goto out;
      
      if (!ostree_repo_write_content (repo, tmp_checksum, in,
                                      g_file_info_get_size (info), NULL,
                                      cancellable, error))
        goto out;

      g_print ("Wrote content object '%s'\n",
               tmp_checksum);
    }

  state->output_target = NULL;
  g_clear_object (&state->output_tmp_path);

  state->checksum_index++;
  if (state->checksum_index < state->n_checksums)
    {
      if (!open_output_target_csum (repo, state, cancellable, error))
        goto out;
    }
      
  ret = TRUE;
 out:
  if (!ret)
    g_prefix_error (error, "opcode close: ");
  return ret;
}
