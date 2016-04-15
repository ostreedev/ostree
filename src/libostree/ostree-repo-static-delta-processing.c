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
#include <gio/gfiledescriptorbased.h>

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-lzma-decompressor.h"
#include "otutil.h"
#include "ostree-varint.h"
#include "bsdiff/bspatch.h"

/* This should really always be true, but hey, let's just assert it */
G_STATIC_ASSERT (sizeof (guint) >= sizeof (guint32));

typedef struct {
  gboolean        trusted;
  gboolean        stats_only;
  OstreeRepo     *repo;
  guint           checksum_index;
  const guint8   *checksums;
  guint           n_checksums;

  const guint8   *opdata;
  guint           oplen;

  GVariant       *mode_dict;
  GVariant       *xattr_dict;
  
  gboolean        object_start;
  gboolean        caught_error;
  GError        **async_error;

  OstreeObjectType output_objtype;
  OstreeRepoContentBareCommit barecommitstate;
  guint64          content_size;
  GOutputStream   *content_out;
  char             checksum[65];
  char             *read_source_object;
  int               read_source_fd;
  gboolean        have_obj;
  guint32         uid;
  guint32         gid;
  guint32         mode;
  GVariant       *xattrs;
  
  const guint8   *output_target;
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

OPPROTO(open_splice_and_close)
OPPROTO(open)
OPPROTO(write)
OPPROTO(set_read_source)
OPPROTO(unset_read_source)
OPPROTO(close)
OPPROTO(bspatch)
#undef OPPROTO

static void
static_delta_execution_state_init (StaticDeltaExecutionState  *state)
{
  state->read_source_fd = -1;
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
open_output_target (StaticDeltaExecutionState   *state,
                    GCancellable                *cancellable,
                    GError                     **error)
{
  gboolean ret = FALSE;
  guint8 *objcsum;

  g_assert (state->checksums != NULL);
  g_assert (state->output_target == NULL);
  g_assert (state->checksum_index < state->n_checksums);

  objcsum = (guint8*)state->checksums + (state->checksum_index * OSTREE_STATIC_DELTA_OBJTYPE_CSUM_LEN);

  if (G_UNLIKELY(!ostree_validate_structureof_objtype (*objcsum, error)))
    goto out;

  state->output_objtype = (OstreeObjectType) *objcsum;
  state->output_target = objcsum + 1;

  ostree_checksum_inplace_from_bytes (state->output_target, state->checksum);

  ret = TRUE;
 out:
  return ret;
}

static guint
delta_opcode_index (OstreeStaticDeltaOpCode op)
{
  switch (op)
    {
    case OSTREE_STATIC_DELTA_OP_OPEN_SPLICE_AND_CLOSE:
      return 0;
    case OSTREE_STATIC_DELTA_OP_OPEN:
      return 1;
    case OSTREE_STATIC_DELTA_OP_WRITE:
      return 2;
    case OSTREE_STATIC_DELTA_OP_SET_READ_SOURCE:
      return 3;
    case OSTREE_STATIC_DELTA_OP_UNSET_READ_SOURCE:
      return 4;
    case OSTREE_STATIC_DELTA_OP_CLOSE:
      return 5;
    case OSTREE_STATIC_DELTA_OP_BSPATCH:
      return 6;
    default:
      g_assert_not_reached ();
    }
}

gboolean
_ostree_static_delta_part_execute (OstreeRepo      *repo,
                                   GVariant        *objects,
                                   GVariant        *part,
                                   gboolean         trusted,
                                   gboolean         stats_only,
                                   OstreeDeltaExecuteStats *stats,
                                   GCancellable    *cancellable,
                                   GError         **error)
{
  gboolean ret = FALSE;
  guint8 *checksums_data;
  g_autoptr(GVariant) checksums = NULL;
  g_autoptr(GVariant) mode_dict = NULL;
  g_autoptr(GVariant) xattr_dict = NULL;
  g_autoptr(GVariant) payload = NULL;
  g_autoptr(GVariant) ops = NULL;
  StaticDeltaExecutionState statedata = { 0, };
  StaticDeltaExecutionState *state = &statedata;
  guint n_executed = 0;

  static_delta_execution_state_init (&statedata);

  state->repo = repo;
  state->async_error = error;
  state->trusted = trusted;
  state->stats_only = stats_only;

  if (!_ostree_static_delta_parse_checksum_array (objects,
                                                  &checksums_data,
                                                  &state->n_checksums,
                                                  error))
    goto out;

  state->checksums = checksums_data;
  g_assert (state->n_checksums > 0);

  g_variant_get (part, "(@a(uuu)@aa(ayay)@ay@ay)",
                 &mode_dict,
                 &xattr_dict,
                 &payload, &ops);

  state->mode_dict = mode_dict;
  state->xattr_dict = xattr_dict;

  state->payload_data = g_variant_get_data (payload);
  state->payload_size = g_variant_get_size (payload);

  state->oplen = g_variant_n_children (ops);
  state->opdata = g_variant_get_data (ops);

  while (state->oplen > 0)
    {
      guint8 opcode;

      opcode = state->opdata[0];
      state->oplen--;
      state->opdata++;

      switch (opcode)
        {
        case OSTREE_STATIC_DELTA_OP_OPEN_SPLICE_AND_CLOSE:
          if (!dispatch_open_splice_and_close (repo, state, cancellable, error))
            goto out;
          break;
        case OSTREE_STATIC_DELTA_OP_OPEN:
          if (!dispatch_open (repo, state, cancellable, error))
            goto out;
          break;
        case OSTREE_STATIC_DELTA_OP_WRITE:
          if (!dispatch_write (repo, state, cancellable, error))
            goto out;
          break;
        case OSTREE_STATIC_DELTA_OP_SET_READ_SOURCE:
          if (!dispatch_set_read_source (repo, state, cancellable, error))
            goto out;
          break;
        case OSTREE_STATIC_DELTA_OP_UNSET_READ_SOURCE:
          if (!dispatch_unset_read_source (repo, state, cancellable, error))
            goto out;
          break;
        case OSTREE_STATIC_DELTA_OP_CLOSE:
          if (!dispatch_close (repo, state, cancellable, error))
            goto out;
          break;
        case OSTREE_STATIC_DELTA_OP_BSPATCH:
          if (!dispatch_bspatch (repo, state, cancellable, error))
            goto out;
          break;
        default:
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "Unknown opcode %u at offset %u", opcode, n_executed);
          goto out;
        }

      n_executed++;
      if (stats)
        stats->n_ops_executed[delta_opcode_index(opcode)]++;
    }

  if (state->caught_error)
    goto out;

  ret = TRUE;
 out:
  return ret;
}

typedef struct {
  OstreeRepo *repo;
  GVariant *header;
  GVariant *part;
  GCancellable *cancellable;
  GSimpleAsyncResult *result;
  gboolean trusted;
} StaticDeltaPartExecuteAsyncData;

static void
static_delta_part_execute_async_data_free (gpointer user_data)
{
  StaticDeltaPartExecuteAsyncData *data = user_data;

  g_clear_object (&data->repo);
  g_variant_unref (data->header);
  g_variant_unref (data->part);
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
                                          data->part,
                                          data->trusted,
                                          FALSE, NULL,
                                          cancellable, &error))
    g_simple_async_result_take_error (res, error);
}

