/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

#include "ot-builtins.h"
#include "ostree.h"

#include <unistd.h>
#include <stdlib.h>

static GOptionEntry options[] = {
  { NULL }
};

typedef struct {
  OstreeRepo *src_repo;
  OstreeRepo *dest_repo;
  gboolean uids_differ;
} OtLocalCloneData;

static gboolean
copy_dir_contents (GFile  *src,
                   GFile  *dest,
                   GCancellable *cancellable,
                   GError   **error)
{
  gboolean ret = FALSE;
  GFile *child_src = NULL;
  GFile *child_dest = NULL;
  GFileEnumerator *dir_enum = NULL;
  GFileInfo *file_info = NULL;
  GError *temp_error = NULL;

  dir_enum = g_file_enumerate_children (src, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, error);
  if (!dir_enum)
    goto out;
  while ((file_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name = g_file_info_get_name (file_info);
      
      g_clear_object (&child_src);
      child_src = g_file_get_child (src, name);
      g_clear_object (&child_dest);
      child_dest = g_file_get_child (dest, name);

      if (!g_file_copy (child_src, child_dest, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS,
                        cancellable, NULL, NULL, error))
        goto out;
      
      g_clear_object (&file_info);
    }
  if (temp_error)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  g_clear_object (&dir_enum);
  g_clear_object (&child_src);
  g_clear_object (&child_dest);
  g_clear_object (&file_info);
  return ret;
}

static void
object_iter_callback (OstreeRepo   *repo,
                      const char   *checksum,
                      OstreeObjectType objtype,
                      GFile        *objfile,
                      GFileInfo    *file_info,
                      gpointer      user_data)
{
  OtLocalCloneData *data = user_data;
  GError *real_error = NULL;
  GError **error = &real_error;
  GFile *content_path = NULL;
  GFileInfo *archive_info = NULL;
  GVariant *archive_metadata = NULL;
  GVariant *xattrs = NULL;
  GInputStream *input = NULL;

  if (objtype == OSTREE_OBJECT_TYPE_RAW_FILE)
    xattrs = ostree_get_xattrs_for_file (objfile, error);
  
  if (objtype == OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT)
    ;
  else if (objtype == OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META)
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META, checksum, &archive_metadata, error))
        goto out;

      if (!ostree_parse_archived_file_meta (archive_metadata, &archive_info, &xattrs, error))
        goto out;

      content_path = ostree_repo_get_object_path (repo, checksum, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT);

      if (g_file_info_get_file_type (archive_info) == G_FILE_TYPE_REGULAR)
        {
          input = (GInputStream*)g_file_read (content_path, NULL, error);
          if (!input)
            goto out;
        }
      
      if (!ostree_repo_stage_object_trusted (data->dest_repo, OSTREE_OBJECT_TYPE_RAW_FILE, checksum,
                                             archive_info, xattrs, input,
                                             NULL, error))
        goto out;
    }
  else
    {
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
        {
          input = (GInputStream*)g_file_read (objfile, NULL, error);
          if (!input)
            goto out;
        }

      if (!ostree_repo_stage_object_trusted (data->dest_repo, objtype, checksum,
                                             file_info, xattrs, input,
                                             NULL, error))
        goto out;
    }

 out:
  ot_clear_gvariant (&archive_metadata);
  ot_clear_gvariant (&xattrs);
  g_clear_object (&archive_info);
  g_clear_object (&input);
  g_clear_object (&content_path);
  if (real_error != NULL)
    {
      g_printerr ("%s\n", real_error->message);
      g_clear_error (error);
      exit (1);
    }
}

gboolean
ostree_builtin_local_clone (int argc, char **argv, const char *repo_path, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  const char *destination;
  OtLocalCloneData data;
  GFile *src_repo_dir = NULL;
  GFile *dest_repo_dir = NULL;
  GFileInfo *src_info = NULL;
  GFileInfo *dest_info = NULL;
  GFile *src_dir = NULL;
  GFile *dest_dir = NULL;

  context = g_option_context_new ("DEST ... - Create new repository DEST");
  g_option_context_add_main_entries (context, options, NULL);

  memset (&data, 0, sizeof (data));

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  data.src_repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (data.src_repo, error))
    goto out;

  if (argc < 1)
    {
      gchar *help = g_option_context_get_help (context, TRUE, NULL);
      g_printerr ("%s\n", help);
      g_free (help);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "DESTINATION must be specified");
      goto out;
    }

  destination = argv[1];

  data.dest_repo = ostree_repo_new (destination);
  if (!ostree_repo_check (data.dest_repo, error))
    goto out;

  src_repo_dir = ot_gfile_new_for_path (ostree_repo_get_path (data.src_repo));
  dest_repo_dir = ot_gfile_new_for_path (ostree_repo_get_path (data.dest_repo));

  src_info = g_file_query_info (src_repo_dir, OSTREE_GIO_FAST_QUERYINFO,
                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                NULL, error);
  if (!src_info)
    goto out;
  dest_info = g_file_query_info (dest_repo_dir, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 NULL, error);
  if (!dest_info)
    goto out;

  data.uids_differ = g_file_info_get_attribute_uint32 (src_info, "unix::uid") != g_file_info_get_attribute_uint32 (dest_info, "unix::uid");

  if (!ostree_repo_prepare_transaction (data.dest_repo, NULL, error))
    goto out;

  if (!ostree_repo_iter_objects (data.src_repo, object_iter_callback, &data, error))
    goto out;

  if (!ostree_repo_commit_transaction (data.dest_repo, NULL, error))
    goto out;
  
  src_dir = g_file_resolve_relative_path (src_repo_dir, "refs/heads");
  dest_dir = g_file_resolve_relative_path (dest_repo_dir, "refs/heads");
  if (!copy_dir_contents (src_dir, dest_dir, NULL, error))
    goto out;
  g_clear_object (&src_dir);
  g_clear_object (&dest_dir);

  src_dir = g_file_resolve_relative_path (src_repo_dir, "tags");
  dest_dir = g_file_resolve_relative_path (dest_repo_dir, "tags");
  if (!copy_dir_contents (src_dir, dest_dir, NULL, error))
    goto out;

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  g_clear_object (&src_repo_dir);
  g_clear_object (&dest_repo_dir);
  g_clear_object (&src_info);
  g_clear_object (&dest_info);
  g_clear_object (&src_dir);
  g_clear_object (&dest_dir);
  g_clear_object (&data.src_repo);
  g_clear_object (&data.dest_repo);
  return ret;
}
