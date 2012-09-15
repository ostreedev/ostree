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
 *     Pull commit
 * 
 * Pull commits:
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
gint opt_packfile_threshold = 66;
gboolean opt_related;
gint opt_depth;

static GOptionEntry options[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Show more information", NULL },
  { "packfile-threshold", 't', 0, G_OPTION_ARG_INT, &opt_packfile_threshold, "Only download packfiles if more than PERCENT objects are needed (default: 66)", "PERCENT" },
  { "related", 0, 0, G_OPTION_ARG_NONE, &opt_related, "Download related commits", NULL },
  { "depth", 0, 0, G_OPTION_ARG_INT, &opt_depth, "Download parent commits up to this depth (default: 0)", NULL },
  { NULL },
};

typedef struct {
  OstreeRepo   *repo;
  char         *remote_name;
  OstreeFetcher *fetcher;
  SoupURI      *base_uri;

  gboolean      fetched_packs;
  GPtrArray    *cached_meta_pack_indexes;
  GPtrArray    *cached_data_pack_indexes;

  GHashTable   *file_checksums_to_fetch;

  GMainLoop    *loop;

  /* Used in meta fetch phase */
  guint         outstanding_uri_requests;
  guint         outstanding_meta_requests;

  /* Used in content fetch phase */
  guint         outstanding_filemeta_requests;
  guint         outstanding_filecontent_requests;
  guint         outstanding_checksum_requests;
  GHashTable   *loose_files;

  GError      **async_error;
  gboolean      caught_error;

  gboolean      stdout_is_tty;
  guint         last_padding;
} OtPullData;

