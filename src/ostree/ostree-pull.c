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
#include "ot-main.h"

#include "ostree-fetcher.h"


gboolean verbose;
gboolean opt_related;

static GOptionEntry options[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Show more information", NULL },
  { "related", 0, 0, G_OPTION_ARG_NONE, &opt_related, "Download related commits", NULL },
  { NULL },
};

typedef struct {
  OstreeRepo   *repo;
  char         *remote_name;
  OstreeRepoMode remote_mode;
  OstreeFetcher *fetcher;
  SoupURI      *base_uri;

  GMainLoop    *loop;
  GCancellable *cancellable;

  gboolean      metadata_scan_active;
  volatile gint n_scanned_metadata;
  volatile gint n_requested_metadata;
  volatile gint n_requested_content;
  guint         n_fetched_metadata;
  guint         outstanding_uri_requests;

  GQueue        queued_filemeta;
  GThread      *metadata_scan_thread;
  OtWorkerQueue  *metadata_objects_to_scan;
  GHashTable   *scanned_metadata; /* Maps object name to itself */
  GHashTable   *requested_content; /* Maps object name to itself */
  
  guint         n_fetched_content;
  guint         outstanding_filemeta_requests;
  guint         outstanding_filecontent_requests;
  guint         outstanding_content_stage_requests;

  guint64       previous_total_downloaded;

  GError      **async_error;
  gboolean      caught_error;

  gboolean      stdout_is_tty;
  guint         last_padding;
} OtPullData;

typedef struct {
  OtPullData *pull_data;

  gboolean fetching_content;

  GFile *meta_path;
  GFile *content_path;

  char *checksum;
} OtFetchOneContentItemData;

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
  ot_lfree char *fetcher_status;
  GString *status;
  guint64 current_bytes_transferred;
  guint64 delta_bytes_transferred;
 
  status = g_string_new ("");

  if (pull_data->metadata_scan_active)
    g_string_append_printf (status, "scan: %u metadata; ",
                            g_atomic_int_get (&pull_data->n_scanned_metadata));

  g_string_append_printf (status, "fetch: %u/%u metadata %u/%u content; ",
                          g_atomic_int_get (&pull_data->n_fetched_metadata),
                          g_atomic_int_get (&pull_data->n_requested_metadata),
                          pull_data->n_fetched_content,
                          g_atomic_int_get (&pull_data->n_requested_content));

  current_bytes_transferred = ostree_fetcher_bytes_transferred (pull_data->fetcher);
  delta_bytes_transferred = current_bytes_transferred - pull_data->previous_total_downloaded;
  pull_data->previous_total_downloaded = current_bytes_transferred;

  if (delta_bytes_transferred < 1024)
    g_string_append_printf (status, "%u B/s; ", 
                            (guint)delta_bytes_transferred);
  else
    g_string_append_printf (status, "%.1f KiB/s; ", 
                            (double)delta_bytes_transferred / 1024);

  fetcher_status = ostree_fetcher_query_state_text (pull_data->fetcher);
  g_string_append (status, fetcher_status);
  if (status->len > pull_data->last_padding)
    pull_data->last_padding = status->len;
  else
    {
      guint diff = pull_data->last_padding - status->len;
      while (diff > 0)
        {
          g_string_append_c (status, ' ');
          diff--;
        }
    }
  g_print ("%c8%s", 0x1B, status->str);

  g_string_free (status, TRUE);

  return TRUE;
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
  if (!pull_data->metadata_scan_active &&
      pull_data->outstanding_uri_requests == 0 &&
      pull_data->outstanding_filemeta_requests == 0 &&
      pull_data->outstanding_filecontent_requests == 0 &&
      pull_data->outstanding_content_stage_requests == 0)
    g_main_loop_quit (pull_data->loop);
  throw_async_error (pull_data, error);
}

