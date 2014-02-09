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

#include <glib-unix.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

#include "ostree-repo-private.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-lzma-decompressor.h"
#include "otutil.h"
#include "ostree-varint.h"

/* This should really always be true, but hey, let's just assert it */
G_STATIC_ASSERT (sizeof (guint) >= sizeof (guint32));

typedef struct {
  OstreeRepo     *repo;
  guint           checksum_index;
  const guint8   *checksums;
  guint           n_checksums;

  const guint8   *opdata;
  guint           oplen;
  
  gboolean        object_start;
  guint           outstanding_content_writes;
  GMainContext   *content_writing_context;
  gboolean        caught_error;
  GError        **async_error;

  OstreeObjectType output_objtype;
  const guint8   *output_target;
  GFile          *output_tmp_path;
  GOutputStream  *output_tmp_stream;
  const guint8   *input_target_csum;

  const guint8   *payload_data;
  guint64         payload_size; 
} StaticDeltaExecutionState;

typedef struct {
  StaticDeltaExecutionState *state;
  char checksum[65];
} StaticDeltaContentWrite;

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

OPPROTO(write)
OPPROTO(gunzip)
OPPROTO(close)
#undef OPPROTO

static OstreeStaticDeltaOperation op_dispatch_table[] = {
  { "write", dispatch_write },
  { "gunzip", dispatch_gunzip },
  { "close", dispatch_close },
  { NULL }
};