static SoupURI *
suburi_new (SoupURI   *base,
            const char *first,
            ...) G_GNUC_NULL_TERMINATED;

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
 
  status = g_string_new ("");

  if (pull_data->loose_files != NULL)
    g_string_append_printf (status, "%u loose files to fetch: ",
                            g_hash_table_size (pull_data->loose_files)
                            + pull_data->outstanding_filemeta_requests
                            + pull_data->outstanding_filecontent_requests);

  if (pull_data->outstanding_checksum_requests > 0)
    g_string_append_printf (status, "Calculating %u checksums; ",
                            pull_data->outstanding_checksum_requests);

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
check_outstanding_requests_handle_error (OtPullData          *pull_data,
                                         GError              *error)
{
  if (pull_data->outstanding_uri_requests == 0 &&
      pull_data->outstanding_meta_requests == 0 &&
      pull_data->outstanding_filemeta_requests == 0 &&
      pull_data->outstanding_filecontent_requests == 0 &&
      pull_data->outstanding_checksum_requests == 0 &&
      (pull_data->loose_files == NULL || g_hash_table_size (pull_data->loose_files) == 0))
    g_main_loop_quit (pull_data->loop);
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

  run_mainloop_monitor_fetcher (pull_data);

  if (pull_data->caught_error)
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
fetch_one_pack_file (OtPullData            *pull_data,
                     const char            *pack_checksum,
                     gboolean               is_meta,
                     GFile                **out_cached_path,
                     GCancellable          *cancellable,
                     GError               **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *ret_cached_path = NULL;
  ot_lobj GFile *tmp_path = NULL;
  ot_lfree char *pack_name = NULL;
  SoupURI *pack_uri = NULL;

  if (!ostree_repo_get_cached_remote_pack_data (pull_data->repo, pull_data->remote_name,
                                                pack_checksum, is_meta, &ret_cached_path,
                                                cancellable, error))
    goto out;

  if (ret_cached_path == NULL)
    {
      pack_name = ostree_get_pack_data_name (is_meta, pack_checksum);
      pack_uri = suburi_new (pull_data->base_uri, "objects", "pack", pack_name, NULL);
      
      if (!fetch_uri (pull_data, pack_uri, "packdata-", &tmp_path, cancellable, error))
        goto out;

      if (!ostree_repo_take_cached_remote_pack_data (pull_data->repo, pull_data->remote_name,
                                                     pack_checksum, is_meta, tmp_path,
                                                     cancellable, error))
        goto out;
    }

  g_clear_object (&ret_cached_path);
  if (!ostree_repo_get_cached_remote_pack_data (pull_data->repo, pull_data->remote_name,
                                                pack_checksum, is_meta, &ret_cached_path,
                                                cancellable, error))
    goto out;

  g_assert (ret_cached_path != NULL);

  ret = TRUE;
  ot_transfer_out_value (out_cached_path, &ret_cached_path);
 out:
  if (pack_uri)
    soup_uri_free (pack_uri);
  return ret;
}

static gboolean
find_object_in_one_remote_pack (OtPullData       *pull_data,
                                GVariant         *csum_bytes_v,
                                OstreeObjectType  objtype,
                                const char       *pack_checksum,
                                gboolean         *out_exists,
                                guint64          *out_offset,
                                GCancellable     *cancellable,
                                GError          **error)
{
  gboolean ret = FALSE;
  guint64 ret_offset;
  gboolean ret_exists;
  ot_lvariant GVariant *mapped_pack = NULL;

  if (!ostree_repo_map_cached_remote_pack_index (pull_data->repo, pull_data->remote_name,
                                                 pack_checksum, OSTREE_OBJECT_TYPE_IS_META (objtype),
                                                 &mapped_pack,
                                                 cancellable, error))
    goto out;
  
  ret_exists = ostree_pack_index_search (mapped_pack, csum_bytes_v, objtype, &ret_offset);

  ret = TRUE;
  if (out_exists)
    *out_exists = ret_exists;
  if (out_offset)
    *out_offset = ret_offset;
 out:
  return ret;
}

static gboolean
find_object_in_remote_packs (OtPullData       *pull_data,
                             const char       *checksum,
                             OstreeObjectType  objtype,
                             char            **out_pack_checksum,
                             guint64          *out_offset,
                             GCancellable     *cancellable,
                             GError          **error)
{
  gboolean ret = FALSE;
  guint64 ret_offset = 0;
  guint i;
  GPtrArray *iter;
  ot_lvariant GVariant *csum_bytes_v = NULL;
  ot_lfree char *ret_pack_checksum = NULL;

  csum_bytes_v = ostree_checksum_to_bytes_v (checksum);

  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    iter = pull_data->cached_meta_pack_indexes;
  else
    iter = pull_data->cached_data_pack_indexes;
  for (i = 0; i < iter->len; i++)
    {
      const char *pack_checksum = iter->pdata[i];
      gboolean exists;

      if (!find_object_in_one_remote_pack (pull_data, csum_bytes_v, objtype,
                                           pack_checksum, &exists, &ret_offset,
                                           cancellable, error))
        goto out;

      if (exists)
        {
          ret_pack_checksum = g_strdup (pack_checksum);
          break;
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_pack_checksum, &ret_pack_checksum);
  if (out_offset)
    *out_offset = ret_offset;
 out:
  return ret;
}

static gboolean
fetch_one_cache_index (OtPullData           *pull_data,
                       const char           *pack_checksum,
                       gboolean              is_meta,
                       GCancellable         *cancellable,
                       GError              **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *tmp_path = NULL;
  ot_lfree char *pack_index_name = NULL;
  SoupURI *index_uri = NULL;

  pack_index_name = ostree_get_pack_index_name (is_meta, pack_checksum);
  index_uri = suburi_new (pull_data->base_uri, "objects", "pack", pack_index_name, NULL);
  
  if (!fetch_uri (pull_data, index_uri, "packindex-", &tmp_path,
                  cancellable, error))
    goto out;
  
  if (!ostree_repo_add_cached_remote_pack_index (pull_data->repo, pull_data->remote_name,
                                                 pack_checksum, is_meta, tmp_path,
                                                 cancellable, error))
    goto out;
  
  if (!ot_gfile_unlink (tmp_path, cancellable, error))
    goto out;
      
  g_clear_object (&tmp_path);

  ret = TRUE;
 out:
  if (tmp_path != NULL)
    (void) ot_gfile_unlink (tmp_path, NULL, NULL);
  if (index_uri)
    soup_uri_free (index_uri);
  return ret;
}

static gboolean
fetch_and_cache_pack_indexes (OtPullData        *pull_data,
                              GCancellable      *cancellable,
                              GError           **error)
{
  gboolean ret = FALSE;
  guint i;
  ot_lobj GFile *superindex_tmppath = NULL;
  ot_lptrarray GPtrArray *cached_meta_indexes = NULL;
  ot_lptrarray GPtrArray *cached_data_indexes = NULL;
  ot_lptrarray GPtrArray *uncached_meta_indexes = NULL;
  ot_lptrarray GPtrArray *uncached_data_indexes = NULL;
  ot_lvariant GVariant *superindex_variant = NULL;
  GVariantIter *contents_iter = NULL;
  SoupURI *superindex_uri = NULL;

  superindex_uri = suburi_new (pull_data->base_uri, "objects", "pack", "index", NULL);
  
  if (!fetch_uri (pull_data, superindex_uri, "index-",
                  &superindex_tmppath, cancellable, error))
    goto out;

  if (!ostree_repo_resync_cached_remote_pack_indexes (pull_data->repo, pull_data->remote_name,
                                                      superindex_tmppath,
                                                      &cached_meta_indexes,
                                                      &cached_data_indexes,
                                                      &uncached_meta_indexes,
                                                      &uncached_data_indexes,
                                                      cancellable, error))
    goto out;

  for (i = 0; i < cached_meta_indexes->len; i++)
    g_ptr_array_add (pull_data->cached_meta_pack_indexes,
                     g_strdup (cached_meta_indexes->pdata[i]));
  for (i = 0; i < cached_data_indexes->len; i++)
    g_ptr_array_add (pull_data->cached_data_pack_indexes,
                     g_strdup (cached_data_indexes->pdata[i]));

  for (i = 0; i < uncached_meta_indexes->len; i++)
    {
      const char *pack_checksum = uncached_meta_indexes->pdata[i];
      if (!fetch_one_cache_index (pull_data, pack_checksum, TRUE, cancellable, error))
        goto out;
      g_ptr_array_add (pull_data->cached_meta_pack_indexes, g_strdup (pack_checksum));
    }
  for (i = 0; i < uncached_data_indexes->len; i++)
    {
      const char *pack_checksum = uncached_data_indexes->pdata[i];
      if (!fetch_one_cache_index (pull_data, pack_checksum, FALSE, cancellable, error))
        goto out;
      g_ptr_array_add (pull_data->cached_data_pack_indexes, g_strdup (pack_checksum));
    }

  ret = TRUE;
 out:
  if (superindex_uri)
    soup_uri_free (superindex_uri);
  if (contents_iter)
    g_variant_iter_free (contents_iter);
  return ret;
}

static gboolean
fetch_loose_object (OtPullData  *pull_data,
                    const char  *checksum,
                    OstreeObjectType objtype,
                    GFile           **out_temp_path,
                    GCancellable *cancellable,
                    GError     **error)
{
  gboolean ret = FALSE;
  ot_lfree char *objpath = NULL;
  ot_lobj GFile *ret_temp_path = NULL;
  SoupURI *obj_uri = NULL;

  objpath = ostree_get_relative_object_path (checksum, objtype);
  obj_uri = suburi_new (pull_data->base_uri, objpath, NULL);
  
  if (!fetch_uri (pull_data, obj_uri, ostree_object_type_to_string (objtype), &ret_temp_path,
                  cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_temp_path, &ret_temp_path);
 out:
  if (obj_uri)
    soup_uri_free (obj_uri);
  return ret;
}

static gboolean
find_object_ensure_indexes (OtPullData            *pull_data,
                            const char            *checksum,
                            OstreeObjectType       objtype,
                            gboolean              *out_is_stored,
                            char                 **out_remote_pack_checksum,
                            guint64               *out_remote_pack_offset,
                            GCancellable          *cancellable,
                            GError               **error)
{
  gboolean ret = FALSE;
  gboolean ret_is_stored;
  ot_lfree char *ret_remote_pack_checksum = NULL;

  if (!ostree_repo_has_object (pull_data->repo, objtype, checksum, &ret_is_stored,
                               cancellable, error))
    goto out;

  if (!ret_is_stored)
    {
      if (!pull_data->fetched_packs)
        {
          pull_data->fetched_packs = TRUE;
          pull_data->cached_meta_pack_indexes = g_ptr_array_new_with_free_func (g_free);
          pull_data->cached_data_pack_indexes = g_ptr_array_new_with_free_func (g_free);

          if (!fetch_and_cache_pack_indexes (pull_data, cancellable, error))
            goto out;
        }

      if (!find_object_in_remote_packs (pull_data, checksum, objtype, 
                                        &ret_remote_pack_checksum, out_remote_pack_offset,
                                        cancellable, error))
        goto out;
    }

  ret = TRUE;
  if (out_is_stored)
    *out_is_stored = ret_is_stored;
  ot_transfer_out_value (out_remote_pack_checksum, &ret_remote_pack_checksum);
 out:
  return ret;
}

static gboolean
find_object_ensure_pack_data (OtPullData            *pull_data,
                              const char            *checksum,
                              OstreeObjectType       objtype,
                              gboolean              *out_is_stored,
                              GFile                **out_remote_pack_path,
                              guint64               *out_remote_pack_offset,
                              GCancellable          *cancellable,
                              GError               **error)
{
  gboolean ret = FALSE;
  gboolean ret_is_stored;
  ot_lfree char *remote_pack_checksum = NULL;
  ot_lobj GFile *ret_remote_pack_path = NULL;

  if (!find_object_ensure_indexes (pull_data, checksum, objtype, &ret_is_stored,
                                   &remote_pack_checksum, out_remote_pack_offset,
                                   cancellable, error))
    goto out;

  if (!ret_is_stored)
    {
      if (remote_pack_checksum)
        {
          if (!fetch_one_pack_file (pull_data, remote_pack_checksum, OSTREE_OBJECT_TYPE_IS_META (objtype),
                                    &ret_remote_pack_path, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
  if (out_is_stored)
    *out_is_stored = ret_is_stored;
  ot_transfer_out_value (out_remote_pack_path, &ret_remote_pack_path);
  /* offset set above */
 out:
  return ret;
}

static gboolean
fetch_and_store_metadata (OtPullData          *pull_data,
                          const char          *checksum,
                          OstreeObjectType     objtype,
                          GVariant           **out_variant,
                          GCancellable        *cancellable,
                          GError             **error)
{
  gboolean ret = FALSE;
  guint64 pack_offset = 0;
  gboolean is_stored;
  ot_lvariant GVariant *ret_variant = NULL;
  ot_lobj GFile *remote_pack_path = NULL;
  ot_lobj GFile *temp_path = NULL;
  ot_lobj GInputStream *input = NULL;
  ot_lvariant GVariant *pack_entry = NULL;
  ot_lvariant GVariant *metadata = NULL;
  GMappedFile *pack_map = NULL;

  g_assert (OSTREE_OBJECT_TYPE_IS_META (objtype));

  if (!find_object_ensure_pack_data (pull_data, checksum, objtype,
                                     &is_stored, &remote_pack_path, &pack_offset,
                                     cancellable, error))
    goto out;
      
  if (remote_pack_path != NULL)
    {
      g_assert (!is_stored);

      pack_map = g_mapped_file_new (ot_gfile_get_path_cached (remote_pack_path), FALSE, error);
      if (!pack_map)
        goto out;

      if (!ostree_read_pack_entry_raw ((guchar*)g_mapped_file_get_contents (pack_map),
                                       g_mapped_file_get_length (pack_map),
                                       pack_offset, FALSE, TRUE, &pack_entry,
                                       cancellable, error))
        goto out;

      g_variant_get_child (pack_entry, 2, "v", &metadata);
      
      input = ot_variant_read (metadata);
    }
  else if (!is_stored)
    {
      if (!fetch_loose_object (pull_data, checksum, objtype, &temp_path, cancellable, error))
        goto out;
      
      input = (GInputStream*)g_file_read (temp_path, cancellable, error);
      if (!input)
        goto out;
    }

  if (input)
    {
      g_assert (remote_pack_path != NULL || !is_stored);
      if (!ostree_repo_stage_object (pull_data->repo, objtype, checksum, input,
                                     cancellable, error))
        goto out;
    }

  if (!ostree_repo_load_variant (pull_data->repo, objtype, checksum,
                                 &ret_variant, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_variant, &ret_variant);
 out:
  if (temp_path)
    (void) ot_gfile_unlink (temp_path, NULL, NULL);
  if (pack_map)
    g_mapped_file_unref (pack_map);
  return ret;
}

static gboolean
fetch_and_store_tree_metadata_recurse (OtPullData   *pull_data,
                                       int           depth,
                                       const char   *rev,
                                       GCancellable *cancellable,
                                       GError      **error)
{
  gboolean ret = FALSE;
  int i, n;
  ot_lvariant GVariant *tree = NULL;
  ot_lvariant GVariant *files_variant = NULL;
  ot_lvariant GVariant *dirs_variant = NULL;
  ot_lobj GFile *stored_path = NULL;
  ot_lfree char *pack_checksum = NULL;

  if (depth > OSTREE_MAX_RECURSION)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exceeded maximum recursion");
      goto out;
    }

  if (!fetch_and_store_metadata (pull_data, rev, OSTREE_OBJECT_TYPE_DIR_TREE,
                                 &tree, cancellable, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
  files_variant = g_variant_get_child_value (tree, 0);
  dirs_variant = g_variant_get_child_value (tree, 1);
      
  n = g_variant_n_children (files_variant);
  for (i = 0; i < n; i++)
    {
      const char *filename;
      ot_lvariant GVariant *csum = NULL;

      g_variant_get_child (files_variant, i, "(&s@ay)", &filename, &csum);

      if (!ot_util_filename_validate (filename, error))
        goto out;

      {
        char *duped_key = ostree_checksum_from_bytes_v (csum);
        g_hash_table_replace (pull_data->file_checksums_to_fetch,
                              duped_key, duped_key);
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

      g_free (tmp_checksum);
      tmp_checksum = ostree_checksum_from_bytes_v (meta_csum);
      if (!fetch_and_store_metadata (pull_data, tmp_checksum, OSTREE_OBJECT_TYPE_DIR_META,
                                     NULL, cancellable, error))
        goto out;

      g_free (tmp_checksum);
      tmp_checksum = ostree_checksum_from_bytes_v (tree_csum);
      if (!fetch_and_store_tree_metadata_recurse (pull_data, depth+1, tmp_checksum, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
fetch_and_store_commit_metadata_recurse (OtPullData   *pull_data,
                                         int           parent_depth,
                                         int           related_depth,
                                         const char   *rev,
                                         GCancellable *cancellable,
                                         GError      **error)
{
  gboolean ret = FALSE;
  ot_lvariant GVariant *commit = NULL;
  ot_lvariant GVariant *related_objects = NULL;
  ot_lvariant GVariant *tree_contents_csum = NULL;
  ot_lvariant GVariant *tree_meta_csum = NULL;
  ot_lfree char *tmp_checksum = NULL;
  GVariantIter *iter = NULL;

  if (!fetch_and_store_metadata (pull_data, rev, OSTREE_OBJECT_TYPE_COMMIT,
                                 &commit, cancellable, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  g_variant_get_child (commit, 6, "@ay", &tree_contents_csum);
  g_variant_get_child (commit, 7, "@ay", &tree_meta_csum);

  g_free (tmp_checksum);
  tmp_checksum = ostree_checksum_from_bytes_v (tree_meta_csum);
  if (!fetch_and_store_metadata (pull_data, tmp_checksum, OSTREE_OBJECT_TYPE_DIR_META,
                                 NULL, cancellable, error))
    goto out;
  
  g_free (tmp_checksum);
  tmp_checksum = ostree_checksum_from_bytes_v (tree_contents_csum);
  if (!fetch_and_store_tree_metadata_recurse (pull_data, 0, tmp_checksum,
                                              cancellable, error))
    goto out;

  if (opt_related)
    {
      const char *name;
      ot_lvariant GVariant *csum_v = NULL;

      if (parent_depth > OSTREE_MAX_RECURSION
          || related_depth > OSTREE_MAX_RECURSION)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Exceeded maximum recursion");
          goto out;
        }

      related_objects = g_variant_get_child_value (commit, 2);
      iter = g_variant_iter_new (related_objects);

      while (g_variant_iter_loop (iter, "(&s@ay)", &name, &csum_v))
        {
          ot_lfree char *checksum = ostree_checksum_from_bytes_v (csum_v);

          /* Pass opt_depth here to ensure we aren't fetching parents of related */
          if (!fetch_and_store_commit_metadata_recurse (pull_data, opt_depth,
                                                        related_depth + 1, checksum,
                                                        cancellable, error))
            goto out;
        }
    }

  if (parent_depth < opt_depth)
    {
      ot_lvariant GVariant *parent_csum_v = NULL;

      parent_csum_v = g_variant_get_child_value (commit, 1);

      if (g_variant_n_children (parent_csum_v) > 0)
        {
          ot_lfree char *checksum = ostree_checksum_from_bytes_v (parent_csum_v);

          if (!fetch_and_store_commit_metadata_recurse (pull_data, parent_depth + 1,
                                                        0, checksum,
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

static gboolean
store_file_from_pack (OtPullData          *pull_data,
                      const char          *checksum,
                      const char          *pack_checksum,
                      GFile               *pack_file,
                      GCancellable        *cancellable,
                      GError             **error)
{
  gboolean ret = FALSE;
  gboolean exists;
  guint64 pack_offset;
  ot_lobj GFile *remote_pack_path = NULL;
  ot_lobj GFile *temp_path = NULL;
  ot_lvariant GVariant *pack_entry = NULL;
  ot_lobj GInputStream *input = NULL;
  ot_lobj GInputStream *file_object_input = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lvariant GVariant *xattrs = NULL;
  ot_lvariant GVariant *csum_bytes_v = NULL;
  GMappedFile *pack_map = NULL;

  csum_bytes_v = ostree_checksum_to_bytes_v (checksum);

  if (!find_object_in_one_remote_pack (pull_data, csum_bytes_v, OSTREE_OBJECT_TYPE_FILE,
                                       pack_checksum, &exists, &pack_offset,
                                       cancellable, error))
    goto out;

  g_assert (exists);

  pack_map = g_mapped_file_new (ot_gfile_get_path_cached (pack_file), FALSE, error);
  if (!pack_map)
    goto out;
  
  if (!ostree_read_pack_entry_raw ((guchar*)g_mapped_file_get_contents (pack_map),
                                   g_mapped_file_get_length (pack_map),
                                   pack_offset, FALSE, FALSE, &pack_entry,
                                   cancellable, error))
    goto out;
  
  if (!ostree_parse_file_pack_entry (pack_entry, &input, &file_info, &xattrs,
                                     cancellable, error))
    goto out;

  if (!ostree_raw_file_to_content_stream (input, file_info, xattrs,
                                          &file_object_input, NULL, cancellable, error))
    goto out;
  
  if (!ostree_repo_stage_object (pull_data->repo, OSTREE_OBJECT_TYPE_FILE, checksum,
                                 file_object_input,
                                 cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (pack_map)
    g_mapped_file_unref (pack_map);
  return ret;
}

typedef struct {
  OtPullData *pull_data;

  gboolean fetching_content;

  GFile *meta_path;
  GFile *content_path;

  char *checksum;
} OtFetchOneContentItemData;

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
content_fetch_on_checksum_complete (GObject        *object,
                                    GAsyncResult   *result,
                                    gpointer        user_data)
{
  OtFetchOneContentItemData *data = user_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  guint64 length;
  GCancellable *cancellable = NULL;
  ot_lfree guchar *csum;
  ot_lvariant GVariant *file_meta = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lvariant GVariant *xattrs = NULL;
  ot_lobj GInputStream *content_input = NULL;
  ot_lobj GInputStream *file_object_input = NULL;
  ot_lfree char *checksum;

  csum = ot_gio_checksum_stream_finish ((GInputStream*)object, result, error);
  if (!csum)
    goto out;

  if (!ot_util_variant_map (data->meta_path, OSTREE_FILE_HEADER_GVARIANT_FORMAT, FALSE,
                            &file_meta, error))
    goto out;
  
  if (!ostree_file_header_parse (file_meta, &file_info, &xattrs, error))
    goto out;

  if (data->content_path)
    {
      content_input = (GInputStream*)g_file_read (data->content_path, cancellable, error);
      if (!content_input)
        goto out;
    }

  if (!ostree_raw_file_to_content_stream (content_input, file_info, xattrs,
                                          &file_object_input, &length,
                                          cancellable, error))
    goto out;

  checksum = ostree_checksum_from_bytes (csum);

  if (strcmp (checksum, data->checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted object %s (actual checksum is %s)",
                   data->checksum, checksum);
      goto out;
    }

  if (!ostree_repo_stage_file_object_trusted (data->pull_data->repo, checksum,
                                              FALSE, file_object_input, length,
                                              cancellable, error))
    goto out;

 out:
  data->pull_data->outstanding_checksum_requests--;
  check_outstanding_requests_handle_error (data->pull_data, local_error);
  destroy_fetch_one_content_item_data (data);
}

static void
enqueue_loose_meta_requests (OtPullData   *pull_data);

static void
content_fetch_on_complete (GObject        *object,
                           GAsyncResult   *result,
                           gpointer        user_data) 
{
  OtFetchOneContentItemData *data = user_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  GCancellable *cancellable = NULL;
  gboolean was_content_fetch = FALSE;
  gboolean need_content_fetch = FALSE;
  ot_lvariant GVariant *file_meta = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GInputStream *content_input = NULL;
  ot_lobj GInputStream *file_object_input = NULL;
  ot_lvariant GVariant *xattrs = NULL;

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

  if (!need_content_fetch)
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
                                              &file_object_input, NULL,
                                              cancellable, error))
        goto out;

      data->pull_data->outstanding_checksum_requests++;
      ot_gio_checksum_stream_async (file_object_input, G_PRIORITY_DEFAULT, NULL,
                                    content_fetch_on_checksum_complete, data);
    }

 out:
  if (was_content_fetch)
    data->pull_data->outstanding_filecontent_requests--;
  else
    {
      data->pull_data->outstanding_filemeta_requests--;
      enqueue_loose_meta_requests (data->pull_data);
    }
  check_outstanding_requests_handle_error (data->pull_data, local_error);
}

static void
enqueue_loose_meta_requests (OtPullData *pull_data)
{
  GHashTableIter hash_iter;
  gpointer key, value;
  GCancellable *cancellable = NULL;

  g_hash_table_iter_init (&hash_iter, pull_data->loose_files);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *checksum = key;
      ot_lfree char *objpath = NULL;
      SoupURI *obj_uri = NULL;
      OtFetchOneContentItemData *one_item_data;
          
      one_item_data = g_new0 (OtFetchOneContentItemData, 1);
      one_item_data->pull_data = pull_data;
      one_item_data->checksum = g_strdup (checksum);
      one_item_data->fetching_content = FALSE;
          
      objpath = ostree_get_relative_object_path (checksum, OSTREE_OBJECT_TYPE_FILE);
      obj_uri = suburi_new (pull_data->base_uri, objpath, NULL);

      ostree_fetcher_request_uri_async (pull_data->fetcher, obj_uri, cancellable,
                                        content_fetch_on_complete, one_item_data);
      soup_uri_free (obj_uri);

      pull_data->outstanding_filemeta_requests++;
      g_hash_table_iter_remove (&hash_iter);

      /* Don't let too many requests queue up; when we're fetching
       * files we need to process the actual content.
       */
      if (pull_data->outstanding_filemeta_requests > 20)
        break;
    }
}

static gboolean
fetch_content (OtPullData           *pull_data,
               GCancellable         *cancellable,
               GError              **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  ot_lobj GFile *temp_path = NULL;
  ot_lobj GFile *content_temp_path = NULL;
  ot_lhash GHashTable *data_packs_to_fetch = NULL;
  ot_lhash GHashTable *loose_files = NULL;
  SoupURI *content_uri = NULL;
  guint n_objects_to_fetch = 0;

  data_packs_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, 
                                               (GDestroyNotify) g_ptr_array_unref);
  loose_files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  
  g_hash_table_iter_init (&hash_iter, pull_data->file_checksums_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *checksum = key;
      GPtrArray *files_to_fetch;
      gboolean is_stored;
      ot_lfree char *remote_pack_checksum = NULL;

      if (!find_object_ensure_indexes (pull_data, checksum, OSTREE_OBJECT_TYPE_FILE,
                                       &is_stored, &remote_pack_checksum, NULL,
                                       cancellable, error))
        goto out;

      if (remote_pack_checksum)
        {
          files_to_fetch = g_hash_table_lookup (data_packs_to_fetch, remote_pack_checksum);
          if (files_to_fetch == NULL)
            {
              files_to_fetch = g_ptr_array_new_with_free_func (g_free);
              g_hash_table_insert (data_packs_to_fetch, remote_pack_checksum, files_to_fetch);
              /* transfer ownership */
              remote_pack_checksum = NULL;
            }
          g_ptr_array_add (files_to_fetch, g_strdup (checksum));
          n_objects_to_fetch++;
        }
      else if (!is_stored)
        {
          char *key = g_strdup (checksum);
          g_hash_table_insert (loose_files, key, key);
          n_objects_to_fetch++;
        }
    }

  g_hash_table_iter_init (&hash_iter, data_packs_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *pack_checksum = key;
      GPtrArray *files = value;
      ot_lvariant GVariant *mapped_pack = NULL;
      ot_lvariant GVariant *content_list = NULL;
      gboolean fetch;

      if (!ostree_repo_map_cached_remote_pack_index (pull_data->repo, pull_data->remote_name,
                                                     pack_checksum, FALSE,
                                                     &mapped_pack,
                                                     cancellable, error))
        goto out;
      
      content_list = g_variant_get_child_value (mapped_pack, 2);

      fetch = (((double)files->len) / g_variant_n_children (content_list)) > ((double)opt_packfile_threshold / 100);
      if (!fetch)
        {
          guint i;
          for (i = 0; i < files->len; i++)
            {
              g_hash_table_insert (loose_files, files->pdata[i], files->pdata[i]);
              files->pdata[i] = NULL;  /* steal data */
            }
          g_hash_table_iter_remove (&hash_iter);
        }
    }

  if (n_objects_to_fetch > 0)
    g_print ("%u content objects to fetch\n", n_objects_to_fetch);

  if (g_hash_table_size (data_packs_to_fetch) > 0)
    g_print ("Fetching %u content packs\n",
             g_hash_table_size (data_packs_to_fetch));

  g_hash_table_iter_init (&hash_iter, data_packs_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *pack_checksum = key;
      GPtrArray *file_checksums = value;
      guint i;
      ot_lobj GFile *pack_path = NULL;

      if (!fetch_one_pack_file (pull_data, pack_checksum, FALSE,
                                &pack_path, cancellable, error))
        goto out;

      g_print ("Storing %u objects from content pack %s\n", file_checksums->len,
               pack_checksum);
      for (i = 0; i < file_checksums->len; i++)
        {
          const char *checksum = file_checksums->pdata[i];
          if (!store_file_from_pack (pull_data, checksum, pack_checksum, pack_path,
                                     cancellable, error))
            goto out;
        }

      if (!ostree_repo_take_cached_remote_pack_data (pull_data->repo, pull_data->remote_name,
                                                     pack_checksum, FALSE, NULL,
                                                     cancellable, error))
        goto out;
    }

  if (g_hash_table_size (loose_files) > 0)
    g_print ("Fetching %u loose objects\n",
             g_hash_table_size (loose_files));
  
  pull_data->loose_files = loose_files;
  
  if (g_hash_table_size (loose_files) > 0)
    {
      enqueue_loose_meta_requests (pull_data);

      run_mainloop_monitor_fetcher (pull_data);

      if (pull_data->caught_error)
        goto out;
    }

  ret = TRUE;
 out:
  if (content_uri)
    soup_uri_free (content_uri);
  if (temp_path)
    (void) ot_gfile_unlink (temp_path, NULL, NULL);
  if (content_temp_path)
    (void) ot_gfile_unlink (content_temp_path, NULL, NULL);
  return ret;
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
  OstreeRepoMode remote_repo_mode;
  ot_lfree char *remote_key = NULL;
  ot_lobj OstreeRepo *repo = NULL;
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
  pull_data->file_checksums_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

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

  if (!ostree_repo_mode_from_string (remote_mode_str, &remote_repo_mode, error))
    goto out;

  switch (remote_repo_mode)
    {
    case OSTREE_REPO_MODE_ARCHIVE:
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

  g_print ("Analyzing objects needed...\n");

  g_hash_table_iter_init (&hash_iter, commits_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *commit = value;
      
      if (!fetch_and_store_commit_metadata_recurse (pull_data, 0, 0, commit,
                                                    cancellable, error))
        goto out;
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

      /* Only skip traversal if depth == 0; otherwise, we have to
       * handle the case where the user specified a bigger depth than
       * they originally did.
       */
      if (original_rev && strcmp (sha256, original_rev) == 0 && opt_depth == 0)
        {
          g_print ("No changes in %s\n", remote_ref);
        }
      else
        {
          if (!ostree_validate_checksum_string (sha256, error))
            goto out;

          if (!fetch_and_store_commit_metadata_recurse (pull_data, 0, 0, sha256, cancellable, error))
            goto out;
         
          g_hash_table_insert (updated_refs, g_strdup (ref), g_strdup (sha256));
        }
    }

  if (!fetch_content (pull_data, cancellable, error))
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

  if (!ostree_repo_clean_cached_remote_pack_data (pull_data->repo, pull_data->remote_name,
                                                  cancellable, error))
    goto out;

  bytes_transferred = ostree_fetcher_bytes_transferred (pull_data->fetcher);
  if (bytes_transferred > 0)
    {
      g_print ("%" G_GUINT64_FORMAT " KiB transferred\n", (guint64)(bytes_transferred / 1024.0));
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
  g_clear_pointer (&pull_data->file_checksums_to_fetch, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->cached_meta_pack_indexes, (GDestroyNotify) g_ptr_array_unref);
  g_clear_pointer (&pull_data->cached_data_pack_indexes, (GDestroyNotify) g_ptr_array_unref);
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
