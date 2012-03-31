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
  SoupSession    *session;
  GOutputStream  *stream;
  gboolean        had_error;
  GError        **error;
} OstreeSoupChunkData;

static void
on_got_chunk (SoupMessage   *msg,
              SoupBuffer    *buf,
              gpointer       user_data)
{
  OstreeSoupChunkData *data = user_data;
  gsize bytes_written;
  
  if (!g_output_stream_write_all (data->stream, buf->data, buf->length,
                                  &bytes_written, NULL, data->error))
    {
      data->had_error = TRUE;
      soup_session_cancel_message (data->session, msg, 500);
    }
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
  SoupMessage *msg = NULL;
  guint response;
  char *uri_string = NULL;
  GFile *ret_temp_filename = NULL;
  GOutputStream *output_stream = NULL;
  OstreeSoupChunkData chunkdata;

  if (!ostree_create_temp_regular_file (ostree_repo_get_tmpdir (pull_data->repo),
                                        tmp_prefix, NULL,
                                        &ret_temp_filename,
                                        &output_stream,
                                        NULL, error))
    goto out;

  chunkdata.session = pull_data->session;
  chunkdata.stream = output_stream;
  chunkdata.had_error = FALSE;
  chunkdata.error = error;
  
  uri_string = soup_uri_to_string (uri, FALSE);
  g_print ("Fetching %s\n", uri_string);
  msg = soup_message_new_from_uri (SOUP_METHOD_GET, uri);

  soup_message_body_set_accumulate (msg->response_body, FALSE);

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
  
  ret = TRUE;
  ot_transfer_out_value (out_temp_filename, &ret_temp_filename);
 out:
  if (ret_temp_filename)
    (void) unlink (ot_gfile_get_path_cached (ret_temp_filename));
  g_clear_object (&ret_temp_filename);
  g_free (uri_string);
  g_clear_object (&msg);
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
  GFile *tmpf = NULL;
  char *ret_contents = NULL;
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
  g_clear_object (&tmpf);
  g_free (ret_contents);
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
  GFile *ret_cached_path = NULL;
  GFile *tmp_path = NULL;
  char *pack_name = NULL;
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
  g_clear_object (&ret_cached_path);
  g_clear_object (&tmp_path);
  g_free (pack_name);
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
  GVariant *mapped_pack = NULL;
  GVariant *csum_bytes = NULL;
  char *ret_pack_checksum = NULL;
  guint64 offset;
  guint i;

  csum_bytes = ostree_checksum_to_bytes (checksum);

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
  ot_clear_gvariant (&mapped_pack);
  g_free (ret_pack_checksum);
  ot_clear_gvariant (&csum_bytes);
  return ret;
}

static gboolean
fetch_one_cache_index (OtPullData          *pull_data,
                      const char           *pack_checksum,
                      GCancellable         *cancellable,
                      GError              **error)
{
  gboolean ret = FALSE;
  SoupURI *index_uri = NULL;
  GFile *tmp_path = NULL;
  char *pack_index_name = NULL;

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
  g_clear_object (&tmp_path);
  g_free (pack_index_name);
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
  SoupURI *superindex_uri = NULL;
  GFile *superindex_tmppath = NULL;
  GPtrArray *cached_indexes = NULL;
  GPtrArray *uncached_indexes = NULL;
  GVariant *superindex_variant = NULL;
  GVariantIter *contents_iter = NULL;
  guint i;

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
  g_clear_object (&superindex_tmppath);
  ot_clear_gvariant (&superindex_variant);
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
  char *objpath = NULL;
  SoupURI *obj_uri = NULL;
  GFile *ret_temp_path = NULL;

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
  g_clear_object (&ret_temp_path);
  g_free (objpath);
  return ret;
}

