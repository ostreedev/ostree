/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2012,2013 Colin Walters <walters@verbum.org>
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

/**
 * See:
 * https://mail.gnome.org/archives/ostree-list/2012-August/msg00021.html
 *
 * DESIGN:
 *
 * Pull refs
 *   For each ref:
 *     Queue scan of commit
 *
 * Mainloop:
 *  Process requests, await idle scan
 *  
 * Async queue:
 *  Scan commit
 *   If already cached, recursively scan content
 *   If not, queue fetch
 * 
 *  For each commit:
 *    Verify checksum
 *    Import
 *    Traverse and queue dirtree/dirmeta
 * 
 * Pull dirtrees:
 *  For each dirtree:
 *    Verify checksum
 *    Import
 *    Traverse and queue content/dirtree/dirmeta
 *
 * Pull content meta:
 *  For each content:
 *    Pull meta
 *    If contentcontent needed:
 *      Queue contentcontent
 *    else:
 *      Import
 *
 * Pull contentcontent:
 *  For each contentcontent
 *    Verify checksum
 *    Import
 *    
 *  
 */

#include "config.h"

#include "ostree.h"
#include "ostree-fetcher.h"

typedef struct {
  enum {
    PULL_MSG_SCAN_IDLE,
    PULL_MSG_MAIN_IDLE,
    PULL_MSG_FETCH,
    PULL_MSG_SCAN,
    PULL_MSG_QUIT
  } t;
  union {
    guint     idle_serial;
    GVariant *item;
  } d;
} PullWorkerMessage;

typedef struct {
  OstreeRepo   *repo;
  OstreeRepoPullFlags flags;
  char         *remote_name;
  OstreeRepoMode remote_mode;
  OstreeFetcher *fetcher;
  SoupURI      *base_uri;

  GMainContext    *main_context;
  GMainLoop    *loop;
  GCancellable *cancellable;

  gboolean      transaction_resuming;
  volatile gint n_scanned_metadata;
  guint         outstanding_uri_requests;

  GThread          *metadata_thread;
  GMainContext     *metadata_thread_context;
  GMainLoop        *metadata_thread_loop;
  OtWaitableQueue  *metadata_objects_to_scan;
  OtWaitableQueue  *metadata_objects_to_fetch;
  GHashTable       *scanned_metadata; /* Maps object name to itself */
  GHashTable       *requested_metadata; /* Maps object name to itself */
  GHashTable       *requested_content; /* Maps object name to itself */
  guint             metadata_scan_idle : 1; /* TRUE if we passed through an idle message */
  guint             idle_serial; /* Incremented when we get a SCAN_IDLE message */
  guint             n_outstanding_metadata_fetches;
  guint             n_outstanding_metadata_stage_requests;
  guint             n_outstanding_content_fetches;
  guint             n_outstanding_content_stage_requests;
  gint              n_requested_metadata;
  gint              n_requested_content;
  guint             n_fetched_metadata;
  guint             n_fetched_content;

  gboolean      have_previous_bytes;
  guint64       previous_bytes_sec;
  guint64       previous_total_downloaded;

  GError      **async_error;
  gboolean      caught_error;
} OtPullData;

typedef struct {
  OtPullData  *pull_data;
  GVariant    *object;
  GFile       *temp_path;
} FetchObjectData;

static SoupURI *
suburi_new (SoupURI   *base,
            const char *first,
            ...) G_GNUC_NULL_TERMINATED;

static gboolean scan_one_metadata_object (OtPullData         *pull_data,
                                          const guchar       *csum,
                                          OstreeObjectType    objtype,
                                          guint               recursion_depth,
                                          GCancellable       *cancellable,
                                          GError            **error);
static gboolean scan_one_metadata_object_v_name (OtPullData         *pull_data,
                                                 GVariant           *object,
                                                 GCancellable       *cancellable,
                                                 GError            **error);


static SoupURI *
suburi_new (SoupURI   *base,
            const char *first,
            ...)
{
  va_list args;
  GPtrArray *arg_array;
  const char *arg;
  char *subpath;
  SoupURI *ret;

  arg_array = g_ptr_array_new ();
  g_ptr_array_add (arg_array, (char*)soup_uri_get_path (base));
  g_ptr_array_add (arg_array, (char*)first);

  va_start (args, first);
  
  while ((arg = va_arg (args, const char *)) != NULL)
    g_ptr_array_add (arg_array, (char*)arg);
  g_ptr_array_add (arg_array, NULL);

  subpath = g_build_filenamev ((char**)arg_array->pdata);
  g_ptr_array_unref (arg_array);
  
  ret = soup_uri_copy (base);
  soup_uri_set_path (ret, subpath);
  g_free (subpath);
  
  va_end (args);
  
  return ret;
}

static gboolean
uri_fetch_update_status (gpointer user_data)
{
  OtPullData *pull_data = user_data;
  gs_free char *fetcher_status = NULL;
  GString *status;
  guint outstanding_stages;
  guint outstanding_fetches;
 
  status = g_string_new ("");

  if (!pull_data->metadata_scan_idle)
    g_string_append_printf (status, "scan: %u; ",
                            g_atomic_int_get (&pull_data->n_scanned_metadata));

  outstanding_stages = pull_data->n_outstanding_content_stage_requests + pull_data->n_outstanding_metadata_stage_requests;
  if (outstanding_stages > 0)
    g_string_append_printf (status, "writing: %u; ", outstanding_stages);

  outstanding_fetches = pull_data->n_outstanding_content_fetches + pull_data->n_outstanding_metadata_fetches;
  if (outstanding_fetches)
    {
      g_string_append_printf (status, "fetch: %u/%u meta %u/%u; ",
                              pull_data->n_fetched_metadata,
                              pull_data->n_requested_metadata,
                              pull_data->n_fetched_content,
                              pull_data->n_requested_content);

      fetcher_status = ostree_fetcher_query_state_text (pull_data->fetcher);
      g_string_append (status, fetcher_status);
    }

  gs_console_begin_status_line (gs_console_get (), status->str, NULL, NULL);

  g_string_free (status, TRUE);

  return TRUE;
}