static gboolean
run_mainloop_monitor_fetcher (OtPullData   *pull_data)
{
  GSource *update_timeout = NULL;

  if (pull_data->stdout_is_tty)
    {
      g_print ("%c7", 0x1B);
      update_timeout = g_timeout_source_new_seconds (1);
      g_source_set_callback (update_timeout, uri_fetch_update_status, pull_data, NULL);
      g_source_attach (update_timeout, g_main_loop_get_context (pull_data->loop));
      g_source_unref (update_timeout);
    }
  
  g_main_loop_run (pull_data->loop);

  if (pull_data->stdout_is_tty)
    {
      g_print ("\n");
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
  ot_lfree char *uri_string = NULL;
  ot_lobj SoupRequest *request = NULL;
  OstreeFetchUriData fetch_data;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  memset (&fetch_data, 0, sizeof (fetch_data));
  fetch_data.pull_data = pull_data;

  uri_string = soup_uri_to_string (uri, FALSE);
  g_print ("Fetching %s\n", uri_string);

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
  ot_lobj GFile *tmpf = NULL;
  ot_lfree char *ret_contents = NULL;
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
    (void) unlink (ot_gfile_get_path_cached (tmpf));
  return ret;
}

static gboolean
idle_queue_content_request (gpointer user_data);

static gboolean
scan_dirtree_object (OtPullData   *pull_data,
                     const char   *checksum,
                     int           recursion_depth,
                     GCancellable *cancellable,
                     GError      **error)
{
  gboolean ret = FALSE;
  int i, n;
  gboolean compressed = pull_data->remote_mode == OSTREE_REPO_MODE_ARCHIVE_Z;
  ot_lvariant GVariant *tree = NULL;
  ot_lvariant GVariant *files_variant = NULL;
  ot_lvariant GVariant *dirs_variant = NULL;
  ot_lobj GFile *stored_path = NULL;

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
      OtFetchOneContentItemData *idle_fetch_data;
      ot_lvariant GVariant *csum = NULL;
      ot_lfree char *file_checksum;

      g_variant_get_child (files_variant, i, "(&s@ay)", &filename, &csum);

      if (!ot_util_filename_validate (filename, error))
        goto out;

      file_checksum = ostree_checksum_from_bytes_v (csum);

      if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_FILE, file_checksum,
                                   &file_is_stored, cancellable, error))
        goto out;

      if (!file_is_stored && !g_hash_table_lookup (pull_data->requested_content, file_checksum))
        {
          char *duped_checksum;

          idle_fetch_data = g_new0 (OtFetchOneContentItemData, 1);
          idle_fetch_data->pull_data = pull_data;
          idle_fetch_data->checksum = file_checksum;
          idle_fetch_data->fetching_content = compressed;
          file_checksum = NULL; /* Transfer ownership */

          duped_checksum = g_strdup (idle_fetch_data->checksum);
          g_hash_table_insert (pull_data->requested_content, duped_checksum, duped_checksum);

          g_atomic_int_inc (&pull_data->n_requested_content);
          ot_worker_queue_hold (pull_data->metadata_objects_to_scan);
          g_main_context_invoke (NULL, idle_queue_content_request, idle_fetch_data);
        }
    }
      
  n = g_variant_n_children (dirs_variant);
  for (i = 0; i < n; i++)
    {
      const char *dirname;
      ot_lvariant GVariant *tree_csum = NULL;
      ot_lvariant GVariant *meta_csum = NULL;
      ot_lfree char *tmp_checksum = NULL;

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
  ot_lfree char *ret_contents = NULL;
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
destroy_fetch_one_content_item_data (OtFetchOneContentItemData *data)
{
  if (data->meta_path)
    (void) ot_gfile_unlink (data->meta_path, NULL, NULL);
  g_clear_object (&data->meta_path);
  if (data->content_path)
    (void) ot_gfile_unlink (data->content_path, NULL, NULL);
  g_clear_object (&data->content_path);
  g_free (data->checksum);
  g_free (data);
}

static void
content_fetch_on_stage_complete (GObject        *object,
                                 GAsyncResult   *result,
                                 gpointer        user_data)
{
  OtFetchOneContentItemData *data = user_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  ot_lfree guchar *csum = NULL;
  ot_lfree char *checksum = NULL;

  if (!ostree_repo_stage_content_finish ((OstreeRepo*)object, result, 
                                         &csum, error))
    goto out;

  checksum = ostree_checksum_from_bytes (csum);

  g_assert (strcmp (checksum, data->checksum) == 0);

  data->pull_data->n_fetched_content++;
 out:
  data->pull_data->outstanding_content_stage_requests--;
  check_outstanding_requests_handle_error (data->pull_data, local_error);
  destroy_fetch_one_content_item_data (data);
}

static void
content_fetch_on_complete (GObject        *object,
                           GAsyncResult   *result,
                           gpointer        user_data);

static void
process_one_file_request (OtFetchOneContentItemData *data)
{
  OtPullData *pull_data = data->pull_data;
  const char *checksum = data->checksum;
  gboolean compressed = pull_data->remote_mode == OSTREE_REPO_MODE_ARCHIVE_Z;
  ot_lfree char *objpath = NULL;
  SoupURI *obj_uri = NULL;

  objpath = ostree_get_relative_object_path (checksum, OSTREE_OBJECT_TYPE_FILE, compressed);
  obj_uri = suburi_new (pull_data->base_uri, objpath, NULL);
      
  ostree_fetcher_request_uri_async (pull_data->fetcher, obj_uri, pull_data->cancellable,
                                    content_fetch_on_complete, data);
  soup_uri_free (obj_uri);
  
  if (compressed)
    pull_data->outstanding_filecontent_requests++;
  else
    pull_data->outstanding_filemeta_requests++;
}

static void
content_fetch_on_complete (GObject        *object,
                           GAsyncResult   *result,
                           gpointer        user_data) 
{
  OtFetchOneContentItemData *data = user_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  gboolean compressed;
  GCancellable *cancellable = NULL;
  gboolean was_content_fetch = FALSE;
  gboolean need_content_fetch = FALSE;
  guint64 length;
  ot_lvariant GVariant *file_meta = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GInputStream *content_input = NULL;
  ot_lobj GInputStream *file_object_input = NULL;
  ot_lvariant GVariant *xattrs = NULL;

  compressed = data->pull_data->remote_mode == OSTREE_REPO_MODE_ARCHIVE_Z;
  was_content_fetch = data->fetching_content;

  if (was_content_fetch)
    {
      data->content_path = ostree_fetcher_request_uri_finish ((OstreeFetcher*)object, result, error);
      if (!data->content_path)
        goto out;
    }
  else
    {
      data->meta_path = ostree_fetcher_request_uri_finish ((OstreeFetcher*)object, result, error);
      if (!data->meta_path)
        goto out;
    }

  if (!was_content_fetch)
    {
      if (!ot_util_variant_map (data->meta_path, OSTREE_FILE_HEADER_GVARIANT_FORMAT, FALSE,
                                &file_meta, error))
        goto out;

      if (!ostree_file_header_parse (file_meta, &file_info, &xattrs, error))
        goto out;

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
        {
          ot_lfree char *content_path = ostree_get_relative_archive_content_path (data->checksum);
          SoupURI *content_uri;

          content_uri = suburi_new (data->pull_data->base_uri, content_path, NULL);

          data->pull_data->outstanding_filecontent_requests++;
          need_content_fetch = TRUE;
          data->fetching_content = TRUE;

          ostree_fetcher_request_uri_async (data->pull_data->fetcher, content_uri, cancellable,
                                            content_fetch_on_complete, data);
          soup_uri_free (content_uri);
        }
    }

  if (!need_content_fetch && compressed)
    {
      ot_lobj GInputStream *uncomp_input = NULL;

      g_assert (data->content_path != NULL);
      content_input = (GInputStream*)g_file_read (data->content_path, cancellable, error);
      if (!content_input)
        goto out;

      if (!ostree_zlib_content_stream_open (content_input, &length, &uncomp_input, 
                                            cancellable, error))
        goto out;

      data->pull_data->outstanding_content_stage_requests++;
      ostree_repo_stage_content_async (data->pull_data->repo, data->checksum,
                                       uncomp_input, length,
                                       cancellable,
                                       content_fetch_on_stage_complete, data);
    }
  else if (!need_content_fetch)
    {
      if (data->content_path)
        {
          content_input = (GInputStream*)g_file_read (data->content_path, cancellable, error);
          if (!content_input)
            goto out;
        }

      if (file_meta == NULL)
        {
          if (!ot_util_variant_map (data->meta_path, OSTREE_FILE_HEADER_GVARIANT_FORMAT, FALSE,
                                    &file_meta, error))
            goto out;

          if (!ostree_file_header_parse (file_meta, &file_info, &xattrs, error))
            goto out;
        }

      if (!ostree_raw_file_to_content_stream (content_input, file_info, xattrs,
                                              &file_object_input, &length,
                                              cancellable, error))
        goto out;

      data->pull_data->outstanding_content_stage_requests++;
      ostree_repo_stage_content_async (data->pull_data->repo, data->checksum,
                                       file_object_input, length,
                                       cancellable,
                                       content_fetch_on_stage_complete, data);
    }

  while (data->pull_data->outstanding_filemeta_requests < 10)
    {
      OtFetchOneContentItemData *queued_data = g_queue_pop_head (&data->pull_data->queued_filemeta);

      if (!queued_data)
        break;

      process_one_file_request (queued_data);
    }

 out:
  if (was_content_fetch)
    data->pull_data->outstanding_filecontent_requests--;
  else
    data->pull_data->outstanding_filemeta_requests--;
  check_outstanding_requests_handle_error (data->pull_data, local_error);
}

static gboolean
idle_queue_content_request (gpointer user_data)
{
  OtFetchOneContentItemData *data = user_data;
  OtPullData *pull_data = data->pull_data;
  
  /* Don't allow file meta requests to back up everything else */
  if (pull_data->outstanding_filemeta_requests > 10)
    {
      g_queue_push_tail (&pull_data->queued_filemeta, data);
    }
  else
    {
      process_one_file_request (data);
    }
      
  ot_worker_queue_release (pull_data->metadata_objects_to_scan);
  
  return FALSE;
}

typedef struct {
  OtPullData  *pull_data;
  GVariant *object;
  GFile *temp_path;
} IdleFetchMetadataObjectData;

static void
on_metadata_staged (GObject           *object,
                    GAsyncResult      *result,
                    gpointer           user_data)
{
  IdleFetchMetadataObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;

  pull_data->n_fetched_metadata++;

  ot_worker_queue_push (pull_data->metadata_objects_to_scan,
                        g_variant_ref (fetch_data->object));
  ot_worker_queue_release (pull_data->metadata_objects_to_scan);

  (void) ot_gfile_unlink (fetch_data->temp_path, NULL, NULL);
  g_object_unref (fetch_data->temp_path);
  g_variant_unref (fetch_data->object);
  g_free (fetch_data);
}

static void
meta_fetch_on_complete (GObject           *object,
                        GAsyncResult      *result,
                        gpointer           user_data)
{
  IdleFetchMetadataObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  ot_lvariant GVariant *metadata = NULL;
  const char *checksum;
  OstreeObjectType objtype;
  GError *local_error = NULL;
  GError **error = &local_error;

  fetch_data->temp_path = ostree_fetcher_request_uri_finish ((OstreeFetcher*)object, result, error);
  if (!fetch_data->temp_path)
    goto out;

  ostree_object_name_deserialize (fetch_data->object, &checksum, &objtype);

  if (!ot_util_variant_map (fetch_data->temp_path, ostree_metadata_variant_type (objtype),
                            FALSE, &metadata, error))
    goto out;

  ostree_repo_stage_metadata_async (pull_data->repo, objtype, checksum, metadata,
                                    pull_data->cancellable,
                                    on_metadata_staged, fetch_data);

 out:
  throw_async_error (pull_data, local_error);
  if (local_error)
    {
      g_variant_unref (fetch_data->object);
      g_free (fetch_data);
    }
}

static gboolean
idle_fetch_metadata_object (gpointer data)
{
  IdleFetchMetadataObjectData *fetch_data = data;
  OtPullData *pull_data = fetch_data->pull_data;
  ot_lfree char *objpath = NULL;
  const char *checksum;
  OstreeObjectType objtype;
  SoupURI *obj_uri = NULL;
  gboolean compressed;

  compressed = pull_data->remote_mode == OSTREE_REPO_MODE_ARCHIVE_Z;

  ostree_object_name_deserialize (fetch_data->object, &checksum, &objtype);

  objpath = ostree_get_relative_object_path (checksum, objtype, compressed);
  obj_uri = suburi_new (pull_data->base_uri, objpath, NULL);

  ostree_fetcher_request_uri_async (pull_data->fetcher, obj_uri, pull_data->cancellable,
                                    meta_fetch_on_complete, fetch_data);
  soup_uri_free (obj_uri);

  return FALSE;
}

/**
 * queue_metadata_object_fetch:
 *
 * Pass a request to the main thread to fetch a metadata object.
 */
static void
queue_metadata_object_fetch (OtPullData  *pull_data,
                             GVariant    *object)
{
  IdleFetchMetadataObjectData *fetch_data = g_new (IdleFetchMetadataObjectData, 1);
  fetch_data->pull_data = pull_data;
  fetch_data->object = g_variant_ref (object);
  ot_worker_queue_hold (fetch_data->pull_data->metadata_objects_to_scan);
  g_idle_add (idle_fetch_metadata_object, fetch_data);
}

static gboolean
scan_commit_object (OtPullData         *pull_data,
                    const char         *checksum,
                    guint               recursion_depth,
                    GCancellable       *cancellable,
                    GError            **error)
{
  gboolean ret = FALSE;
  ot_lvariant GVariant *commit = NULL;
  ot_lvariant GVariant *related_objects = NULL;
  ot_lvariant GVariant *tree_contents_csum = NULL;
  ot_lvariant GVariant *tree_meta_csum = NULL;
  ot_lfree char *tmp_checksum = NULL;
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
  
  if (opt_related)
    {
      const char *name;
      ot_lvariant GVariant *csum_v = NULL;

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
  ot_lvariant GVariant *object = NULL;
  ot_lfree char *tmp_checksum = NULL;
  gboolean is_stored;

  tmp_checksum = ostree_checksum_from_bytes (csum);
  object = ostree_object_name_serialize (tmp_checksum, objtype);

  if (g_hash_table_lookup (pull_data->scanned_metadata, object))
    return TRUE;

  if (!ostree_repo_has_object (pull_data->repo, objtype, tmp_checksum, &is_stored,
                               cancellable, error))
    goto out;
      
  if (!is_stored)
    {
      g_atomic_int_inc (&pull_data->n_requested_metadata);
      queue_metadata_object_fetch (pull_data, object);
    }
  else
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
  ot_lfree guchar *csum = NULL;

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

/**
 * scan_one_metadata_object_dispatch:
 *
 * Called from the metadatascan worker thread. If we're missing an
 * object from one of them, we queue a request to the main thread to
 * fetch it.  When it's fetched, we get passed the object back and
 * scan it.
 */
static void
scan_one_metadata_object_dispatch (gpointer item,
                                   gpointer user_data)
{
  OtPullData *pull_data = user_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  ot_lvariant GVariant *v_item = NULL;

  v_item = item;

  if (!scan_one_metadata_object_v_name (pull_data, v_item,
                                        pull_data->cancellable, error))
    goto out;

 out:
  if (local_error)
    {
      IdleThrowErrorData *throwdata = g_new0 (IdleThrowErrorData, 1);
      throwdata->pull_data = pull_data;
      throwdata->error = local_error;
      g_main_context_invoke (NULL, idle_throw_error, throwdata);
    }
}

static void
on_metadata_worker_idle (gpointer user_data)
{
  OtPullData *pull_data = user_data;

  pull_data->metadata_scan_active = FALSE;
  
  check_outstanding_requests_handle_error (pull_data, NULL);
}

static gboolean
idle_start_worker (gpointer user_data)
{
  OtPullData *pull_data = user_data;

  ot_worker_queue_start (pull_data->metadata_objects_to_scan);

  return FALSE;
}

static gboolean
parse_ref_summary (const char    *contents,
                   GHashTable   **out_refs,
                   GError       **error)
{
  gboolean ret = FALSE;
  ot_lhash GHashTable *ret_refs = NULL;
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
  ot_lfree char *ret_value = NULL;

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
  ot_lfree char *contents = NULL;
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

static gboolean
ostree_builtin_pull (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  int i;
  GCancellable *cancellable = NULL;
  ot_lfree char *remote_key = NULL;
  ot_lobj OstreeRepo *repo = NULL;
  ot_lfree char *remote_config_content = NULL;
  ot_lfree char *path = NULL;
  ot_lfree char *baseurl = NULL;
  ot_lfree char *summary_data = NULL;
  ot_lhash GHashTable *requested_refs_to_fetch = NULL;
  ot_lhash GHashTable *updated_refs = NULL;
  ot_lhash GHashTable *commits_to_fetch = NULL;
  ot_lfree char *branch_rev = NULL;
  ot_lfree char *remote_mode_str = NULL;
  OtPullData pull_data_real;
  OtPullData *pull_data = &pull_data_real;
  SoupURI *summary_uri = NULL;
  GKeyFile *config = NULL;
  GKeyFile *remote_config = NULL;
  char **configured_branches = NULL;
  guint64 bytes_transferred;

  memset (pull_data, 0, sizeof (*pull_data));

  context = g_option_context_new ("REMOTE [BRANCH...] - Download data from remote repository");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  pull_data->async_error = error;
  pull_data->loop = g_main_loop_new (NULL, FALSE);

  pull_data->repo = repo;

  pull_data->scanned_metadata = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                                       (GDestroyNotify)g_variant_unref, NULL);
  pull_data->requested_content = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                        (GDestroyNotify)g_free, NULL);

  if (argc < 2)
    {
      ot_util_usage_error (context, "REMOTE must be specified", error);
      goto out;
    }

  pull_data->stdout_is_tty = isatty (1);

  pull_data->remote_name = g_strdup (argv[1]);
  pull_data->fetcher = ostree_fetcher_new (ostree_repo_get_tmpdir (pull_data->repo));
  config = ostree_repo_get_config (repo);

  remote_key = g_strdup_printf ("remote \"%s\"", pull_data->remote_name);
  if (!repo_get_string_key_inherit (repo, remote_key, "url", &baseurl, error))
    goto out;
  pull_data->base_uri = soup_uri_new (baseurl);

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

  switch (pull_data->remote_mode)
    {
    case OSTREE_REPO_MODE_ARCHIVE:
    case OSTREE_REPO_MODE_ARCHIVE_Z:
      break;
    default:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't pull from archives with mode \"%s\"",
                   remote_mode_str);
    }

  requested_refs_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  updated_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  commits_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  if (argc > 2)
    {
      for (i = 2; i < argc; i++)
        {
          const char *branch = argv[i];
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

  if (!ostree_repo_prepare_transaction (pull_data->repo, NULL, error))
    goto out;

  pull_data->metadata_scan_active = TRUE;

  pull_data->metadata_objects_to_scan = ot_worker_queue_new ("metadatascan",
                                                             scan_one_metadata_object_dispatch,
                                                             pull_data);
  ot_worker_queue_set_idle_callback (pull_data->metadata_objects_to_scan,
                                     NULL, on_metadata_worker_idle, pull_data);

  g_hash_table_iter_init (&hash_iter, commits_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *commit = value;

      ot_worker_queue_push (pull_data->metadata_objects_to_scan,
                            ostree_object_name_serialize (commit, OSTREE_OBJECT_TYPE_COMMIT));
    }

  g_hash_table_iter_init (&hash_iter, requested_refs_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *ref = key;
      const char *sha256 = value;
      ot_lfree char *key = NULL;
      ot_lfree char *remote_ref = NULL;
      ot_lfree char *baseurl = NULL;
      ot_lfree char *original_rev = NULL;

      remote_ref = g_strdup_printf ("%s/%s", pull_data->remote_name, ref);

      if (!ostree_repo_resolve_rev (pull_data->repo, remote_ref, TRUE, &original_rev, error))
        goto out;

      if (original_rev && strcmp (sha256, original_rev) == 0)
        {
          g_print ("No changes in %s\n", remote_ref);
        }
      else
        {
          ot_worker_queue_push (pull_data->metadata_objects_to_scan,
                                ostree_object_name_serialize (sha256, OSTREE_OBJECT_TYPE_COMMIT));
          g_hash_table_insert (updated_refs, g_strdup (ref), g_strdup (sha256));
        }
    }

  g_idle_add (idle_start_worker, pull_data);

  /* Start metadata thread, which kicks off further metadata requests
   * as well as content fetches.
   */
  if (!run_mainloop_monitor_fetcher (pull_data))
    goto out;

  if (!ostree_repo_commit_transaction (pull_data->repo, cancellable, error))
    goto out;

  g_hash_table_iter_init (&hash_iter, updated_refs);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *ref = key;
      const char *checksum = value;
      ot_lfree char *remote_ref = NULL;

      remote_ref = g_strdup_printf ("%s/%s", pull_data->remote_name, ref);

      if (!ostree_repo_write_ref (pull_data->repo, pull_data->remote_name, ref, checksum, error))
        goto out;
      
      g_print ("remote %s is now %s\n", remote_ref, checksum);
    }

  bytes_transferred = ostree_fetcher_bytes_transferred (pull_data->fetcher);
  if (bytes_transferred > 0)
    {
      guint shift; 
      if (bytes_transferred < 1024)
        shift = 1;
      else
        shift = 1024;
      g_print ("%u metadata, %u content objects fetched; %" G_GUINT64_FORMAT " %s transferred\n", 
               pull_data->n_fetched_metadata, pull_data->n_fetched_content,
               (guint64)(bytes_transferred / shift),
               shift == 1 ? "B" : "KiB");
    }

  ret = TRUE;
 out:
  if (pull_data->loop)
    g_main_loop_unref (pull_data->loop);
  g_strfreev (configured_branches);
  if (context)
    g_option_context_free (context);
  g_clear_object (&pull_data->fetcher);
  g_free (pull_data->remote_name);
  if (pull_data->base_uri)
    soup_uri_free (pull_data->base_uri);
  g_clear_pointer (&pull_data->metadata_objects_to_scan, (GDestroyNotify) ot_worker_queue_unref);
  g_clear_pointer (&pull_data->scanned_metadata, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_content, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&remote_config, (GDestroyNotify) g_key_file_unref);
  if (summary_uri)
    soup_uri_free (summary_uri);
  return ret;
}

static OstreeCommand commands[] = {
  { "pull", ostree_builtin_pull, 0 },
  { NULL }
};

int
main (int    argc,
      char **argv)
{
  return ostree_main (argc, argv, commands);
}
