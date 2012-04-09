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

#include <libsoup/soup-gnome.h>

#include "ostree.h"
#include "ot-main.h"

gboolean verbose;

static GOptionEntry options[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Show more information", NULL },
  { NULL },
};

static void
log_verbose (const char  *fmt,
             ...) G_GNUC_PRINTF (1, 2);

static void
log_verbose (const char  *fmt,
             ...)
{
  va_list args;
  char *msg;

  if (!verbose)
    return;

  va_start (args, fmt);
  
  msg = g_strdup_vprintf (fmt, args);
  g_print ("%s\n", msg);
  g_free (msg);
}

typedef struct {
  OstreeRepo   *repo;
  char         *remote_name;
  SoupSession  *session;
  SoupURI      *base_uri;

  gboolean      fetched_packs;
  GPtrArray    *cached_pack_indexes;

  GHashTable   *file_checksums_to_fetch;

  gboolean      stdout_is_tty;

  guint64       dl_current_bytes;
  guint64       dl_total_bytes;
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

typedef struct {
  OtPullData     *pull_data;
  GOutputStream  *stream;
  gboolean        had_error;
  GError        **error;
} OstreeSoupChunkData;

static void
sync_progress (OtPullData   *pull_data)
{
  if (pull_data->stdout_is_tty)
    g_print ("%c8%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT " KiB",
             0x1b, (pull_data->dl_current_bytes / 1024), (pull_data->dl_total_bytes / 1024));
}

static void
on_got_chunk (SoupMessage   *msg,
              SoupBuffer    *buf,
              gpointer       user_data)
{
  OstreeSoupChunkData *data = user_data;
  gsize bytes_written;

  data->pull_data->dl_current_bytes += buf->length;
  sync_progress (data->pull_data);

  if (!g_output_stream_write_all (data->stream, buf->data, buf->length,
                                  &bytes_written, NULL, data->error))
    {
      data->had_error = TRUE;
      soup_session_cancel_message (data->pull_data->session, msg, 500);
    }
}

static void
on_got_content_length (SoupMessage        *msg,
                       OtPullData         *pull_data)
{
  goffset size;

  g_assert (msg->response_headers);
  
  size = soup_message_headers_get_content_length (msg->response_headers);
  if (size > 0)
    pull_data->dl_total_bytes = (guint64) size;

  sync_progress (pull_data);
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
  guint response;
  ot_lfree char *uri_string = NULL;
  ot_lobj GFile *ret_temp_filename = NULL;
  ot_lobj GOutputStream *output_stream = NULL;
  ot_lobj SoupMessage *msg = NULL;
  OstreeSoupChunkData chunkdata;

  if (!ostree_create_temp_regular_file (ostree_repo_get_tmpdir (pull_data->repo),
                                        tmp_prefix, NULL,
                                        &ret_temp_filename,
                                        &output_stream,
                                        NULL, error))
    goto out;

  chunkdata.pull_data = pull_data;
  chunkdata.stream = output_stream;
  chunkdata.had_error = FALSE;
  chunkdata.error = error;
  
  uri_string = soup_uri_to_string (uri, FALSE);
  g_print ("Fetching %s\n", uri_string);

  if (pull_data->stdout_is_tty)
    {
      g_print ("0/? KiB");
      pull_data->dl_current_bytes = 0;
      pull_data->dl_total_bytes = 0;
      sync_progress (pull_data);
    }

  msg = soup_message_new_from_uri (SOUP_METHOD_GET, uri);

  soup_message_body_set_accumulate (msg->response_body, FALSE);

  soup_message_add_header_handler (msg, "got-headers",
                                   "Content-Length",
                                   G_CALLBACK (on_got_content_length),
                                   pull_data);
  g_signal_connect (msg, "got-chunk", G_CALLBACK (on_got_chunk), &chunkdata);
  
  response = soup_session_send_message (pull_data->session, msg);
  if (response != 200)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to retrieve '%s': %d %s",
                   uri_string, response, msg->reason_phrase);
      goto out;
    }

  if (!g_output_stream_close (output_stream, NULL, error))
    goto out;

  g_print ("\n");
  
  ret = TRUE;
  ot_transfer_out_value (out_temp_filename, &ret_temp_filename);
 out:
  if (ret_temp_filename)
    (void) unlink (ot_gfile_get_path_cached (ret_temp_filename));
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
                                                pack_checksum, &ret_cached_path,
                                                cancellable, error))
    goto out;

  if (ret_cached_path == NULL)
    {
      pack_name = g_strconcat ("ostpack-", pack_checksum, ".data", NULL);
      pack_uri = suburi_new (pull_data->base_uri, "objects", "pack", pack_name, NULL);
      
      if (!fetch_uri (pull_data, pack_uri, "packdata-", &tmp_path, cancellable, error))
        goto out;

      if (!ostree_repo_take_cached_remote_pack_data (pull_data->repo, pull_data->remote_name,
                                                     pack_checksum, tmp_path,
                                                     cancellable, error))
        goto out;
    }

  if (!ostree_repo_get_cached_remote_pack_data (pull_data->repo, pull_data->remote_name,
                                                pack_checksum, &ret_cached_path,
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
find_object_in_remote_packs (OtPullData       *pull_data,
                             const char       *checksum,
                             OstreeObjectType  objtype,
                             char            **out_pack_checksum,
                             guint64          *out_offset,
                             GCancellable     *cancellable,
                             GError          **error)
{
  gboolean ret = FALSE;
  guint64 offset;
  guint i;
  ot_lvariant GVariant *mapped_pack = NULL;
  ot_lvariant GVariant *csum_bytes = NULL;
  ot_lfree char *ret_pack_checksum = NULL;

  csum_bytes = ostree_checksum_to_bytes_v (checksum);

  for (i = 0; i < pull_data->cached_pack_indexes->len; i++)
    {
      const char *pack_checksum = pull_data->cached_pack_indexes->pdata[i];

      ot_clear_gvariant (&mapped_pack);
      if (!ostree_repo_map_cached_remote_pack_index (pull_data->repo, pull_data->remote_name,
                                                     pack_checksum, &mapped_pack,
                                                     cancellable, error))
        goto out;

      if (ostree_pack_index_search (mapped_pack, csum_bytes, objtype, &offset))
        {
          ret_pack_checksum = g_strdup (pack_checksum);
          break;
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_pack_checksum, &ret_pack_checksum);
  if (out_offset)
    *out_offset = offset;
 out:
  return ret;
}

static gboolean
fetch_one_cache_index (OtPullData          *pull_data,
                      const char           *pack_checksum,
                      GCancellable         *cancellable,
                      GError              **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *tmp_path = NULL;
  ot_lfree char *pack_index_name = NULL;
  SoupURI *index_uri = NULL;

  pack_index_name = g_strconcat ("ostpack-", pack_checksum, ".index", NULL);
  index_uri = suburi_new (pull_data->base_uri, "objects", "pack", pack_index_name, NULL);
  
  if (!fetch_uri (pull_data, index_uri, "packindex-", &tmp_path,
                  cancellable, error))
    goto out;
  
  if (!ostree_repo_add_cached_remote_pack_index (pull_data->repo, pull_data->remote_name,
                                                 pack_checksum, tmp_path,
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
  ot_lptrarray GPtrArray *cached_indexes = NULL;
  ot_lptrarray GPtrArray *uncached_indexes = NULL;
  ot_lvariant GVariant *superindex_variant = NULL;
  GVariantIter *contents_iter = NULL;
  SoupURI *superindex_uri = NULL;

  superindex_uri = suburi_new (pull_data->base_uri, "objects", "pack", "index", NULL);
  
  if (!fetch_uri (pull_data, superindex_uri, "index-",
                  &superindex_tmppath, cancellable, error))
    goto out;

  if (!ostree_repo_resync_cached_remote_pack_indexes (pull_data->repo, pull_data->remote_name,
                                                      superindex_tmppath,
                                                      &cached_indexes, &uncached_indexes,
                                                      cancellable, error))
    goto out;

  for (i = 0; i < cached_indexes->len; i++)
    g_ptr_array_add (pull_data->cached_pack_indexes,
                     g_strdup (cached_indexes->pdata[i]));

  for (i = 0; i < uncached_indexes->len; i++)
    {
      const char *pack_checksum = uncached_indexes->pdata[i];

      if (!fetch_one_cache_index (pull_data, pack_checksum, cancellable, error))
        goto out;
      
      g_ptr_array_add (pull_data->cached_pack_indexes, g_strdup (pack_checksum));
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

static void
unlink_file_on_unref (GFile *f)
{
  (void) ot_gfile_unlink (f, NULL, NULL);
  g_object_unref (f);
}

static gboolean
fetch_object_if_not_stored (OtPullData           *pull_data,
                            const char           *checksum,
                            OstreeObjectType      objtype,
                            GInputStream        **out_input,
                            GCancellable         *cancellable,
                            GError              **error)
{
  gboolean ret = FALSE;
  guint64 pack_offset = 0;
  gboolean is_stored;
  ot_lobj GInputStream *ret_input = NULL;
  ot_lobj GFile *temp_path = NULL;
  ot_lobj GFile *stored_path = NULL;
  ot_lobj GFile *pack_path = NULL;
  ot_lfree char *local_pack_checksum = NULL;
  ot_lfree char *remote_pack_checksum = NULL;
  ot_lvariant GVariant *pack_entry = NULL;
  GMappedFile *pack_map = NULL;

  if (!ostree_repo_find_object (pull_data->repo, objtype, checksum,
                                &stored_path, &local_pack_checksum, NULL,
                                cancellable, error))
    goto out;

  is_stored = (stored_path != NULL || local_pack_checksum != NULL);
  if (!is_stored)
    {
      if (!pull_data->fetched_packs)
        {
          pull_data->fetched_packs = TRUE;
          pull_data->cached_pack_indexes = g_ptr_array_new_with_free_func (g_free);

          if (!fetch_and_cache_pack_indexes (pull_data, cancellable, error))
            goto out;
        }

      if (!find_object_in_remote_packs (pull_data, checksum, objtype, 
                                        &remote_pack_checksum, &pack_offset,
                                        cancellable, error))
        goto out;
    }
      
  if (remote_pack_checksum != NULL)
    {
      g_assert (!is_stored);

      if (!fetch_one_pack_file (pull_data, remote_pack_checksum, &pack_path,
                                cancellable, error))
        goto out;

      pack_map = g_mapped_file_new (ot_gfile_get_path_cached (pack_path), FALSE, error);
      if (!pack_map)
        goto out;

      if (!ostree_read_pack_entry_raw ((guchar*)g_mapped_file_get_contents (pack_map),
                                       g_mapped_file_get_length (pack_map),
                                       pack_offset, FALSE, &pack_entry,
                                       cancellable, error))
        goto out;

      /* Kind of a hack... */
      ret_input = ostree_read_pack_entry_as_stream (pack_entry);
      g_object_set_data_full ((GObject*)ret_input, "ostree-pull-pack-map",
                              pack_map, (GDestroyNotify) g_mapped_file_unref);
      pack_map = NULL; /* Transfer ownership */
    }
  else if (!is_stored)
    {
      if (!fetch_loose_object (pull_data, checksum, objtype, &temp_path, cancellable, error))
        goto out;
      
      ret_input = (GInputStream*)g_file_read (temp_path, cancellable, error);
      if (!ret_input)
        goto out;
      g_object_set_data_full ((GObject*)ret_input, "ostree-tmpfile-unlink",
                              g_object_ref (temp_path),
                              (GDestroyNotify)unlink_file_on_unref);
    }

  ret = TRUE;
  ot_transfer_out_value (out_input, &ret_input);
 out:
  if (pack_map)
    g_mapped_file_unref (pack_map);
  return ret;
}

static gboolean
fetch_and_store_object (OtPullData       *pull_data,
                        const char       *checksum,
                        OstreeObjectType objtype,
                        GCancellable     *cancellable,
                        GError          **error)
{
  gboolean ret = FALSE;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GInputStream *input = NULL;

  g_assert (objtype != OSTREE_OBJECT_TYPE_RAW_FILE);

  if (!fetch_object_if_not_stored (pull_data, checksum, objtype, &input,
                                   cancellable, error))
    goto out;

  if (input)
    {
      if (!ostree_repo_stage_object (pull_data->repo, objtype, checksum, NULL, NULL,
                                     input, cancellable, error))
        goto out;

      log_verbose ("Staged object: %s.%s", checksum, ostree_object_type_to_string (objtype));
    }

  ret = TRUE;
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
  ot_lvariant GVariant *ret_variant = NULL;

  if (!fetch_and_store_object (pull_data, checksum, objtype,
                               cancellable, error))
    goto out;

  if (!ostree_repo_load_variant (pull_data->repo, objtype, checksum,
                                 &ret_variant, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_variant, &ret_variant);
 out:
  return ret;
}

static gboolean
fetch_and_store_file (OtPullData          *pull_data,
                      const char          *checksum,
                      GCancellable        *cancellable,
                      GError             **error)
{
  gboolean ret = FALSE;
  gboolean skip_archive_fetch;
  ot_lobj GInputStream *input = NULL;
  ot_lobj GFile *stored_path = NULL;
  ot_lfree char *pack_checksum = NULL;
  ot_lvariant GVariant *archive_metadata_container = NULL;
  ot_lvariant GVariant *archive_metadata = NULL;
  ot_lobj GFileInfo *archive_file_info = NULL;
  ot_lvariant GVariant *archive_xattrs = NULL;

  /* If we're fetching from an archive into a bare repository, we need
   * to explicitly check for raw file types locally.
   */
  if (ostree_repo_get_mode (pull_data->repo) == OSTREE_REPO_MODE_BARE)
    {
      if (!ostree_repo_find_object (pull_data->repo, OSTREE_OBJECT_TYPE_RAW_FILE,
                                    checksum, &stored_path, &pack_checksum,
                                    NULL, cancellable, error))
        goto out;
      
      skip_archive_fetch = (stored_path || pack_checksum);
    }
  else
    {
      skip_archive_fetch = FALSE;
    }

  if (!skip_archive_fetch)
    {
      if (!fetch_object_if_not_stored (pull_data, checksum,
                                       OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META,
                                       &input, cancellable, error))
        goto out;

      if (input != NULL)
        {
          if (!ot_util_variant_from_stream (input, OSTREE_SERIALIZED_VARIANT_FORMAT,
                                            FALSE, &archive_metadata_container, cancellable, error))
            goto out;

          if (!ostree_unwrap_metadata (archive_metadata_container, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META,
                                       &archive_metadata, error))
            goto out;
  
          if (!ostree_parse_archived_file_meta (archive_metadata, &archive_file_info,
                                                &archive_xattrs, error))
            goto out;

          g_clear_object (&input);
          if (g_file_info_get_file_type (archive_file_info) == G_FILE_TYPE_REGULAR)
            {
              if (!fetch_object_if_not_stored (pull_data, checksum,
                                               OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT,
                                               &input, cancellable, error))
                goto out;
            }

          if (!ostree_repo_stage_object (pull_data->repo, OSTREE_OBJECT_TYPE_RAW_FILE, checksum,
                                         archive_file_info, archive_xattrs, input,
                                         cancellable, error))
            goto out;
        }
    }              

  ret = TRUE;
 out:
  return ret;
}

static gboolean
fetch_and_store_tree_metadata_recurse (OtPullData   *pull_data,
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

  if (!fetch_and_store_metadata (pull_data, rev, OSTREE_OBJECT_TYPE_DIR_TREE,
                                 &tree, cancellable, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
  files_variant = g_variant_get_child_value (tree, 2);
  dirs_variant = g_variant_get_child_value (tree, 3);
      
  n = g_variant_n_children (files_variant);
  for (i = 0; i < n; i++)
    {
      const char *filename;
      const char *checksum;

      g_variant_get_child (files_variant, i, "(&s&s)", &filename, &checksum);

      if (!ot_util_filename_validate (filename, error))
        goto out;
      if (!ostree_validate_checksum_string (checksum, error))
        goto out;

      {
        char *duped_key = g_strdup (checksum);
        g_hash_table_replace (pull_data->file_checksums_to_fetch,
                              duped_key, duped_key);
      }
    }
      
  n = g_variant_n_children (dirs_variant);
  for (i = 0; i < n; i++)
    {
      const char *dirname;
      const char *tree_checksum;
      const char *meta_checksum;

      g_variant_get_child (dirs_variant, i, "(&s&s&s)",
                           &dirname, &tree_checksum, &meta_checksum);

      if (!ot_util_filename_validate (dirname, error))
        goto out;
      if (!ostree_validate_checksum_string (tree_checksum, error))
        goto out;
      if (!ostree_validate_checksum_string (meta_checksum, error))
        goto out;

      if (!fetch_and_store_object (pull_data, meta_checksum, OSTREE_OBJECT_TYPE_DIR_META,
                                   cancellable, error))
        goto out;

      if (!fetch_and_store_tree_metadata_recurse (pull_data, tree_checksum, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
fetch_and_store_commit_metadata_recurse (OtPullData   *pull_data,
                                         const char   *rev,
                                         GCancellable *cancellable,
                                         GError      **error)
{
  gboolean ret = FALSE;
  ot_lvariant GVariant *commit = NULL;
  const char *tree_contents_checksum;
  const char *tree_meta_checksum;

  if (!fetch_and_store_metadata (pull_data, rev, OSTREE_OBJECT_TYPE_COMMIT,
                                 &commit, cancellable, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  g_variant_get_child (commit, 6, "&s", &tree_contents_checksum);
  g_variant_get_child (commit, 7, "&s", &tree_meta_checksum);
  
  if (!fetch_and_store_object (pull_data, tree_meta_checksum, OSTREE_OBJECT_TYPE_DIR_META,
                               cancellable, error))
    goto out;
  
  if (!fetch_and_store_tree_metadata_recurse (pull_data, tree_contents_checksum,
                                              cancellable, error))
    goto out;

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

static gboolean
fetch_files (OtPullData           *pull_data,
             GCancellable         *cancellable,
             GError              **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;

  g_hash_table_iter_init (&hash_iter, pull_data->file_checksums_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *checksum = key;

      if (!fetch_and_store_file (pull_data, checksum, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
pull_one_commit (OtPullData       *pull_data,
                 const char       *rev,
                 GCancellable     *cancellable,
                 GError          **error) 
{
  gboolean ret = FALSE;

  if (!ostree_repo_prepare_transaction (pull_data->repo, NULL, error))
    goto out;
  
  if (!fetch_and_store_commit_metadata_recurse (pull_data, rev, cancellable, error))
    goto out;
  
  if (!fetch_files (pull_data, cancellable, error))
    goto out;
  
  if (!ostree_repo_commit_transaction (pull_data->repo, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
pull_one_ref (OtPullData       *pull_data,
              const char       *branch,
              const char       *rev,
              GCancellable     *cancellable,
              GError          **error)
{
  gboolean ret = FALSE;
  ot_lfree char *key = NULL;
  ot_lfree char *remote_ref = NULL;
  ot_lfree char *baseurl = NULL;
  ot_lfree char *original_rev = NULL;

  remote_ref = g_strdup_printf ("%s/%s", pull_data->remote_name, branch);

  if (!ostree_repo_resolve_rev (pull_data->repo, remote_ref, TRUE, &original_rev, error))
    goto out;

  if (original_rev && strcmp (rev, original_rev) == 0)
    {
      g_print ("No changes in %s\n", remote_ref);
    }
  else
    {
      if (!ostree_validate_checksum_string (rev, error))
        goto out;

      if (!pull_one_commit (pull_data, rev, cancellable, error))
        goto out;
      
      if (!ostree_repo_write_ref (pull_data->repo, pull_data->remote_name, branch, rev, error))
        goto out;
      
      g_print ("remote %s is now %s\n", remote_ref, rev);
    }

  ret = TRUE;
 out:
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
ostree_builtin_pull (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  int i;
  GCancellable *cancellable = NULL;
  ot_lobj OstreeRepo *repo = NULL;
  ot_lfree char *path = NULL;
  ot_lfree char *baseurl = NULL;
  ot_lfree char *summary_data = NULL;
  ot_lhash GHashTable *refs_to_fetch = NULL;
  ot_lhash GHashTable *commits_to_fetch = NULL;
  ot_lfree char *branch_rev = NULL;
  OtPullData pull_data_real;
  OtPullData *pull_data = &pull_data_real;
  SoupURI *summary_uri = NULL;
  GKeyFile *config = NULL;
  char **configured_branches = NULL;

  context = g_option_context_new ("REMOTE [BRANCH...] - Download data from remote repository");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  memset (pull_data, 0, sizeof (*pull_data));
  pull_data->repo = repo;
  pull_data->file_checksums_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  if (argc < 2)
    {
      ot_util_usage_error (context, "REMOTE must be specified", error);
      goto out;
    }

  pull_data->stdout_is_tty = isatty (1);

  pull_data->remote_name = g_strdup (argv[1]);
  pull_data->session = soup_session_sync_new_with_options (SOUP_SESSION_USER_AGENT, "ostree ",
                                                           SOUP_SESSION_ADD_FEATURE_BY_TYPE, SOUP_TYPE_COOKIE_JAR,
                                                           NULL);

  config = ostree_repo_get_config (repo);

  key = g_strdup_printf ("remote \"%s\"", pull_data->remote_name);
  baseurl = g_key_file_get_string (config, key, "url", error);
  if (!baseurl)
    goto out;
  pull_data->base_uri = soup_uri_new (baseurl);

  if (!pull_data->base_uri)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to parse url '%s'", baseurl);
      goto out;
    }

  refs_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
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
              g_hash_table_insert (refs_to_fetch, g_strdup (branch), contents);
            }
        }
    }
  else
    {
      GError *temp_error = NULL;
      gboolean fetch_all_refs;

      configured_branches = g_key_file_get_string_list (config, key, "branches", NULL, &temp_error);
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
          
          if (!parse_ref_summary (summary_data, &refs_to_fetch, error))
            goto out;
        }
      else
        {
          char **branches_iter = configured_branches;

          if (!*branches_iter)
            g_print ("No configured branches for remote %s\n", pull_data->remote_name);
          for (;*branches_iter; branches_iter++)
            {
              const char *branch = *branches_iter;
              char *contents;
              
              if (!fetch_ref_contents (pull_data, branch, &contents, cancellable, error))
                goto out;
              
              /* Transfer ownership of contents */
              g_hash_table_insert (refs_to_fetch, g_strdup (branch), contents);
            }
        }
    }

  g_hash_table_iter_init (&hash_iter, refs_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *ref = key;
      const char *sha256 = value;
      
      if (!pull_one_ref (pull_data, ref, sha256, cancellable, error))
        goto out;
    }

  g_hash_table_iter_init (&hash_iter, commits_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *commit = value;
      
      if (!pull_one_commit (pull_data, commit, cancellable, error))
        goto out;
    }

  if (!ostree_repo_clean_cached_remote_pack_data (pull_data->repo, pull_data->remote_name,
                                                  cancellable, error))
    goto out;

  ret = TRUE;
 out:
  g_strfreev (configured_branches);
  if (context)
    g_option_context_free (context);
  g_clear_object (&pull_data->session);
  if (pull_data->base_uri)
    soup_uri_free (pull_data->base_uri);
  if (pull_data->cached_pack_indexes)
    g_ptr_array_unref (pull_data->cached_pack_indexes);
  if (summary_uri)
    soup_uri_free (summary_uri);
  return ret;
}

static OstreeBuiltin builtins[] = {
  { "pull", ostree_builtin_pull, 0 },
  { NULL }
};

int
main (int    argc,
      char **argv)
{
  return ostree_main (argc, argv, builtins);
}
