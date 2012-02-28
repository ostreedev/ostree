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
fetch_uri (OstreeRepo  *repo,
           SoupSession *soup,
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

  if (!ostree_create_temp_regular_file (ostree_repo_get_tmpdir (repo),
                                        tmp_prefix, NULL,
                                        &ret_temp_filename,
                                        &output_stream,
                                        NULL, error))
    goto out;

  chunkdata.session = soup;
  chunkdata.stream = output_stream;
  chunkdata.had_error = FALSE;
  chunkdata.error = error;
  
  uri_string = soup_uri_to_string (uri, FALSE);
  g_print ("Fetching %s\n", uri_string);
  msg = soup_message_new_from_uri (SOUP_METHOD_GET, uri);

  soup_message_body_set_accumulate (msg->response_body, FALSE);

  g_signal_connect (msg, "got-chunk", G_CALLBACK (on_got_chunk), &chunkdata);
  
  response = soup_session_send_message (soup, msg);
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
fetch_uri_contents_utf8 (OstreeRepo  *repo,
                         SoupSession *soup,
                         SoupURI     *uri,
                         char       **out_contents,
                         GCancellable  *cancellable,
                         GError     **error)
{
  gboolean ret = FALSE;
  GFile *tmpf = NULL;
  char *ret_contents = NULL;
  gsize len;

  if (!fetch_uri (repo, soup, uri, "tmp-", &tmpf, cancellable, error))
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
fetch_object (OstreeRepo  *repo,
              SoupSession *soup,
              SoupURI     *baseuri,
              const char  *checksum,
              OstreeObjectType objtype,
              GFile           **out_temp_path,
              GCancellable *cancellable,
              GError     **error)
{
  gboolean ret = FALSE;
  char *objpath = NULL;
  char *relpath = NULL;
  SoupURI *obj_uri = NULL;
  GFile *ret_temp_path = NULL;

  objpath = ostree_get_relative_object_path (checksum, objtype);
  obj_uri = soup_uri_copy (baseuri);
  relpath = g_build_filename (soup_uri_get_path (obj_uri), objpath, NULL);
  soup_uri_set_path (obj_uri, relpath);
  
  if (!fetch_uri (repo, soup, obj_uri, ostree_object_type_to_string (objtype), &ret_temp_path,
                  cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_temp_path, &ret_temp_path);
 out:
  if (obj_uri)
    soup_uri_free (obj_uri);
  g_clear_object (&ret_temp_path);
  g_free (objpath);
  g_free (relpath);
  return ret;
}

static gboolean
fetch_and_store_object (OstreeRepo  *repo,
                        SoupSession *soup,
                        SoupURI     *baseuri,
                        const char  *checksum,
                        OstreeObjectType objtype,
                        gboolean         *out_is_pending,
                        GVariant        **out_metadata,
                        GCancellable *cancellable,
                        GError     **error)
{
  gboolean ret = FALSE;
  GFileInfo *file_info = NULL;
  GInputStream *input = NULL;
  GFile *stored_path = NULL;
  GFile *pending_path = NULL;
  GFile *temp_path = NULL;
  GVariant *ret_metadata = NULL;
  gboolean ret_is_pending;

  g_assert (objtype != OSTREE_OBJECT_TYPE_RAW_FILE);

  if (!ostree_repo_find_object (repo, objtype, checksum,
                                &stored_path, &pending_path, NULL, error))
    goto out;
      
  if (!(stored_path || pending_path))
    {
      if (!fetch_object (repo, soup, baseuri, checksum, objtype, &temp_path, cancellable, error))
        goto out;
    }

  if (temp_path)
    {
      file_info = g_file_query_info (temp_path, OSTREE_GIO_FAST_QUERYINFO,
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
      if (!file_info)
        goto out;
      
      input = (GInputStream*)g_file_read (temp_path, cancellable, error);
      if (!input)
        goto out;
    }
  
  if (pending_path || temp_path)
    {
      if (!ostree_repo_stage_object (repo, objtype, checksum, file_info, NULL, input, cancellable, error))
        goto out;

      log_verbose ("Staged object: %s.%s", checksum, ostree_object_type_to_string (objtype));

      ret_is_pending = TRUE;
      if (out_metadata)
        {
          if (!ostree_map_metadata_file (pending_path ? pending_path : temp_path, objtype, &ret_metadata, error))
            goto out;
        }
    }
  else
    {
      ret_is_pending = FALSE;
    }

  ret = TRUE;
  if (out_is_pending)
    *out_is_pending = ret_is_pending;
  ot_transfer_out_value (out_metadata, &ret_metadata);
 out:
  if (temp_path)
    (void) unlink (ot_gfile_get_path_cached (temp_path));
  ot_clear_gvariant (&ret_metadata);
  g_clear_object (&temp_path);
  g_clear_object (&file_info);
  g_clear_object (&input);
  g_clear_object (&stored_path);
  g_clear_object (&pending_path);
  return ret;
}

static gboolean
fetch_and_store_tree_recurse (OstreeRepo   *repo,
                              SoupSession  *soup,
                              SoupURI      *base_uri,
                              const char   *rev,
                              GCancellable *cancellable,
                              GError      **error)
{
  gboolean ret = FALSE;
  GVariant *tree = NULL;
  GVariant *files_variant = NULL;
  GVariant *dirs_variant = NULL;
  gboolean is_pending;
  int i, n;
  GVariant *archive_metadata = NULL;
  GFileInfo *archive_file_info = NULL;
  GVariant *archive_xattrs = NULL;
  GFile *meta_temp_path = NULL;
  GFile *content_temp_path = NULL;
  GFile *stored_path = NULL;
  GFile *pending_path = NULL;
  GInputStream *input = NULL;

  if (!fetch_and_store_object (repo, soup, base_uri, rev, OSTREE_OBJECT_TYPE_DIR_TREE,
                               &is_pending, &tree, cancellable, error))
    goto out;

  if (!is_pending)
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

          g_clear_object (&stored_path);
          g_clear_object (&pending_path);
          /* If we're fetching from an archive into a bare repository, we need
           * to explicitly check for raw file types locally.
           */
          if (ostree_repo_get_mode (repo) == OSTREE_REPO_MODE_BARE)
            {
              if (!ostree_repo_find_object (repo, OSTREE_OBJECT_TYPE_RAW_FILE, checksum,
                                            &stored_path, &pending_path, cancellable, error))
                goto out;
            }
          else
            {
              if (!ostree_repo_find_object (repo, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT, checksum,
                                            &stored_path, &pending_path, cancellable, error))
                goto out;
            }

          g_clear_object (&input);
          g_clear_object (&archive_file_info);
          ot_clear_gvariant (&archive_xattrs);
          if (!(stored_path || pending_path))
            {
              g_clear_object (&meta_temp_path);
              if (!fetch_object (repo, soup, base_uri, checksum,
                                 OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META,
                                 &meta_temp_path,
                                 cancellable,
                                 error))
                goto out;

              if (!ostree_map_metadata_file (meta_temp_path,
                                             OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META,
                                             &archive_metadata, error))
                goto out;

              if (!ostree_parse_archived_file_meta (archive_metadata, &archive_file_info, &archive_xattrs, error))
                goto out;

              if (g_file_info_get_file_type (archive_file_info) == G_FILE_TYPE_REGULAR)
                {
                  if (!fetch_object (repo, soup, base_uri, checksum,
                                     OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT,
                                     &content_temp_path,
                                     cancellable,
                                     error))
                    goto out;
                  
                  input = (GInputStream*)g_file_read (content_temp_path, cancellable, error);
                  if (!input)
                    goto out;
                }
            }

          if (!stored_path)
            {
              log_verbose ("Staged file object: %s", checksum);

              if (!ostree_repo_stage_object (repo, OSTREE_OBJECT_TYPE_RAW_FILE,
                                             checksum,
                                             archive_file_info, archive_xattrs, input,
                                             cancellable, error))
                goto out;
            }
              
          if (meta_temp_path)
            {
              (void) unlink (ot_gfile_get_path_cached (meta_temp_path));
              g_clear_object (&meta_temp_path);
            }
          if (content_temp_path)
            {
              (void) unlink (ot_gfile_get_path_cached (content_temp_path));
              g_clear_object (&content_temp_path);
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

          if (!fetch_and_store_object (repo, soup, base_uri, meta_checksum, OSTREE_OBJECT_TYPE_DIR_META,
                                       NULL, NULL, cancellable, error))
            goto out;

          if (!fetch_and_store_tree_recurse (repo, soup, base_uri, tree_checksum, cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  ot_clear_gvariant (&tree);
  ot_clear_gvariant (&files_variant);
  ot_clear_gvariant (&dirs_variant);
  ot_clear_gvariant (&archive_metadata);
  ot_clear_gvariant (&archive_xattrs);
  g_clear_object (&archive_file_info);
  g_clear_object (&input);
  g_clear_object (&stored_path);
  g_clear_object (&pending_path);
  if (content_temp_path)
    {
      (void) unlink (ot_gfile_get_path_cached (content_temp_path));
      g_clear_object (&content_temp_path);
    }
  if (meta_temp_path)
    {
      (void) unlink (ot_gfile_get_path_cached (meta_temp_path));
      g_clear_object (&meta_temp_path);
    }
  return ret;
}

static gboolean
fetch_and_store_commit_recurse (OstreeRepo   *repo,
                                SoupSession  *soup,
                                SoupURI      *base_uri,
                                const char   *rev,
                                GCancellable *cancellable,
                                GError      **error)
{
  gboolean ret = FALSE;
  GVariant *commit = NULL;
  const char *tree_contents_checksum;
  const char *tree_meta_checksum;
  gboolean is_pending;

  if (!fetch_and_store_object (repo, soup, base_uri, rev, OSTREE_OBJECT_TYPE_COMMIT,
                               &is_pending, &commit, cancellable, error))
    goto out;

  if (!is_pending)
    log_verbose ("Already have commit %s", rev);
  else
    {
      /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
      g_variant_get_child (commit, 6, "&s", &tree_contents_checksum);
      g_variant_get_child (commit, 7, "&s", &tree_meta_checksum);
      
      if (!fetch_and_store_object (repo, soup, base_uri, tree_meta_checksum, OSTREE_OBJECT_TYPE_DIR_META,
                                   NULL, NULL, cancellable, error))
        goto out;
      
      if (!fetch_and_store_tree_recurse (repo, soup, base_uri, tree_contents_checksum,
                                         cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  ot_clear_gvariant (&commit);
  return ret;
}

static gboolean
fetch_ref_contents (OstreeRepo    *repo,
                    SoupSession   *soup,
                    SoupURI       *base_uri,
                    const char    *ref,
                    char         **out_contents,
                    GCancellable  *cancellable,
                    GError       **error)
{
  gboolean ret = FALSE;
  char *ret_contents = NULL;
  char *refpath = NULL;
  SoupURI *target_uri = NULL;

  target_uri = soup_uri_copy (base_uri);
  refpath = g_build_filename (soup_uri_get_path (target_uri), "refs", "heads", ref, NULL);
  soup_uri_set_path (target_uri, refpath);
  
  if (!fetch_uri_contents_utf8 (repo, soup, target_uri, &ret_contents, cancellable, error))
    goto out;

  g_strchomp (ret_contents);

  if (!ostree_validate_checksum_string (ret_contents, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_contents, &ret_contents);
 out:
  g_free (refpath);
  g_free (ret_contents);
  if (target_uri)
    soup_uri_free (target_uri);
  return ret;
}

static gboolean
pull_one_commit (OstreeRepo       *repo,
                 const char       *remote,
                 const char       *branch,
                 const char       *rev,
                 SoupSession      *soup,
                 SoupURI          *base_uri,
                 GCancellable     *cancellable,
                 GError          **error)
{
  gboolean ret = FALSE;
  char *key = NULL;
  char *remote_ref = NULL;
  char *baseurl = NULL;
  char *original_rev = NULL;

  remote_ref = g_strdup_printf ("%s/%s", remote, branch);

  if (!ostree_repo_resolve_rev (repo, remote_ref, TRUE, &original_rev, error))
    goto out;

  if (original_rev && strcmp (rev, original_rev) == 0)
    {
      g_print ("No changes in %s\n", remote_ref);
    }
  else
    {
      if (!ostree_validate_checksum_string (rev, error))
        goto out;

      if (!ostree_repo_prepare_transaction (repo, NULL, error))
        goto out;
      
      if (!fetch_and_store_commit_recurse (repo, soup, base_uri, rev, cancellable, error))
        goto out;

      if (!ostree_repo_commit_transaction (repo, cancellable, error))
        goto out;
      
      if (!ostree_repo_write_ref (repo, remote, branch, rev, error))
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
  OstreeRepo *repo = NULL;
  const char *remote;
  SoupSession *soup = NULL;
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

  if (argc < 2)
    {
      ot_util_usage_error (context, "REMOTE must be specified", error);
      goto out;
    }

  remote = argv[1];

  soup = soup_session_sync_new_with_options (SOUP_SESSION_USER_AGENT, "ostree ",
                                             SOUP_SESSION_ADD_FEATURE_BY_TYPE, SOUP_TYPE_COOKIE_JAR,
                                             NULL);

  config = ostree_repo_get_config (repo);

  key = g_strdup_printf ("remote \"%s\"", remote);
  baseurl = g_key_file_get_string (config, key, "url", error);
  if (!baseurl)
    goto out;
  base_uri = soup_uri_new (baseurl);

  if (!base_uri)
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
          
          if (!fetch_ref_contents (repo, soup, base_uri, branch, &contents, cancellable, error))
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

      if (!fetch_uri_contents_utf8 (repo, soup, summary_uri, &summary_data, cancellable, error))
        goto out;

      if (!parse_ref_summary (summary_data, &refs_to_fetch, error))
        goto out;
    }

  g_hash_table_iter_init (&hash_iter, refs_to_fetch);

  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *ref = key;
      const char *sha256 = value;
      
      if (!pull_one_commit (repo, remote, ref, sha256, soup, base_uri, cancellable, error))
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
  g_clear_object (&soup);
  if (base_uri)
    soup_uri_free (base_uri);
  if (summary_uri)
    soup_uri_free (summary_uri);
  g_clear_object (&repo);
  g_clear_object (&soup);
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