static gboolean
find_object (OtPullData        *pull_data,
             const char        *checksum,
             OstreeObjectType   objtype,
             gboolean          *out_is_stored,
             gboolean          *out_is_pending,
             char             **out_remote_pack_checksum,
             guint64           *out_offset,
             GCancellable      *cancellable,
             GError           **error)
{
  gboolean ret = FALSE;
  gboolean ret_is_stored;
  gboolean ret_is_pending;
  GFile *stored_path = NULL;
  GFile *pending_path = NULL;
  char *local_pack_checksum = NULL;
  char *ret_remote_pack_checksum = NULL;
  guint64 offset;

  if (!ostree_repo_find_object (pull_data->repo, objtype, checksum,
                                &stored_path, &pending_path,
                                &local_pack_checksum, NULL,
                                cancellable, error))
    goto out;

  ret_is_stored = (stored_path != NULL || local_pack_checksum != NULL);
  ret_is_pending = pending_path != NULL;

  if (!(ret_is_stored || ret_is_pending))
    {
      if (!find_object_in_remote_packs (pull_data, checksum, objtype, 
                                        &ret_remote_pack_checksum, &offset,
                                        cancellable, error))
        goto out;
    }

  ret = TRUE;
  if (out_is_stored)
    *out_is_stored = ret_is_stored;
  if (out_is_pending)
    *out_is_pending = ret_is_pending;
  ot_transfer_out_value (out_remote_pack_checksum, &ret_remote_pack_checksum);
  if (out_offset)
    *out_offset = offset;
 out:
  g_free (local_pack_checksum);
  g_free (ret_remote_pack_checksum);
  g_clear_object (&stored_path);
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
                            gboolean             *out_is_stored,
                            gboolean             *out_is_pending,
                            GInputStream        **out_input,
                            GCancellable         *cancellable,
                            GError              **error)
{
  gboolean ret = FALSE;
  gboolean ret_is_stored = FALSE;
  gboolean ret_is_pending = FALSE;
  GInputStream *ret_input = NULL;
  GFile *temp_path = NULL;
  GFile *pack_path = NULL;
  GMappedFile *pack_map = NULL;
  char *remote_pack_checksum = NULL;
  guint64 pack_offset = 0;
  GVariant *pack_entry = NULL;

  if (!find_object (pull_data, checksum, objtype, &ret_is_stored,
                    &ret_is_pending, &remote_pack_checksum,
                    &pack_offset, cancellable, error))
    goto out;
      
  if (remote_pack_checksum != NULL)
    {
      g_assert (!(ret_is_stored || ret_is_pending));

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

      ret_input = ostree_read_pack_entry_as_stream (pack_entry);
      g_object_set_data_full ((GObject*)ret_input, "ostree-pull-pack-map",
                              pack_map, (GDestroyNotify) g_mapped_file_unref);
      pack_map = NULL; /* Transfer ownership */
    }
  else if (!(ret_is_stored || ret_is_pending))
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
  if (out_is_stored)
    *out_is_stored = ret_is_stored;
  if (out_is_pending)
    *out_is_pending = ret_is_pending;
 out:
  g_clear_object (&temp_path);
  g_clear_object (&pack_path);
  if (pack_map)
    g_mapped_file_unref (pack_map);
  ot_clear_gvariant (&pack_entry);
  g_clear_object (&pack_path);
  g_clear_object (&ret_input);
  return ret;
}

static gboolean
fetch_and_store_object (OtPullData       *pull_data,
                        const char       *checksum,
                        OstreeObjectType objtype,
                        gboolean         *out_was_stored,
                        GCancellable     *cancellable,
                        GError          **error)
{
  gboolean ret = FALSE;
  GFileInfo *file_info = NULL;
  GInputStream *input = NULL;
  GFile *stored_path = NULL;
  GFile *pending_path = NULL;
  char *pack_checksum = NULL;
  gboolean is_stored;
  gboolean is_pending;

  g_assert (objtype != OSTREE_OBJECT_TYPE_RAW_FILE);

  if (!fetch_object_if_not_stored (pull_data, checksum, objtype,
                                   &is_stored, &is_pending, &input,
                                   cancellable, error))
    goto out;

  if (is_pending || input)
    {
      if (!ostree_repo_stage_object (pull_data->repo, objtype, checksum, NULL, NULL,
                                     input, cancellable, error))
        goto out;

      log_verbose ("Staged object: %s.%s", checksum, ostree_object_type_to_string (objtype));
    }

  ret = TRUE;
  if (out_was_stored)
    *out_was_stored = is_stored;
 out:
  g_clear_object (&file_info);
  g_clear_object (&input);
  g_clear_object (&stored_path);
  g_clear_object (&pending_path);
  g_free (pack_checksum);
  return ret;
}