static void
on_content_written (GObject          *src,
                    GAsyncResult     *result,
                    gpointer          user_data);

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
open_output_target (StaticDeltaExecutionState   *state,
                    GCancellable                *cancellable,
                    GError                     **error)
{
  gboolean ret = FALSE;
  guint8 *objcsum;
  char checksum[65];
  guint64 object_size;
  gs_unref_object GInputStream *content_in_stream = NULL;

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

  ostree_checksum_inplace_from_bytes (state->output_target, checksum);

  /* Object size is the first element of the opstream */
  if (!read_varuint64 (state, &object_size, error))
    goto out;

  if (OSTREE_OBJECT_TYPE_IS_META (state->output_objtype))
    {
      if (!gs_file_open_in_tmpdir (state->repo->tmp_dir, 0644,
                                   &state->output_tmp_path, &state->output_tmp_stream,
                                   cancellable, error))
        goto out;
    }
  else
    {
      int pipefds[2];

      if (!g_unix_open_pipe (pipefds, FD_CLOEXEC, error))
        goto out;

      content_in_stream = g_unix_input_stream_new (pipefds[0], TRUE);
      state->output_tmp_stream = g_unix_output_stream_new (pipefds[1], TRUE);
      
      if (!state->content_writing_context)
        state->content_writing_context = g_main_context_new();
      
      g_main_context_push_thread_default (state->content_writing_context);
      
      {
        StaticDeltaContentWrite *writedata = g_new0 (StaticDeltaContentWrite, 1);
        writedata->state = state;
        memcpy (writedata->checksum, checksum, sizeof (writedata->checksum));
        ostree_repo_write_content_async (state->repo, checksum,
                                         content_in_stream,
                                         object_size,
                                         cancellable,
                                         on_content_written,
                                         writedata);
      }
      state->outstanding_content_writes++;
      
      g_main_context_pop_thread_default (state->content_writing_context);
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
_ostree_static_delta_part_validate (OstreeRepo     *repo,
                                    GFile          *part_path,
                                    guint           part_offset,
                                    const char     *expected_checksum,
                                    GCancellable   *cancellable,
                                    GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_object GInputStream *tmp_in = NULL;
  gs_free guchar *actual_checksum_bytes = NULL;
  gs_free gchar *actual_checksum = NULL;
  
  tmp_in = (GInputStream*)g_file_read (part_path, cancellable, error);
  if (!tmp_in)
    goto out;
  
  if (!ot_gio_checksum_stream (tmp_in, &actual_checksum_bytes,
                               cancellable, error))
    goto out;

  actual_checksum = ostree_checksum_from_bytes (actual_checksum_bytes);
  if (strcmp (actual_checksum, expected_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Checksum mismatch in static delta part %u; expected=%s actual=%s",
                   part_offset, expected_checksum, actual_checksum);
      goto out;
    }
  
  ret = TRUE;
 out:
  return ret;
}

gboolean
_ostree_static_delta_part_execute_raw (OstreeRepo      *repo,
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

  state->repo = repo;
  state->async_error = error;

  if (!_ostree_static_delta_parse_checksum_array (objects,
                                                  &checksums_data,
                                                  &state->n_checksums,
                                                  error))
    goto out;

  state->checksums = checksums_data;
  g_assert (state->n_checksums > 0);

  g_variant_get (part, "(@ay@ay)", &payload, &ops);

  state->payload_data = g_variant_get_data (payload);
  state->payload_size = g_variant_get_size (payload);

  state->oplen = g_variant_n_children (ops);
  state->opdata = g_variant_get_data (ops);
  state->object_start = TRUE;
  while (state->oplen > 0)
    {
      guint8 opcode;
      OstreeStaticDeltaOperation *op;

      if (state->object_start)
        {
          if (!open_output_target (state, cancellable, error))
            goto out;
          state->object_start = FALSE;
        }

      opcode = state->opdata[0];

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

  while (state->outstanding_content_writes > 0)
    g_main_context_iteration (state->content_writing_context, TRUE);

  if (state->caught_error)
    goto out;

  ret = TRUE;
 out:
  g_clear_pointer (&state->content_writing_context, g_main_context_unref);
  g_clear_object (&state->output_tmp_path);
  g_clear_object (&state->output_tmp_stream);
  return ret;
}

static gboolean
decompress_all (GConverter   *converter,
                GBytes       *data,
                GBytes      **out_uncompressed,
                GCancellable *cancellable,
                GError      **error)
{
  gboolean ret = FALSE;
  gs_unref_object GMemoryInputStream *memin = (GMemoryInputStream*)g_memory_input_stream_new_from_bytes (data);
  gs_unref_object GMemoryOutputStream *memout = (GMemoryOutputStream*)g_memory_output_stream_new (NULL, 0, g_realloc, g_free);
  gs_unref_object GInputStream *convin = g_converter_input_stream_new ((GInputStream*)memin, converter);

  if (0 > g_output_stream_splice ((GOutputStream*)memout, convin,
                                  G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                  G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                  cancellable, error))
    goto out;

  ret = TRUE;
  *out_uncompressed = g_memory_output_stream_steal_as_bytes (memout);
 out:
  return ret;
}

gboolean
_ostree_static_delta_part_execute (OstreeRepo      *repo,
                                   GVariant        *header,
                                   GBytes          *part_bytes,
                                   GCancellable    *cancellable,
                                   GError         **error)
{
  gboolean ret = FALSE;
  gsize partlen;
  const guint8*partdata;
  gs_unref_bytes GBytes *part_payload_bytes = NULL;
  gs_unref_bytes GBytes *payload_data = NULL;
  gs_unref_variant GVariant *payload = NULL;
  guint8 comptype;

  partdata = g_bytes_get_data (part_bytes, &partlen);
  
  if (partlen < 1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted 0 length delta part");
      goto out;
    }
        
  /* First byte is compression type */
  comptype = partdata[0];
  /* Then the rest may be compressed or uncompressed */
  part_payload_bytes = g_bytes_new_from_bytes (part_bytes, 1, partlen - 1);
  switch (comptype)
    {
    case 0:
      /* No compression */
      payload_data = g_bytes_ref (part_payload_bytes);
      break;
    case 'g':
      {
        gs_unref_object GConverter *decomp =
          (GConverter*) g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW);

        if (!decompress_all (decomp, part_payload_bytes, &payload_data,
                             cancellable, error))
          goto out;
      }
      break;
    case 'x':
      {
        gs_unref_object GConverter *decomp =
          (GConverter*) _ostree_lzma_decompressor_new ();

        if (!decompress_all (decomp, part_payload_bytes, &payload_data,
                             cancellable, error))
          goto out;
      }
      break;
    default:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid compression type '%u'", comptype);
      goto out;
    }
        
  payload = ot_variant_new_from_bytes (G_VARIANT_TYPE (OSTREE_STATIC_DELTA_PART_PAYLOAD_FORMAT),
                                       payload_data, FALSE);
  if (!_ostree_static_delta_part_execute_raw (repo, header, payload,
                                              cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

typedef struct {
  OstreeRepo *repo;
  GVariant *header;
  GBytes *partdata;
  GCancellable *cancellable;
  GSimpleAsyncResult *result;
} StaticDeltaPartExecuteAsyncData;

static void
static_delta_part_execute_async_data_free (gpointer user_data)
{
  StaticDeltaPartExecuteAsyncData *data = user_data;

  g_clear_object (&data->repo);
  g_variant_unref (data->header);
  g_bytes_unref (data->partdata);
  g_clear_object (&data->cancellable);
  g_free (data);
}

static void
static_delta_part_execute_thread (GSimpleAsyncResult  *res,
                                  GObject             *object,
                                  GCancellable        *cancellable)
{
  GError *error = NULL;
  StaticDeltaPartExecuteAsyncData *data;

  data = g_simple_async_result_get_op_res_gpointer (res);
  if (!_ostree_static_delta_part_execute (data->repo,
                                          data->header,
                                          data->partdata,
                                          cancellable, &error))
    g_simple_async_result_take_error (res, error);
}

void
_ostree_static_delta_part_execute_async (OstreeRepo      *repo,
                                         GVariant        *header,
                                         GBytes          *partdata,
                                         GCancellable    *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer         user_data)
{
  StaticDeltaPartExecuteAsyncData *asyncdata;

  asyncdata = g_new0 (StaticDeltaPartExecuteAsyncData, 1);
  asyncdata->repo = g_object_ref (repo);
  asyncdata->header = g_variant_ref (header);
  asyncdata->partdata = g_bytes_ref (partdata);
  asyncdata->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  asyncdata->result = g_simple_async_result_new ((GObject*) repo,
                                                 callback, user_data,
                                                 _ostree_static_delta_part_execute_async);

  g_simple_async_result_set_op_res_gpointer (asyncdata->result, asyncdata,
                                             static_delta_part_execute_async_data_free);
  g_simple_async_result_run_in_thread (asyncdata->result, static_delta_part_execute_thread, G_PRIORITY_DEFAULT, cancellable);
  g_object_unref (asyncdata->result);
}

gboolean
_ostree_static_delta_part_execute_finish (OstreeRepo      *repo,
                                          GAsyncResult    *result,
                                          GError         **error) 
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == _ostree_static_delta_part_execute_async);

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;
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

static void
on_content_written (GObject          *src,
                    GAsyncResult     *result,
                    gpointer          user_data)
{
  StaticDeltaContentWrite *writedata = user_data;
  StaticDeltaExecutionState *state = writedata->state;
  GError *local_error = NULL;
  GError **error = &local_error;

  if (!ostree_repo_write_content_finish ((OstreeRepo*)src, result, NULL, error))
    goto out;

  g_print ("Wrote content object '%s'\n", writedata->checksum);

 out:
  state->outstanding_content_writes--;
  if (state->outstanding_content_writes == 0)
    g_main_context_wakeup (state->content_writing_context);
  if (local_error)
    {
      if (!state->caught_error)
        {
          state->caught_error = TRUE;
          g_main_context_wakeup (state->content_writing_context);
          g_propagate_error (state->async_error, local_error);
        }
      else
        {
          g_error_free (local_error);
        }
    }
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

  if (!g_output_stream_close (state->output_tmp_stream, cancellable, error))
    goto out;

  g_clear_object (&state->output_tmp_stream);

  ostree_checksum_inplace_from_bytes (state->output_target, tmp_checksum);

  if (OSTREE_OBJECT_TYPE_IS_META (state->output_objtype))
    {
      gs_unref_variant GVariant *metadata = NULL;
      
      g_assert (state->output_tmp_path);

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
      /* We already have an async write going, the close() above will
       * ensure it completes.
       */
    }

  state->output_target = NULL;
  g_clear_object (&state->output_tmp_path);

  state->object_start = TRUE;
  state->checksum_index++;
      
  ret = TRUE;
 out:
  if (!ret)
    g_prefix_error (error, "opcode close: ");
  return ret;
}