void
_ostree_static_delta_part_execute_async (OstreeRepo      *repo,
                                         GVariant        *header,
                                         GVariant        *part,
                                         gboolean         trusted,
                                         GCancellable    *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer         user_data)
{
  StaticDeltaPartExecuteAsyncData *asyncdata;

  asyncdata = g_new0 (StaticDeltaPartExecuteAsyncData, 1);
  asyncdata->repo = g_object_ref (repo);
  asyncdata->header = g_variant_ref (header);
  asyncdata->part = g_variant_ref (part);
  asyncdata->trusted = trusted;
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

static gboolean
do_content_open_generic (OstreeRepo                 *repo,
                         StaticDeltaExecutionState  *state,
                         GCancellable               *cancellable,  
                         GError                    **error)
{
  gboolean ret = FALSE;
  g_autoptr(GVariant) modev = NULL;
  guint64 mode_offset;
  guint64 xattr_offset;
  guint32 uid, gid, mode;

  if (!read_varuint64 (state, &mode_offset, error))
    goto out;
  if (!read_varuint64 (state, &xattr_offset, error))
    goto out;

  state->barecommitstate.fd = -1;

  modev = g_variant_get_child_value (state->mode_dict, mode_offset);
  g_variant_get (modev, "(uuu)", &uid, &gid, &mode);
  state->uid = GUINT32_FROM_BE (uid);
  state->gid = GUINT32_FROM_BE (gid);
  state->mode = GUINT32_FROM_BE (mode);

  state->xattrs = g_variant_get_child_value (state->xattr_dict, xattr_offset);

  ret = TRUE;
 out:
  return ret;
}

struct bzpatch_opaque_s
{
  StaticDeltaExecutionState  *state;
  guint64 offset, length;
};

static int
bspatch_read (const struct bspatch_stream* stream, void* buffer, int length)
{
  struct bzpatch_opaque_s *opaque = stream->opaque;

  g_assert (length <= opaque->length);
  g_assert (opaque->offset + length <= opaque->state->payload_size);

  memcpy (buffer, opaque->state->payload_data + opaque->offset, length);
  opaque->offset += length;
  opaque->length -= length;
  return 0;
}

static gboolean
dispatch_bspatch (OstreeRepo                 *repo,
                  StaticDeltaExecutionState  *state,
                  GCancellable               *cancellable,
                  GError                    **error)
{
  gboolean ret = FALSE;
  guint64 offset, length;
  g_autoptr(GInputStream) in_stream = NULL;
  g_autoptr(GMappedFile) input_mfile = NULL;
  g_autofree guchar *buf = NULL;
  struct bspatch_stream stream;
  struct bzpatch_opaque_s opaque;
  gsize bytes_written;

  if (!read_varuint64 (state, &offset, error))
    goto out;
  if (!read_varuint64 (state, &length, error))
    goto out;

  if (state->stats_only)
    {
      ret = TRUE;
      goto out;
    }

  if (!state->have_obj)
    {
      input_mfile = g_mapped_file_new_from_fd (state->read_source_fd, FALSE, error);
      if (!input_mfile)
        goto out;

      buf = g_malloc0 (state->content_size);

      opaque.state = state;
      opaque.offset = offset;
      opaque.length = length;
      stream.read = bspatch_read;
      stream.opaque = &opaque;
      if (bspatch ((const guint8*)g_mapped_file_get_contents (input_mfile),
                   g_mapped_file_get_length (input_mfile),
                   buf,
                   state->content_size,
                   &stream) < 0)
        goto out;

      if (!g_output_stream_write_all (state->content_out,
                                      buf,
                                      state->content_size,
                                      &bytes_written,
                                      cancellable, error))
        goto out;

      g_assert (bytes_written == state->content_size);
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
dispatch_open_splice_and_close (OstreeRepo                 *repo,
                                StaticDeltaExecutionState  *state,
                                GCancellable               *cancellable,  
                                GError                    **error)
{
  gboolean ret = FALSE;

  if (!open_output_target (state, cancellable, error))
    goto out;

  if (OSTREE_OBJECT_TYPE_IS_META (state->output_objtype))
    {
      g_autoptr(GVariant) metadata = NULL;
      guint64 offset;
      guint64 length;

      if (!read_varuint64 (state, &length, error))
        goto out;
      if (!read_varuint64 (state, &offset, error))
        goto out;
      if (!validate_ofs (state, offset, length, error))
        goto out;

      if (state->stats_only)
        {
          ret = TRUE;
          goto out;
        }
      
      metadata = g_variant_new_from_data (ostree_metadata_variant_type (state->output_objtype),
                                          state->payload_data + offset, length, TRUE, NULL, NULL);

      if (state->trusted)
        {
          if (!ostree_repo_write_metadata_trusted (state->repo, state->output_objtype,
                                                   state->checksum,
                                                   metadata,
                                                   cancellable,
                                                   error))
            goto out;
        }
      else
        {
          g_autofree guchar *actual_csum = NULL;

          if (!ostree_repo_write_metadata (state->repo, state->output_objtype,
                                           state->checksum,
                                           metadata, &actual_csum,
                                           cancellable,
                                           error))
            goto out;
        }
    }
  else
    {
      guint64 content_offset;
      guint64 objlen;
      gsize bytes_written;
      g_autoptr(GInputStream) object_input = NULL;
      g_autoptr(GInputStream) memin = NULL;
      
      if (!do_content_open_generic (repo, state, cancellable, error))
        goto out;

      if (!read_varuint64 (state, &state->content_size, error))
        goto out;
      if (!read_varuint64 (state, &content_offset, error))
        goto out;
      if (!validate_ofs (state, content_offset, state->content_size, error))
        goto out;
      
      if (state->stats_only)
        {
          ret = TRUE;
          goto out;
        }

      /* Fast path for regular files to bare repositories */
      if (S_ISREG (state->mode) && 
          (repo->mode == OSTREE_REPO_MODE_BARE ||
           repo->mode == OSTREE_REPO_MODE_BARE_USER))
        {
          if (state->trusted)
            {
              if (!_ostree_repo_open_trusted_content_bare (repo, state->checksum,
                                                           state->content_size,
                                                           &state->barecommitstate,
                                                           &state->content_out,
                                                           &state->have_obj,
                                                           cancellable, error))
                goto out;
            }
          else
            {
              if (!_ostree_repo_open_untrusted_content_bare (repo, state->checksum,
                                                             state->content_size,
                                                             &state->barecommitstate,
                                                             &state->content_out,
                                                             &state->have_obj,
                                                             cancellable, error))
                goto out;
            }

          if (!state->have_obj)
            {
              if (!g_output_stream_write_all (state->content_out,
                                              state->payload_data + content_offset,
                                              state->content_size,
                                              &bytes_written,
                                              cancellable, error))
                goto out;
            }
        }
      else
        {
          /* Slower path, for symlinks and unpacking deltas into archive-z2 */
          g_autoptr(GFileInfo) finfo = NULL;
      
          finfo = _ostree_header_gfile_info_new (state->mode, state->uid, state->gid);

          if (S_ISLNK (state->mode))
            {
              g_autofree char *nulterminated_target =
                g_strndup ((char*)state->payload_data + content_offset, state->content_size);
              g_file_info_set_symlink_target (finfo, nulterminated_target);
            }
          else
            {
              g_assert (S_ISREG (state->mode));
              g_file_info_set_size (finfo, state->content_size);
              memin = g_memory_input_stream_new_from_data (state->payload_data + content_offset, state->content_size, NULL);
            }

          if (!ostree_raw_file_to_content_stream (memin, finfo, state->xattrs,
                                                  &object_input, &objlen,
                                                  cancellable, error))
            goto out;
          
          if (state->trusted)
            {
              if (!ostree_repo_write_content_trusted (state->repo,
                                                      state->checksum,
                                                      object_input,
                                                      objlen,
                                                      cancellable,
                                                      error))
                goto out;
            }
          else
            {
              g_autofree guchar *actual_csum = NULL;
              if (!ostree_repo_write_content (state->repo,
                                              state->checksum,
                                              object_input,
                                              objlen,
                                              &actual_csum,
                                              cancellable,
                                              error))
                goto out;
            }
        }
    }

  if (!dispatch_close (repo, state, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (state->stats_only)
    (void) dispatch_close (repo, state, cancellable, NULL);
  if (!ret)
    g_prefix_error (error, "opcode open-splice-and-close: ");
  return ret;
}

static gboolean
dispatch_open (OstreeRepo                 *repo,
               StaticDeltaExecutionState  *state,
               GCancellable               *cancellable,  
               GError                    **error)
{
  gboolean ret = FALSE;

  g_assert (state->output_target == NULL);
  /* FIXME - lift this restriction */
  if (!state->stats_only)
    {
      g_assert (repo->mode == OSTREE_REPO_MODE_BARE ||
                repo->mode == OSTREE_REPO_MODE_BARE_USER);
    }
  
  if (!open_output_target (state, cancellable, error))
    goto out;

  if (!do_content_open_generic (repo, state, cancellable, error))
    goto out;

  if (!read_varuint64 (state, &state->content_size, error))
    goto out;

  if (state->stats_only)
    {
      ret = TRUE;
      goto out;
    }

  if (state->trusted)
    {
      if (!_ostree_repo_open_trusted_content_bare (repo, state->checksum,
                                                   state->content_size,
                                                   &state->barecommitstate,
                                                   &state->content_out,
                                                   &state->have_obj,
                                                   cancellable, error))
        goto out;
    }
  else
    {
      if (!_ostree_repo_open_untrusted_content_bare (repo, state->checksum,
                                                     state->content_size,
                                                     &state->barecommitstate,
                                                     &state->content_out,
                                                     &state->have_obj,
                                                     cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  if (!ret)
    g_prefix_error (error, "opcode open: ");
  return ret;
}

static gboolean
dispatch_write (OstreeRepo                 *repo,
               StaticDeltaExecutionState  *state,
               GCancellable               *cancellable,  
               GError                    **error)
{
  gboolean ret = FALSE;
  guint64 content_size;
  guint64 content_offset;
  gsize bytes_written;
      
  if (!read_varuint64 (state, &content_size, error))
    goto out;
  if (!read_varuint64 (state, &content_offset, error))
    goto out;

  if (state->stats_only)
    {
      ret = TRUE;
      goto out;
    }

  if (!state->have_obj)
    {
      if (state->read_source_fd != -1)
        {
          if (lseek (state->read_source_fd, content_offset, SEEK_SET) == -1)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
          while (content_size > 0)
            {
              char buf[4096];
              gssize bytes_read;

              do
                bytes_read = read (state->read_source_fd, buf, MIN(sizeof(buf), content_size));
              while (G_UNLIKELY (bytes_read == -1 && errno == EINTR));
              if (bytes_read == -1)
                {
                  glnx_set_error_from_errno (error);
                  goto out;
                }
              if (G_UNLIKELY (bytes_read == 0))
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Unexpected EOF reading object %s", state->read_source_object);
                  goto out;
                }
              
              if (!g_output_stream_write_all (state->content_out,
                                              buf,
                                              bytes_read,
                                              &bytes_written,
                                              cancellable, error))
                goto out;
              
              content_size -= bytes_read;
            }
        }
      else
        {
          if (!validate_ofs (state, content_offset, content_size, error))
            goto out;

          if (!g_output_stream_write_all (state->content_out,
                                          state->payload_data + content_offset,
                                          content_size,
                                          &bytes_written,
                                          cancellable, error))
            goto out;
        }
    }
  
  ret = TRUE;
 out:
  if (!ret)
    g_prefix_error (error, "opcode open-splice-and-close: ");
  return ret;
}

static gboolean
dispatch_set_read_source (OstreeRepo                 *repo,
                          StaticDeltaExecutionState  *state,
                          GCancellable               *cancellable,  
                          GError                    **error)
{
  gboolean ret = FALSE;
  guint64 source_offset;

  if (state->read_source_fd)
    {
      (void) close (state->read_source_fd);
      state->read_source_fd = -1;
    }

  if (!read_varuint64 (state, &source_offset, error))
    goto out;
  if (!validate_ofs (state, source_offset, 32, error))
    goto out;

  if (state->stats_only)
    {
      ret = TRUE;
      goto out;
    }

  g_free (state->read_source_object);
  state->read_source_object = ostree_checksum_from_bytes (state->payload_data + source_offset);
  
  if (!_ostree_repo_read_bare_fd (repo, state->read_source_object, &state->read_source_fd,
                                  cancellable, error))
    goto out;
  
  ret = TRUE;
 out:
  if (!ret)
    g_prefix_error (error, "opcode set-read-source: ");
  return ret;
}

static gboolean
dispatch_unset_read_source (OstreeRepo                 *repo,
                            StaticDeltaExecutionState  *state,
                            GCancellable               *cancellable,  
                            GError                    **error)
{
  gboolean ret = FALSE;

  if (state->stats_only)
    {
      ret = TRUE;
      goto out;
    }

  if (state->read_source_fd)
    {
      (void) close (state->read_source_fd);
      state->read_source_fd = -1;
    }

  g_clear_pointer (&state->read_source_object, g_free);
  
  ret = TRUE;
 out:
  if (!ret)
    g_prefix_error (error, "opcode unset-read-source: ");
  return ret;
}

static gboolean
dispatch_close (OstreeRepo                 *repo,
                StaticDeltaExecutionState  *state,
                GCancellable               *cancellable,  
                GError                    **error)
{
  gboolean ret = FALSE;
  
  if (state->content_out)
    {
      if (!g_output_stream_flush (state->content_out, cancellable, error))
        goto out;

      if (state->trusted)
        {
          if (!_ostree_repo_commit_trusted_content_bare (repo, state->checksum, &state->barecommitstate,
                                                         state->uid, state->gid, state->mode,
                                                         state->xattrs,
                                                         cancellable, error))
            goto out;
        }
      else
        {
          if (!_ostree_repo_commit_untrusted_content_bare (repo, state->checksum, &state->barecommitstate,
                                                           state->uid, state->gid, state->mode,
                                                           state->xattrs,
                                                           cancellable, error))
            goto out;
        }
    }

  if (!dispatch_unset_read_source (repo, state, cancellable, error))
    goto out;
      
  g_clear_pointer (&state->xattrs, g_variant_unref);
  g_clear_object (&state->content_out);
  
  state->checksum_index++;
  state->output_target = NULL;

  ret = TRUE;
 out:
  if (!ret)
    g_prefix_error (error, "opcode open-splice-and-close: ");
  return ret;
}