static gboolean
fetch_and_store_metadata (OtPullData          *pull_data,
                          const char          *checksum,
                          OstreeObjectType     objtype,
                          gboolean            *out_was_stored,
                          GVariant           **out_variant,
                          GCancellable        *cancellable,
                          GError             **error)
{
  gboolean ret = FALSE;
  gboolean ret_was_stored;
  GVariant *ret_variant = NULL;

  if (!fetch_and_store_object (pull_data, checksum, objtype,
                               &ret_was_stored, cancellable, error))
    goto out;

  if (!ostree_repo_load_variant (pull_data->repo, objtype, checksum,
                                 &ret_variant, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_variant, &ret_variant);
  if (out_was_stored)
    *out_was_stored = ret_was_stored;
 out:
  ot_clear_gvariant (&ret_variant);
  return ret;
}

static gboolean
fetch_and_store_file (OtPullData          *pull_data,
                      const char          *checksum,
                      GCancellable        *cancellable,
                      GError             **error)
{
  gboolean ret = FALSE;
  GInputStream *input = NULL;
  GFile *stored_path = NULL;
  GFile *pending_path = NULL;
  char *pack_checksum = NULL;
  GVariant *archive_metadata_container = NULL;
  GVariant *archive_metadata = NULL;
  GFileInfo *archive_file_info = NULL;
  GVariant *archive_xattrs = NULL;
  gboolean skip_archive_fetch;

  /* If we're fetching from an archive into a bare repository, we need
   * to explicitly check for raw file types locally.
   */
  if (ostree_repo_get_mode (pull_data->repo) == OSTREE_REPO_MODE_BARE)
    {
      if (!ostree_repo_find_object (pull_data->repo, OSTREE_OBJECT_TYPE_RAW_FILE,
                                    checksum, &stored_path, &pending_path, &pack_checksum,
                                    NULL, cancellable, error))
        goto out;
      
      if (stored_path || pack_checksum)
        skip_archive_fetch = TRUE;
      else if (pending_path != NULL)
        {
          skip_archive_fetch = TRUE;
          if (!ostree_repo_stage_object (pull_data->repo, OSTREE_OBJECT_TYPE_RAW_FILE,
                                         checksum, NULL, NULL, NULL, cancellable, error))
            goto out;
        }
      else
        skip_archive_fetch = FALSE;
      
      g_clear_object (&stored_path);
    }
  else
    {
      skip_archive_fetch = FALSE;
    }

  if (!skip_archive_fetch)
    {
      if (!fetch_object_if_not_stored (pull_data, checksum,
                                       OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META,
                                       NULL, NULL, &input, cancellable, error))
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
                                               NULL, NULL, &input,
                                               cancellable, error))
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
  g_free (pack_checksum);
  g_clear_object (&stored_path);
  g_clear_object (&pending_path);
  g_clear_object (&input);
  ot_clear_gvariant (&archive_metadata_container);
  ot_clear_gvariant (&archive_metadata);
  ot_clear_gvariant (&archive_xattrs);
  g_clear_object (&archive_file_info);
  return ret;
}