static PullWorkerMessage *
pull_worker_message_new (int msgtype, gpointer data)
{
  PullWorkerMessage *msg = g_new (PullWorkerMessage, 1);
  msg->t = msgtype;
  switch (msgtype)
    {
    case PULL_MSG_SCAN_IDLE:
    case PULL_MSG_MAIN_IDLE:
      msg->d.idle_serial = GPOINTER_TO_UINT (data);
      break;
    case PULL_MSG_SCAN:
    case PULL_MSG_FETCH:
      msg->d.item = data;
      break;
    case PULL_MSG_QUIT:
      break;
    }
  return msg;
}

static void
throw_async_error (OtPullData          *pull_data,
                   GError              *error)
{
  if (error)
    {
      if (!pull_data->caught_error)
        {
          pull_data->caught_error = TRUE;
          g_propagate_error (pull_data->async_error, error);
          g_main_loop_quit (pull_data->loop);
        }
      else
        {
          g_error_free (error);
        }
    }
}

static void
check_outstanding_requests_handle_error (OtPullData          *pull_data,
                                         GError              *error)
{
  gboolean current_fetch_idle = (pull_data->n_outstanding_metadata_fetches == 0 &&
                                 pull_data->n_outstanding_content_fetches == 0);
  gboolean current_stage_idle = (pull_data->n_outstanding_metadata_stage_requests == 0 &&
                                 pull_data->n_outstanding_content_stage_requests == 0);

  g_debug ("pull: scan: %u fetching: %u staging: %u",
           !pull_data->metadata_scan_idle, !current_fetch_idle, !current_stage_idle);

  throw_async_error (pull_data, error);

  /* This is true in the phase when we're fetching refs */
  if (pull_data->metadata_objects_to_scan == NULL)
    {
      if (pull_data->outstanding_uri_requests == 0)
        g_main_loop_quit (pull_data->loop);
      return;
    }
  else if (pull_data->metadata_scan_idle && current_fetch_idle && current_stage_idle)
    {
      g_main_loop_quit (pull_data->loop);
    }
}

static gboolean
idle_check_outstanding_requests (gpointer user_data)
{
  check_outstanding_requests_handle_error (user_data, NULL);
  return FALSE;
}

static gboolean
run_mainloop_monitor_fetcher (OtPullData   *pull_data)
{
  GSource *update_timeout = NULL;
  GSConsole *console;
  GSource *idle_src;

  console = gs_console_get ();

  if (console)
    {
      gs_console_begin_status_line (console, "", NULL, NULL);

      update_timeout = g_timeout_source_new_seconds (1);
      g_source_set_callback (update_timeout, uri_fetch_update_status, pull_data, NULL);
      g_source_attach (update_timeout, g_main_loop_get_context (pull_data->loop));
      g_source_unref (update_timeout);
    }
  
  idle_src = g_idle_source_new ();
  g_source_set_callback (idle_src, idle_check_outstanding_requests, pull_data, NULL);
  g_source_attach (idle_src, pull_data->main_context);
  g_main_loop_run (pull_data->loop);

  if (console)
    {
      gs_console_end_status_line (console, NULL, NULL);
      g_source_destroy (update_timeout);
    }

  return !pull_data->caught_error;
}

typedef struct {
  OtPullData     *pull_data;
  GFile          *result_file;
} OstreeFetchUriData;

static void
uri_fetch_on_complete (GObject        *object,
                       GAsyncResult   *result,
                       gpointer        user_data) 
{
  OstreeFetchUriData *data = user_data;
  GError *local_error = NULL;

  data->result_file = ostree_fetcher_request_uri_finish ((OstreeFetcher*)object,
                                                         result, &local_error);
  data->pull_data->outstanding_uri_requests--;
  check_outstanding_requests_handle_error (data->pull_data, local_error);
}

static gboolean
fetch_uri (OtPullData  *pull_data,
           SoupURI     *uri,
           const char  *tmp_prefix,
           GFile      **out_temp_filename,
           GCancellable  *cancellable,
           GError     **error)
{
  gboolean ret = FALSE;
  gs_free char *uri_string = NULL;
  gs_unref_object SoupRequest *request = NULL;
  OstreeFetchUriData fetch_data;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  memset (&fetch_data, 0, sizeof (fetch_data));
  fetch_data.pull_data = pull_data;

  uri_string = soup_uri_to_string (uri, FALSE);

  pull_data->outstanding_uri_requests++;
  ostree_fetcher_request_uri_async (pull_data->fetcher, uri, cancellable,
                                    uri_fetch_on_complete, &fetch_data);

  if (!run_mainloop_monitor_fetcher (pull_data))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_temp_filename, &fetch_data.result_file);
 out:
  return ret;
}

