/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2012 Colin Walters <walters@verbum.org>
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

#include "ostree-curl-fetcher.h"
#include "ostree.h"

struct OstreeCurlFetcher
{
  GObject parent_instance;

  GFile *tmpdir;
  
  GSSubprocess *curl_proc;
  GQueue *queue;
};

G_DEFINE_TYPE (OstreeCurlFetcher, ostree_curl_fetcher, G_TYPE_OBJECT)

static void
ostree_curl_fetcher_finalize (GObject *object)
{
  OstreeCurlFetcher *self;

  self = OSTREE_CURL_FETCHER (object);

  g_clear_object (&self->curl_proc);
  g_queue_free (self->queue);

  G_OBJECT_CLASS (ostree_curl_fetcher_parent_class)->finalize (object);
}

static void
ostree_curl_fetcher_class_init (OstreeCurlFetcherClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = ostree_curl_fetcher_finalize;
}

static void
ostree_curl_fetcher_init (OstreeCurlFetcher *self)
{
  self->queue = g_queue_new ();
}

OstreeCurlFetcher *
ostree_curl_fetcher_new (GFile *tmpdir)
{
  OstreeCurlFetcher *self = (OstreeCurlFetcher*)g_object_new (OSTREE_TYPE_CURL_FETCHER, NULL);

  self->tmpdir = g_object_ref (tmpdir);
 
  return self;
}

typedef struct {
  OstreeCurlFetcher *self;
  gchar *uri;
  GFile *tmpfile;
  GCancellable *cancellable;
  GSimpleAsyncResult *result;
} OstreeCurlFetcherOp;

static void
fetcher_op_free (OstreeCurlFetcherOp *op)
{
  g_clear_object (&op->self);
  g_free (op->uri);
  g_clear_object (&op->tmpfile);
  g_clear_object (&op->cancellable);
  g_free (op);
}

static void
maybe_fetch (OstreeCurlFetcher     *self);

static void
on_curl_exited (GObject       *object,
                GAsyncResult  *result,
                gpointer       user_data)
{
  GSSubprocess *proc = GS_SUBPROCESS (object);
  OstreeCurlFetcherOp *op = user_data;
  GError *error = NULL;
  int estatus;
  
  if (!gs_subprocess_wait_finish (proc, result, &estatus, &error))
    goto out;

  if (!g_spawn_check_exit_status (estatus, &error))
    goto out;

 out:
  if (error)
      g_simple_async_result_take_error (op->result, error);
  
  g_simple_async_result_complete (op->result);
  
  g_clear_object (&op->self->curl_proc);

  maybe_fetch (op->self);

  g_object_unref (op->result);
}

static void
maybe_fetch (OstreeCurlFetcher     *self)
{
  OstreeCurlFetcherOp *op;
  GError *error = NULL;
  gs_unref_object GSSubprocessContext *context = NULL;

  if (self->curl_proc != NULL
      || g_queue_is_empty (self->queue))
    return;

  op = g_queue_pop_head (self->queue);

  if (!ostree_create_temp_regular_file (self->tmpdir, NULL, NULL,
                                        &op->tmpfile, NULL,
                                        op->cancellable, &error))
    goto out;

  context = gs_subprocess_context_newv ("curl", op->uri, "-o",
                                        gs_file_get_path_cached (op->tmpfile),
                                        NULL);
  g_assert (self->curl_proc == NULL);
  self->curl_proc = gs_subprocess_new (context, op->cancellable, &error);
  if (!self->curl_proc)
    goto out;

  gs_subprocess_wait (self->curl_proc, op->cancellable,
                      on_curl_exited, op);

 out:
  if (error)
    {
      g_simple_async_result_take_error (op->result, error);
      g_simple_async_result_complete (op->result);
    }
}

void
ostree_curl_fetcher_request_uri_async (OstreeCurlFetcher          *self,
                                       const char                 *uri,
                                       GCancellable               *cancellable,
                                       GAsyncReadyCallback         callback,
                                       gpointer                    user_data)
{
  OstreeCurlFetcherOp *op;

  op = g_new0 (OstreeCurlFetcherOp, 1);
  op->self = g_object_ref (self);
  op->uri = g_strdup (uri);
  op->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  op->result = g_simple_async_result_new ((GObject*) self, callback, user_data,
                                          ostree_curl_fetcher_request_uri_async);

  g_queue_push_tail (self->queue, op);

  g_simple_async_result_set_op_res_gpointer (op->result, op,
                                             (GDestroyNotify) fetcher_op_free);
  
  maybe_fetch (self);
}

GFile *
ostree_curl_fetcher_request_uri_finish (OstreeCurlFetcher     *self,
                                        GAsyncResult          *result,
                                        GError               **error)
{
  GSimpleAsyncResult *simple;
  OstreeCurlFetcherOp *op;

  g_return_val_if_fail (g_simple_async_result_is_valid (result, (GObject*)self, ostree_curl_fetcher_request_uri_async), FALSE);

  simple = G_SIMPLE_ASYNC_RESULT (result);
  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;
  op = g_simple_async_result_get_op_res_gpointer (simple);

  return g_object_ref (op->tmpfile);
}