static gboolean
fetch_and_store_tree_recurse (OtPullData   *pull_data,
                              const char   *rev,
                              GCancellable *cancellable,
                              GError      **error)
{
  gboolean ret = FALSE;
  GVariant *tree = NULL;
  GVariant *files_variant = NULL;
  GVariant *dirs_variant = NULL;
  gboolean was_stored;
  int i, n;
  GFile *stored_path = NULL;
  GFile *pending_path = NULL;
  char *pack_checksum = NULL;

  if (!fetch_and_store_metadata (pull_data, rev, OSTREE_OBJECT_TYPE_DIR_TREE,
                                 &was_stored, &tree, cancellable, error))
    goto out;

  if (was_stored)
    log_verbose ("Already have tree %s", rev);
  else
    {
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

          if (!fetch_and_store_file (pull_data, checksum, cancellable, error))
            goto out;
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
                                       NULL, cancellable, error))
            goto out;

          if (!fetch_and_store_tree_recurse (pull_data, tree_checksum, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  ot_clear_gvariant (&tree);
  ot_clear_gvariant (&files_variant);
  ot_clear_gvariant (&dirs_variant);
  g_clear_object (&stored_path);
  g_clear_object (&pending_path);
  g_free (pack_checksum);
  return ret;
}

static gboolean
fetch_and_store_commit_recurse (OtPullData   *pull_data,
                                const char   *rev,
                                GCancellable *cancellable,
                                GError      **error)
{
  gboolean ret = FALSE;
  GVariant *commit = NULL;
  const char *tree_contents_checksum;
  const char *tree_meta_checksum;
  gboolean was_stored;

  if (!fetch_and_store_metadata (pull_data, rev, OSTREE_OBJECT_TYPE_COMMIT,
                                 &was_stored, &commit, cancellable, error))
    goto out;

  if (was_stored)
    log_verbose ("Already have commit %s", rev);
  else
    {
      /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
      g_variant_get_child (commit, 6, "&s", &tree_contents_checksum);
      g_variant_get_child (commit, 7, "&s", &tree_meta_checksum);
      
      if (!fetch_and_store_object (pull_data, tree_meta_checksum, OSTREE_OBJECT_TYPE_DIR_META,
                                   NULL, cancellable, error))
        goto out;
      
      if (!fetch_and_store_tree_recurse (pull_data, tree_contents_checksum,
                                         cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  ot_clear_gvariant (&commit);
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
  char *ret_contents = NULL;
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
  g_free (ret_contents);
  if (target_uri)
    soup_uri_free (target_uri);
  return ret;
}

static gboolean
pull_one_commit (OtPullData       *pull_data,
                 const char       *branch,
                 const char       *rev,
                 GCancellable     *cancellable,
                 GError          **error)
{
  gboolean ret = FALSE;
  char *key = NULL;
  char *remote_ref = NULL;
  char *baseurl = NULL;
  char *original_rev = NULL;

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

      if (!pull_data->fetched_packs)
        {
          pull_data->fetched_packs = TRUE;
          pull_data->cached_pack_indexes = g_ptr_array_new_with_free_func (g_free);

          g_print ("Fetching packs\n");

          if (!fetch_and_cache_pack_indexes (pull_data, cancellable, error))
            goto out;
        }

      if (!ostree_repo_prepare_transaction (pull_data->repo, NULL, error))
        goto out;
      
      if (!fetch_and_store_commit_recurse (pull_data, rev, cancellable, error))
        goto out;

      if (!ostree_repo_commit_transaction (pull_data->repo, cancellable, error))
        goto out;
      
      if (!ostree_repo_write_ref (pull_data->repo, pull_data->remote_name, branch, rev, error))
        goto out;
      
      g_print ("remote %s is now %s\n", remote_ref, rev);
    }

  ret = TRUE;
 out:
  g_free (key);
  g_free (remote_ref);
  g_free (baseurl);
  g_free (original_rev);
  return ret;
}

static gboolean
parse_ref_summary (const char    *contents,
                   GHashTable   **out_refs,
                   GError       **error)
{
  gboolean ret = FALSE;
  GHashTable *ret_refs = NULL;
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
  if (ret_refs)
    g_hash_table_unref (ret_refs);
  g_strfreev (lines);
  return ret;
}
                      
static gboolean
ostree_builtin_pull (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  OtPullData pull_data_real;
  OtPullData *pull_data = &pull_data_real;
  OstreeRepo *repo = NULL;
  char *path = NULL;
  char *baseurl = NULL;
  char *summary_data = NULL;
  SoupURI *base_uri = NULL;
  SoupURI *summary_uri = NULL;
  GKeyFile *config = NULL;
  GCancellable *cancellable = NULL;
  GHashTable *refs_to_fetch = NULL;
  GHashTableIter hash_iter;
  gpointer key, value;
  char *branch_rev = NULL;
  int i;

  context = g_option_context_new ("REMOTE [BRANCH...] - Download data from remote repository");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  memset (pull_data, 0, sizeof (*pull_data));
  pull_data->repo = repo;

  if (argc < 2)
    {
      ot_util_usage_error (context, "REMOTE must be specified", error);
      goto out;
    }

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

  if (argc > 2)
    {
      refs_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      for (i = 2; i < argc; i++)
        {
          const char *branch = argv[i];
          char *contents;
          
          if (!fetch_ref_contents (pull_data, branch, &contents, cancellable, error))
            goto out;
      
          /* Transfer ownership of contents */
          g_hash_table_insert (refs_to_fetch, g_strdup (branch), contents);
        }
    }
  else
    {
      summary_uri = soup_uri_copy (base_uri);
      path = g_build_filename (soup_uri_get_path (summary_uri), "refs", "summary", NULL);
      soup_uri_set_path (summary_uri, path);

      if (!fetch_uri_contents_utf8 (pull_data, summary_uri, &summary_data, cancellable, error))
        goto out;

      if (!parse_ref_summary (summary_data, &refs_to_fetch, error))
        goto out;
    }

  g_hash_table_iter_init (&hash_iter, refs_to_fetch);

  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *ref = key;
      const char *sha256 = value;
      
      if (!pull_one_commit (pull_data, ref, sha256, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  if (refs_to_fetch)
    g_hash_table_unref (refs_to_fetch);
  g_free (path);
  g_free (baseurl);
  g_free (summary_data);
  g_free (branch_rev);
  if (context)
    g_option_context_free (context);
  g_clear_object (&pull_data->session);
  if (pull_data->base_uri)
    soup_uri_free (pull_data->base_uri);
  if (pull_data->cached_pack_indexes)
    g_ptr_array_unref (pull_data->cached_pack_indexes);
  if (summary_uri)
    soup_uri_free (summary_uri);
  g_clear_object (&repo);
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