static gboolean
fetch_uri_contents_utf8 (OtPullData  *pull_data,
                         SoupURI     *uri,
                         char       **out_contents,
                         GCancellable  *cancellable,
                         GError     **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *tmpf = NULL;
  gs_free char *ret_contents = NULL;
  gsize len;

  if (!fetch_uri (pull_data, uri, "tmp-", &tmpf, cancellable, error))
    goto out;

  if (!g_file_load_contents (tmpf, cancellable, &ret_contents, &len, NULL, error))
    goto out;

  if (!g_utf8_validate (ret_contents, -1, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid UTF-8");
      goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_contents, &ret_contents);
 out:
  if (tmpf)
    (void) unlink (gs_file_get_path_cached (tmpf));
  return ret;
}

static gboolean
scan_dirtree_object (OtPullData   *pull_data,
                     const char   *checksum,
                     int           recursion_depth,
                     GCancellable *cancellable,
                     GError      **error)
{
  gboolean ret = FALSE;
  int i, n;
  gs_unref_variant GVariant *tree = NULL;
  gs_unref_variant GVariant *files_variant = NULL;
  gs_unref_variant GVariant *dirs_variant = NULL;
  gs_unref_object GFile *stored_path = NULL;

  if (recursion_depth > OSTREE_MAX_RECURSION)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exceeded maximum recursion");
      goto out;
    }

  if (!ostree_repo_load_variant (pull_data->repo, OSTREE_OBJECT_TYPE_DIR_TREE, checksum,
                                 &tree, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
  files_variant = g_variant_get_child_value (tree, 0);
  dirs_variant = g_variant_get_child_value (tree, 1);
      
  n = g_variant_n_children (files_variant);
  for (i = 0; i < n; i++)
    {
      const char *filename;
      gboolean file_is_stored;
      gs_unref_variant GVariant *csum = NULL;
      gs_free char *file_checksum;

      g_variant_get_child (files_variant, i, "(&s@ay)", &filename, &csum);

      if (!ot_util_filename_validate (filename, error))
        goto out;

      file_checksum = ostree_checksum_from_bytes_v (csum);

      if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_FILE, file_checksum,
                                   &file_is_stored, cancellable, error))
        goto out;
      
      if (!file_is_stored && !g_hash_table_lookup (pull_data->requested_content, file_checksum))
        {
          g_hash_table_insert (pull_data->requested_content, file_checksum, file_checksum);
      
          ot_waitable_queue_push (pull_data->metadata_objects_to_fetch,
                                  pull_worker_message_new (PULL_MSG_FETCH,
                                                           ostree_object_name_serialize (file_checksum, OSTREE_OBJECT_TYPE_FILE)));
          file_checksum = NULL; /* Transfer ownership to hash */
        }
    }
      
  n = g_variant_n_children (dirs_variant);
  for (i = 0; i < n; i++)
    {
      const char *dirname;
      gs_unref_variant GVariant *tree_csum = NULL;
      gs_unref_variant GVariant *meta_csum = NULL;
      gs_free char *tmp_checksum = NULL;

      g_variant_get_child (dirs_variant, i, "(&s@ay@ay)",
                           &dirname, &tree_csum, &meta_csum);

      if (!ot_util_filename_validate (dirname, error))
        goto out;

      if (!scan_one_metadata_object (pull_data, ostree_checksum_bytes_peek (tree_csum),
                                     OSTREE_OBJECT_TYPE_DIR_TREE, recursion_depth + 1,
                                     cancellable, error))
        goto out;
      
      if (!scan_one_metadata_object (pull_data, ostree_checksum_bytes_peek (meta_csum),
                                     OSTREE_OBJECT_TYPE_DIR_META, recursion_depth + 1,
                                     cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
fetch_ref_contents (OtPullData    *pull_data,
                    const char    *ref,
                    char         **out_contents,
                    GCancellable  *cancellable,
                    GError       **error)
{
  gboolean ret = FALSE;
  gs_free char *ret_contents = NULL;
  SoupURI *target_uri = NULL;

  target_uri = suburi_new (pull_data->base_uri, "refs", "heads", ref, NULL);
  
  if (!fetch_uri_contents_utf8 (pull_data, target_uri, &ret_contents, cancellable, error))
    goto out;

  g_strchomp (ret_contents);

  if (!ostree_validate_checksum_string (ret_contents, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_contents, &ret_contents);
 out:
  if (target_uri)
    soup_uri_free (target_uri);
  return ret;
}

static void
content_fetch_on_stage_complete (GObject        *object,
                                 GAsyncResult   *result,
                                 gpointer        user_data)
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  OstreeObjectType objtype;
  const char *expected_checksum;
  gs_free guchar *csum = NULL;
  gs_free char *checksum = NULL;

  if (!ostree_repo_stage_content_finish ((OstreeRepo*)object, result, 
                                         &csum, error))
    goto out;

  checksum = ostree_checksum_from_bytes (csum);

  ostree_object_name_deserialize (fetch_data->object, &expected_checksum, &objtype);
  g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);

  g_debug ("stage of %s complete", ostree_object_to_string (checksum, objtype));

  if (strcmp (checksum, expected_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted content object; checksum expected='%s' actual='%s'",
                   expected_checksum, checksum);
      goto out;
    }

  pull_data->n_fetched_content++;
 out:
  pull_data->n_outstanding_content_stage_requests--;
  check_outstanding_requests_handle_error (pull_data, local_error);
  (void) gs_file_unlink (fetch_data->temp_path, NULL, NULL);
  g_object_unref (fetch_data->temp_path);
  g_variant_unref (fetch_data->object);
  g_free (fetch_data);
}

static void
content_fetch_on_complete (GObject        *object,
                           GAsyncResult   *result,
                           gpointer        user_data) 
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  GCancellable *cancellable = NULL;
  guint64 length;
  gs_unref_variant GVariant *file_meta = NULL;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_object GInputStream *content_input = NULL;
  gs_unref_object GInputStream *file_object_input = NULL;
  gs_unref_variant GVariant *xattrs = NULL;
  gs_unref_object GInputStream *file_in = NULL;
  gs_unref_object GInputStream *object_input = NULL;
  const char *checksum;
  OstreeObjectType objtype;

  fetch_data->temp_path = ostree_fetcher_request_uri_finish ((OstreeFetcher*)object, result, error);
  if (!fetch_data->temp_path)
    goto out;

  ostree_object_name_deserialize (fetch_data->object, &checksum, &objtype);
  g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);

  g_debug ("fetch of %s complete", ostree_object_to_string (checksum, objtype));

  if (!ostree_content_file_parse (TRUE, fetch_data->temp_path, FALSE,
                                  &file_in, &file_info, &xattrs,
                                  cancellable, error))
    goto out;

  if (!ostree_raw_file_to_content_stream (file_in, file_info, xattrs,
                                          &object_input, &length,
                                          cancellable, error))
    goto out;
  
  pull_data->n_outstanding_content_stage_requests++;
  ostree_repo_stage_content_async (pull_data->repo, checksum,
                                   object_input, length,
                                   cancellable,
                                   content_fetch_on_stage_complete, fetch_data);

 out:
  pull_data->n_outstanding_content_fetches--;
  check_outstanding_requests_handle_error (pull_data, local_error);
}

static void
on_metadata_staged (GObject           *object,
                    GAsyncResult      *result,
                    gpointer           user_data)
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  const char *expected_checksum;
  OstreeObjectType objtype;
  gs_free char *checksum = NULL;
  gs_free guchar *csum = NULL;

  if (!ostree_repo_stage_metadata_finish ((OstreeRepo*)object, result, 
                                          &csum, error))
    goto out;

  checksum = ostree_checksum_from_bytes (csum);

  ostree_object_name_deserialize (fetch_data->object, &expected_checksum, &objtype);
  g_assert (OSTREE_OBJECT_TYPE_IS_META (objtype));

  g_debug ("stage of %s complete", ostree_object_to_string (checksum, objtype));

  if (strcmp (checksum, expected_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted metadata object; checksum expected='%s' actual='%s'",
                   expected_checksum, checksum);
      goto out;
    }

  pull_data->metadata_scan_idle = FALSE;
  ot_waitable_queue_push (pull_data->metadata_objects_to_scan,
                          pull_worker_message_new (PULL_MSG_SCAN,
                                                  g_variant_ref (fetch_data->object)));
 out:
  pull_data->n_outstanding_metadata_stage_requests--;
  (void) gs_file_unlink (fetch_data->temp_path, NULL, NULL);
  g_object_unref (fetch_data->temp_path);
  g_variant_unref (fetch_data->object);
  g_free (fetch_data);

  check_outstanding_requests_handle_error (pull_data, local_error);
}

static void
meta_fetch_on_complete (GObject           *object,
                        GAsyncResult      *result,
                        gpointer           user_data)
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  gs_unref_variant GVariant *metadata = NULL;
  const char *checksum;
  OstreeObjectType objtype;
  GError *local_error = NULL;
  GError **error = &local_error;

  fetch_data->temp_path = ostree_fetcher_request_uri_finish ((OstreeFetcher*)object, result, error);
  if (!fetch_data->temp_path)
    goto out;

  ostree_object_name_deserialize (fetch_data->object, &checksum, &objtype);

  g_debug ("fetch of %s complete", ostree_object_to_string (checksum, objtype));

  if (!ot_util_variant_map (fetch_data->temp_path, ostree_metadata_variant_type (objtype),
                            FALSE, &metadata, error))
    goto out;

  ostree_repo_stage_metadata_async (pull_data->repo, objtype, checksum, metadata,
                                    pull_data->cancellable,
                                    on_metadata_staged, fetch_data);

  pull_data->n_outstanding_metadata_stage_requests++;
 out:
  pull_data->n_outstanding_metadata_fetches--;
  pull_data->n_fetched_metadata++;
  throw_async_error (pull_data, local_error);
  if (local_error)
    {
      g_variant_unref (fetch_data->object);
      g_free (fetch_data);
    }
}

static gboolean
scan_commit_object (OtPullData         *pull_data,
                    const char         *checksum,
                    guint               recursion_depth,
                    GCancellable       *cancellable,
                    GError            **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *commit = NULL;
  gs_unref_variant GVariant *related_objects = NULL;
  gs_unref_variant GVariant *tree_contents_csum = NULL;
  gs_unref_variant GVariant *tree_meta_csum = NULL;
  gs_free char *tmp_checksum = NULL;
  GVariantIter *iter = NULL;

  if (recursion_depth > OSTREE_MAX_RECURSION)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exceeded maximum recursion");
      goto out;
    }

  if (!ostree_repo_load_variant (pull_data->repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                 &commit, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  g_variant_get_child (commit, 6, "@ay", &tree_contents_csum);
  g_variant_get_child (commit, 7, "@ay", &tree_meta_csum);

  if (!scan_one_metadata_object (pull_data, ostree_checksum_bytes_peek (tree_contents_csum),
                                 OSTREE_OBJECT_TYPE_DIR_TREE, recursion_depth + 1,
                                 cancellable, error))
    goto out;

  if (!scan_one_metadata_object (pull_data, ostree_checksum_bytes_peek (tree_meta_csum),
                                 OSTREE_OBJECT_TYPE_DIR_META, recursion_depth + 1,
                                 cancellable, error))
    goto out;
  
  if (pull_data->flags & OSTREE_REPO_PULL_FLAGS_RELATED)
    {
      const char *name;
      gs_unref_variant GVariant *csum_v = NULL;

      related_objects = g_variant_get_child_value (commit, 2);
      iter = g_variant_iter_new (related_objects);

      while (g_variant_iter_loop (iter, "(&s@ay)", &name, &csum_v))
        {
          if (!scan_one_metadata_object (pull_data, ostree_checksum_bytes_peek (csum_v),
                                         OSTREE_OBJECT_TYPE_COMMIT, recursion_depth + 1,
                                         cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  if (iter)
    g_variant_iter_free (iter);
  return ret;
}

static gboolean
scan_one_metadata_object (OtPullData         *pull_data,
                          const guchar       *csum,
                          OstreeObjectType    objtype,
                          guint               recursion_depth,
                          GCancellable       *cancellable,
                          GError            **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *object = NULL;
  gs_free char *tmp_checksum = NULL;
  gboolean is_requested;
  gboolean is_stored;

  tmp_checksum = ostree_checksum_from_bytes (csum);
  object = ostree_object_name_serialize (tmp_checksum, objtype);

  if (g_hash_table_lookup (pull_data->scanned_metadata, object))
    return TRUE;

  is_requested = g_hash_table_lookup (pull_data->requested_metadata, tmp_checksum) != NULL;
  if (!ostree_repo_has_object (pull_data->repo, objtype, tmp_checksum, &is_stored,
                               cancellable, error))
    goto out;

  if (!is_stored && !is_requested)
    {
      char *duped_checksum = g_strdup (tmp_checksum);
      g_hash_table_insert (pull_data->requested_metadata, duped_checksum, duped_checksum);
      
      ot_waitable_queue_push (pull_data->metadata_objects_to_fetch,
                              pull_worker_message_new (PULL_MSG_FETCH,
                                                       g_variant_ref (object)));
    }
  else if (is_stored)
    {
      if (pull_data->transaction_resuming || is_requested)
        {
          switch (objtype)
            {
            case OSTREE_OBJECT_TYPE_COMMIT:
              if (!scan_commit_object (pull_data, tmp_checksum, recursion_depth,
                                       pull_data->cancellable, error))
                goto out;
              break;
            case OSTREE_OBJECT_TYPE_DIR_META:
              break;
            case OSTREE_OBJECT_TYPE_DIR_TREE:
              if (!scan_dirtree_object (pull_data, tmp_checksum, recursion_depth,
                                        pull_data->cancellable, error))
                goto out;
              break;
            case OSTREE_OBJECT_TYPE_FILE:
              g_assert_not_reached ();
              break;
            }
        }
      g_hash_table_insert (pull_data->scanned_metadata, g_variant_ref (object), object);
      g_atomic_int_inc (&pull_data->n_scanned_metadata);
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
scan_one_metadata_object_v_name (OtPullData         *pull_data,
                                 GVariant           *object,
                                 GCancellable       *cancellable,
                                 GError            **error)
{
  OstreeObjectType objtype;
  const char *checksum = NULL;
  gs_free guchar *csum = NULL;

  ostree_object_name_deserialize (object, &checksum, &objtype);
  csum = ostree_checksum_to_bytes (checksum);

  return scan_one_metadata_object (pull_data, csum, objtype, 0,
                                   cancellable, error);
}

typedef struct {
  OtPullData *pull_data;
  GError *error;
} IdleThrowErrorData;

static gboolean
idle_throw_error (gpointer user_data)
{
  IdleThrowErrorData *data = user_data;
  
  throw_async_error (data->pull_data, data->error);

  g_free (data);
  return FALSE;
}

static gboolean
on_metadata_objects_to_scan_ready (gint         fd,
                                   GIOCondition condition,
                                   gpointer     user_data)
{
  OtPullData *pull_data = user_data;
  PullWorkerMessage *msg;
  PullWorkerMessage *last_idle_msg = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;

  while (ot_waitable_queue_pop (pull_data->metadata_objects_to_scan, (gpointer*)&msg))
    {
      if (msg->t == PULL_MSG_SCAN)
        {
          if (!scan_one_metadata_object_v_name (pull_data, msg->d.item,
                                                pull_data->cancellable, error))
            goto out;
          g_variant_unref (msg->d.item);
          g_free (msg);
        }
      else if (msg->t == PULL_MSG_MAIN_IDLE)
        {
          g_free (last_idle_msg);
          last_idle_msg = msg;
        }
      else if (msg->t == PULL_MSG_QUIT)
        {
          g_free (msg);
          g_main_loop_quit (pull_data->metadata_thread_loop);
        }
      else
        g_assert_not_reached ();
      }
    
  if (last_idle_msg)
    ot_waitable_queue_push (pull_data->metadata_objects_to_fetch,
                            last_idle_msg);
  
  /* When we have no queue to process, notify the main thread */
  ot_waitable_queue_push (pull_data->metadata_objects_to_fetch,
                          pull_worker_message_new (PULL_MSG_SCAN_IDLE, GUINT_TO_POINTER (0)));

 out:
  if (local_error)
    {
      IdleThrowErrorData *throwdata = g_new0 (IdleThrowErrorData, 1);
      throwdata->pull_data = pull_data;
      throwdata->error = local_error;
      g_main_context_invoke (NULL, idle_throw_error, throwdata);
    }
  return TRUE;
}

/**
 * metadata_thread_main:
 *
 * Called from the metadatascan worker thread. If we're missing an
 * object from one of them, we queue a request to the main thread to
 * fetch it.  When it's fetched, we get passed the object back and
 * scan it.
 */
static gpointer
metadata_thread_main (gpointer user_data)
{
  OtPullData *pull_data = user_data;
  GSource *src;

  pull_data->metadata_thread_context = g_main_context_new ();
  pull_data->metadata_thread_loop = g_main_loop_new (pull_data->metadata_thread_context, TRUE);

  src = ot_waitable_queue_create_source (pull_data->metadata_objects_to_scan);
  g_source_set_callback (src, (GSourceFunc)on_metadata_objects_to_scan_ready, pull_data, NULL);
  g_source_attach (src, pull_data->metadata_thread_context);
  g_source_unref (src);

  g_main_loop_run (pull_data->metadata_thread_loop);
  return NULL;
}

static gboolean
on_metadata_objects_to_fetch_ready (gint         fd,
                                    GIOCondition condition,
                                    gpointer     user_data)
{
  OtPullData *pull_data = user_data;
  PullWorkerMessage *msg;

  if (!ot_waitable_queue_pop (pull_data->metadata_objects_to_fetch, (gpointer*)&msg))
    goto out;

  if (msg->t == PULL_MSG_MAIN_IDLE)
    {
      if (msg->d.idle_serial == pull_data->idle_serial)
        {
          g_assert (!pull_data->metadata_scan_idle);
          pull_data->metadata_scan_idle = TRUE;
          g_debug ("pull: metadata scan is idle");
        }
    }
  else if (msg->t == PULL_MSG_SCAN_IDLE)
    {
      if (!pull_data->metadata_scan_idle)
        {
          g_debug ("pull: queue MAIN_IDLE");
          pull_data->idle_serial++;
          ot_waitable_queue_push (pull_data->metadata_objects_to_scan,
                                  pull_worker_message_new (PULL_MSG_MAIN_IDLE, GUINT_TO_POINTER (pull_data->idle_serial)));
        }
    }
  else if (msg->t == PULL_MSG_FETCH)
    {
      const char *checksum;
      gs_free char *objpath = NULL;
      OstreeObjectType objtype;
      SoupURI *obj_uri = NULL;
      gboolean is_meta;
      FetchObjectData *fetch_data;
      
      ostree_object_name_deserialize (msg->d.item, &checksum, &objtype);
      objpath = ostree_get_relative_object_path (checksum, objtype, TRUE);
      obj_uri = suburi_new (pull_data->base_uri, objpath, NULL);

      is_meta = OSTREE_OBJECT_TYPE_IS_META (objtype);
      if (is_meta)
        {
          pull_data->n_outstanding_metadata_fetches++;
          pull_data->n_requested_metadata++;
        }
      else
        {
          pull_data->n_outstanding_content_fetches++;
          pull_data->n_requested_content++;
        }
      fetch_data = g_new (FetchObjectData, 1);
      fetch_data->pull_data = pull_data;
      fetch_data->object = g_variant_ref (msg->d.item);
      ostree_fetcher_request_uri_async (pull_data->fetcher, obj_uri, pull_data->cancellable,
                                        is_meta ? meta_fetch_on_complete : content_fetch_on_complete, fetch_data);
      soup_uri_free (obj_uri);
      g_variant_unref (msg->d.item);
    }
  else
    {
      g_assert_not_reached ();
    }
  g_free (msg);

 out:
  check_outstanding_requests_handle_error (pull_data, NULL);
  
  return TRUE;
}

static gboolean
parse_ref_summary (const char    *contents,
                   GHashTable   **out_refs,
                   GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_hashtable GHashTable *ret_refs = NULL;
  char **lines = NULL;
  char **iter = NULL;
  char *ref = NULL;
  char *sha256 = NULL;

  ret_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  lines = g_strsplit_set (contents, "\n", -1);
  for (iter = lines; *iter; iter++)
    {
      const char *line = *iter;
      const char *spc;

      if (!*line)
        continue;

      spc = strchr (line, ' ');
      if (!spc)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid ref summary file; missing ' ' in line");
          goto out;
        }

      g_free (ref);
      ref = g_strdup (spc + 1);
      if (!ostree_validate_rev (ref, error))
        goto out;
      
      g_free (sha256);
      sha256 = g_strndup (line, spc - line);
      if (!ostree_validate_checksum_string (sha256, error))
        goto out;

      g_hash_table_replace (ret_refs, ref, sha256);
      /* Transfer ownership */
      ref = NULL;
      sha256 = NULL;
    }

  ret = TRUE;
  ot_transfer_out_value (out_refs, &ret_refs);
 out:
  g_strfreev (lines);
  return ret;
}

static gboolean
repo_get_string_key_inherit (OstreeRepo          *repo,
                             const char          *section,
                             const char          *key,
                             char               **out_value,
                             GError             **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  GKeyFile *config;
  gs_free char *ret_value = NULL;

  config = ostree_repo_get_config (repo);

  ret_value = g_key_file_get_value (config, section, key, &temp_error);
  if (temp_error)
    {
      OstreeRepo *parent = ostree_repo_get_parent (repo);
      if (parent &&
          (g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND)
           || g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND)))
        {
          g_clear_error (&temp_error);
          if (!repo_get_string_key_inherit (parent, section, key, &ret_value, error))
            goto out;
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_value, &ret_value);
 out:
  return ret;
}

static gboolean
load_remote_repo_config (OtPullData    *pull_data,
                         GKeyFile     **out_keyfile,
                         GCancellable  *cancellable,
                         GError       **error)
{
  gboolean ret = FALSE;
  gs_free char *contents = NULL;
  GKeyFile *ret_keyfile = NULL;
  SoupURI *target_uri = NULL;

  target_uri = suburi_new (pull_data->base_uri, "config", NULL);
  
  if (!fetch_uri_contents_utf8 (pull_data, target_uri, &contents,
                                cancellable, error))
    goto out;

  ret_keyfile = g_key_file_new ();
  if (!g_key_file_load_from_data (ret_keyfile, contents, strlen (contents),
                                  0, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_keyfile, &ret_keyfile);
 out:
  g_clear_pointer (&ret_keyfile, (GDestroyNotify) g_key_file_unref);
  g_clear_pointer (&target_uri, (GDestroyNotify) soup_uri_free);
  return ret;
}

gboolean
ostree_repo_pull (OstreeRepo               *repo,
                  const char               *remote_name,
                  char                    **refs_to_fetch,
                  OstreeRepoPullFlags       flags,
                  GCancellable             *cancellable,
                  GError                  **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  gboolean tls_permissive = FALSE;
  OstreeFetcherConfigFlags fetcher_flags = 0;
  gs_free char *remote_key = NULL;
  gs_free char *remote_config_content = NULL;
  gs_free char *path = NULL;
  gs_free char *baseurl = NULL;
  gs_free char *summary_data = NULL;
  gs_unref_hashtable GHashTable *requested_refs_to_fetch = NULL;
  gs_unref_hashtable GHashTable *updated_refs = NULL;
  gs_unref_hashtable GHashTable *commits_to_fetch = NULL;
  gs_free char *branch_rev = NULL;
  gs_free char *remote_mode_str = NULL;
  GSource *queue_src = NULL;
  OtPullData pull_data_real;
  OtPullData *pull_data = &pull_data_real;
  SoupURI *summary_uri = NULL;
  GKeyFile *config = NULL;
  GKeyFile *remote_config = NULL;
  char **configured_branches = NULL;
  guint64 bytes_transferred;
  guint64 start_time;
  guint64 end_time;

  memset (pull_data, 0, sizeof (*pull_data));

  pull_data->async_error = error;
  pull_data->main_context = g_main_context_get_thread_default ();
  pull_data->loop = g_main_loop_new (pull_data->main_context, FALSE);
  pull_data->flags = flags;

  pull_data->repo = repo;

  pull_data->scanned_metadata = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                                       (GDestroyNotify)g_variant_unref, NULL);
  pull_data->requested_content = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                        (GDestroyNotify)g_free, NULL);
  pull_data->requested_metadata = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                         (GDestroyNotify)g_free, NULL);

  start_time = g_get_monotonic_time ();

  pull_data->remote_name = g_strdup (remote_name);
  config = ostree_repo_get_config (repo);

  remote_key = g_strdup_printf ("remote \"%s\"", pull_data->remote_name);
  if (!repo_get_string_key_inherit (repo, remote_key, "url", &baseurl, error))
    goto out;
  pull_data->base_uri = soup_uri_new (baseurl);

  if (!ot_keyfile_get_boolean_with_default (config, remote_key, "tls-permissive",
                                            FALSE, &tls_permissive, error))
    goto out;
  if (tls_permissive)
    fetcher_flags |= OSTREE_FETCHER_FLAGS_TLS_PERMISSIVE;

  pull_data->fetcher = ostree_fetcher_new (ostree_repo_get_tmpdir (pull_data->repo),
                                           fetcher_flags);

  if (!pull_data->base_uri)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to parse url '%s'", baseurl);
      goto out;
    }

  if (!load_remote_repo_config (pull_data, &remote_config, cancellable, error))
    goto out;

  if (!ot_keyfile_get_value_with_default (remote_config, "core", "mode", "bare",
                                          &remote_mode_str, error))
    goto out;

  if (!ostree_repo_mode_from_string (remote_mode_str, &pull_data->remote_mode, error))
    goto out;

  if (pull_data->remote_mode != OSTREE_REPO_MODE_ARCHIVE_Z2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't pull from archives with mode \"%s\"",
                   remote_mode_str);
      goto out;
    }

  requested_refs_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  updated_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  commits_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  if (refs_to_fetch != NULL)
    {
      char **strviter;
      for (strviter = refs_to_fetch; *strviter; strviter++)
        {
          const char *branch = *strviter;
          char *contents;

          if (ostree_validate_checksum_string (branch, NULL))
            {
              char *key = g_strdup (branch);
              g_hash_table_insert (commits_to_fetch, key, key);
            }
          else
            {
              if (!fetch_ref_contents (pull_data, branch, &contents, cancellable, error))
                goto out;
      
              /* Transfer ownership of contents */
              g_hash_table_insert (requested_refs_to_fetch, g_strdup (branch), contents);
            }
        }
    }
  else
    {
      GError *temp_error = NULL;
      gboolean fetch_all_refs;

      configured_branches = g_key_file_get_string_list (config, remote_key, "branches", NULL, &temp_error);
      if (configured_branches == NULL && temp_error != NULL)
        {
          if (g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
            {
              g_clear_error (&temp_error);
              fetch_all_refs = TRUE;
            }
          else
            {
              g_propagate_error (error, temp_error);
              goto out;
            }
        }
      else
        fetch_all_refs = FALSE;

      if (fetch_all_refs)
        {
          summary_uri = soup_uri_copy (pull_data->base_uri);
          path = g_build_filename (soup_uri_get_path (summary_uri), "refs", "summary", NULL);
          soup_uri_set_path (summary_uri, path);
          
          if (!fetch_uri_contents_utf8 (pull_data, summary_uri, &summary_data, cancellable, error))
            goto out;
          
          if (!parse_ref_summary (summary_data, &requested_refs_to_fetch, error))
            goto out;
        }
      else
        {
          char **branches_iter = configured_branches;

          if (!(branches_iter && *branches_iter))
            g_print ("No configured branches for remote %s\n", pull_data->remote_name);
          for (;branches_iter && *branches_iter; branches_iter++)
            {
              const char *branch = *branches_iter;
              char *contents;
              
              if (!fetch_ref_contents (pull_data, branch, &contents, cancellable, error))
                goto out;
              
              /* Transfer ownership of contents */
              g_hash_table_insert (requested_refs_to_fetch, g_strdup (branch), contents);
            }
        }
    }

  if (!ostree_repo_prepare_transaction (pull_data->repo, FALSE, &pull_data->transaction_resuming,
                                        cancellable, error))
    goto out;

  pull_data->metadata_objects_to_fetch = ot_waitable_queue_new ();
  pull_data->metadata_objects_to_scan = ot_waitable_queue_new ();
  pull_data->metadata_thread = g_thread_new ("metadatascan", metadata_thread_main, pull_data);

  g_hash_table_iter_init (&hash_iter, commits_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *commit = value;

      ot_waitable_queue_push (pull_data->metadata_objects_to_scan,
                              pull_worker_message_new (PULL_MSG_SCAN,
                                                       ostree_object_name_serialize (commit, OSTREE_OBJECT_TYPE_COMMIT)));
    }

  g_hash_table_iter_init (&hash_iter, requested_refs_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *ref = key;
      const char *sha256 = value;
      gs_free char *key = NULL;
      gs_free char *remote_ref = NULL;
      gs_free char *baseurl = NULL;
      gs_free char *original_rev = NULL;

      remote_ref = g_strdup_printf ("%s/%s", pull_data->remote_name, ref);

      if (!ostree_repo_resolve_rev (pull_data->repo, remote_ref, TRUE, &original_rev, error))
        goto out;

      if (original_rev && strcmp (sha256, original_rev) == 0)
        {
          g_print ("No changes in %s\n", remote_ref);
        }
      else
        {
          ot_waitable_queue_push (pull_data->metadata_objects_to_scan,
                                  pull_worker_message_new (PULL_MSG_SCAN,
                                                           ostree_object_name_serialize (sha256, OSTREE_OBJECT_TYPE_COMMIT)));
          g_hash_table_insert (updated_refs, g_strdup (ref), g_strdup (sha256));
        }
    }
  
  {
    queue_src = ot_waitable_queue_create_source (pull_data->metadata_objects_to_fetch);
    g_source_set_callback (queue_src, (GSourceFunc)on_metadata_objects_to_fetch_ready, pull_data, NULL);
    g_source_attach (queue_src, pull_data->main_context);
    g_source_unref (queue_src);
  }

  /* Prime the message queue */
  pull_data->idle_serial++;
  ot_waitable_queue_push (pull_data->metadata_objects_to_scan,
                          pull_worker_message_new (PULL_MSG_MAIN_IDLE, GUINT_TO_POINTER (pull_data->idle_serial)));
  
  /* Now await work completion */
  if (!run_mainloop_monitor_fetcher (pull_data))
    goto out;
  
  if (!ostree_repo_commit_transaction (pull_data->repo, cancellable, error))
    goto out;

  g_hash_table_iter_init (&hash_iter, updated_refs);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *ref = key;
      const char *checksum = value;
      gs_free char *remote_ref = NULL;
          
      remote_ref = g_strdup_printf ("%s/%s", pull_data->remote_name, ref);
          
      if (!ostree_repo_write_ref (pull_data->repo, pull_data->remote_name, ref, checksum, error))
        goto out;
          
      g_print ("remote %s is now %s\n", remote_ref, checksum);
    }
      
  end_time = g_get_monotonic_time ();

  bytes_transferred = ostree_fetcher_bytes_transferred (pull_data->fetcher);
  if (bytes_transferred > 0)
    {
      guint shift; 
      if (bytes_transferred < 1024)
        shift = 1;
      else
        shift = 1024;
      g_print ("%u metadata, %u content objects fetched; %" G_GUINT64_FORMAT " %s transferred in %u seconds\n", 
               pull_data->n_fetched_metadata, pull_data->n_fetched_content,
               (guint64)(bytes_transferred / shift),
               shift == 1 ? "B" : "KiB",
               (guint) ((end_time - start_time) / G_USEC_PER_SEC));
    }

  ret = TRUE;
 out:
  if (pull_data->main_context)
    g_main_context_unref (pull_data->main_context);
  if (pull_data->loop)
    g_main_loop_unref (pull_data->loop);
  g_strfreev (configured_branches);
  g_clear_object (&pull_data->fetcher);
  g_free (pull_data->remote_name);
  if (pull_data->base_uri)
    soup_uri_free (pull_data->base_uri);
  if (queue_src)
    g_source_destroy (queue_src);
  if (pull_data->metadata_thread)
    {
      ot_waitable_queue_push (pull_data->metadata_objects_to_scan,
                              pull_worker_message_new (PULL_MSG_QUIT, NULL));
      g_thread_join (pull_data->metadata_thread);
    }
  g_clear_pointer (&pull_data->metadata_objects_to_scan, (GDestroyNotify) ot_waitable_queue_unref);
  g_clear_pointer (&pull_data->metadata_objects_to_fetch, (GDestroyNotify) ot_waitable_queue_unref);
  g_clear_pointer (&pull_data->scanned_metadata, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_content, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_metadata, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&remote_config, (GDestroyNotify) g_key_file_unref);
  if (summary_uri)
    soup_uri_free (summary_uri);
  return ret;
}
