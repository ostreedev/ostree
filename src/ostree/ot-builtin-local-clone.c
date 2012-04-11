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
copy_dir_contents_recurse (GFile  *src,
                           GFile  *dest,
                           GCancellable *cancellable,
                           GError   **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  ot_lobj GFile *child_src = NULL;
  ot_lobj GFile *child_dest = NULL;
  ot_lobj GFileEnumerator *dir_enum = NULL;
  ot_lobj GFileInfo *file_info = NULL;

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

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!ot_gfile_ensure_directory (child_dest, FALSE, error))
            goto out;
          
          if (!copy_dir_contents_recurse (child_src, child_dest, cancellable, error))
            goto out;
        }
      else
        {
          if (!g_file_copy (child_src, child_dest, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS,
                            cancellable, NULL, NULL, error))
            goto out;
        }
      
      g_clear_object (&file_info);
    }
  if (temp_error)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
import_one_object (OtLocalCloneData *data,
                   const char   *checksum,
                   OstreeObjectType objtype,
                   GCancellable  *cancellable,
                   GError        **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *objfile = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GFile *content_path = NULL;
  ot_lobj GFileInfo *archive_info = NULL;
  ot_lvariant GVariant *metadata = NULL;
  ot_lvariant GVariant *xattrs = NULL;
  ot_lobj GInputStream *input = NULL;

  objfile = ostree_repo_get_object_path (data->src_repo, checksum, objtype);
  file_info = g_file_query_info (objfile, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);

  if (file_info == NULL)
    goto out;

  if (objtype == OSTREE_OBJECT_TYPE_FILE)
    {
      if (!ostree_repo_load_file (data->src_repo, checksum,
                                  &input, &file_info, &xattrs,
                                  cancellable, error))
        goto out;

      if (!ostree_repo_stage_object_trusted (data->dest_repo, OSTREE_OBJECT_TYPE_FILE,
                                             checksum, FALSE, file_info, xattrs, input,
                                             cancellable, error))
        goto out;
    }
  else
    {
      if (!ostree_repo_load_variant (data->src_repo, objtype, checksum, &metadata,
                                     error))
        goto out;

      input = ot_variant_read (metadata);

      if (!ostree_repo_stage_object_trusted (data->dest_repo, objtype,
                                             checksum, FALSE, NULL, NULL, input,
                                             cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
copy_one_ref (GFile   *src_repo_dir,
              GFile   *dest_repo_dir,
              const char *name,
              GCancellable  *cancellable,
              GError **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *src_path = NULL;
  ot_lobj GFile *dest_path = NULL;
  ot_lobj GFile *dest_parent = NULL;
  ot_lfree char *refpath = NULL;

  refpath = g_build_filename ("refs/heads", name, NULL);
  src_path = g_file_resolve_relative_path (src_repo_dir, refpath);
  dest_path = g_file_resolve_relative_path (dest_repo_dir, refpath);
  dest_parent = g_file_get_parent (dest_path);
  
  if (!ot_gfile_ensure_directory (dest_parent, TRUE, error))
    goto out;
  
  if (!g_file_copy (src_path, dest_path, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS,
                    cancellable, NULL, NULL, error))
    goto out;
  
  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_builtin_local_clone (int argc, char **argv, GFile *repo_path, GError **error)
{
  gboolean ret = FALSE;
  GCancellable *cancellable = NULL;
  GOptionContext *context;
  const char *destination;
  int i;
  GHashTableIter hash_iter;
  gpointer key, value;
  ot_lhash GHashTable *objects = NULL;
  ot_lobj GFile *dest_f = NULL;
  ot_lobj GFile *src_repo_dir = NULL;
  ot_lobj GFile *dest_repo_dir = NULL;
  ot_lobj GFileInfo *src_info = NULL;
  ot_lobj GFileInfo *dest_info = NULL;
  ot_lobj GFile *src_dir = NULL;
  ot_lobj GFile *dest_dir = NULL;
  OtLocalCloneData data;

  context = g_option_context_new ("DEST ... - Create new repository DEST");
  g_option_context_add_main_entries (context, options, NULL);

  memset (&data, 0, sizeof (data));

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  data.src_repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (data.src_repo, error))
    goto out;

  if (argc < 2)
    {
      gchar *help = g_option_context_get_help (context, TRUE, NULL);
      g_printerr ("%s\n", help);
      g_free (help);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "DESTINATION must be specified");
      goto out;
    }

  destination = argv[1];
  dest_f = ot_gfile_new_for_path (destination);

  data.dest_repo = ostree_repo_new (dest_f);
  if (!ostree_repo_check (data.dest_repo, error))
    goto out;

  src_repo_dir = g_object_ref (ostree_repo_get_path (data.src_repo));
  dest_repo_dir = g_object_ref (ostree_repo_get_path (data.dest_repo));

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

  if (!ostree_repo_list_objects (data.src_repo, OSTREE_REPO_LIST_OBJECTS_ALL,
                                 &objects, cancellable, error))
    goto out;

  if (!ostree_repo_prepare_transaction (data.dest_repo, NULL, error))
    goto out;
  
  g_hash_table_iter_init (&hash_iter, objects);

  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      const char *checksum;
      OstreeObjectType objtype;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      if (!import_one_object (&data, checksum, objtype, cancellable, error))
        goto out;
    }

  if (!ostree_repo_commit_transaction (data.dest_repo, NULL, error))
    goto out;

  if (argc > 2)
    {
      for (i = 2; i < argc; i++)
        {
          if (!copy_one_ref (src_repo_dir, dest_repo_dir, argv[i], cancellable, error))
            goto out;
        }
    }
  else
    {
      src_dir = g_file_resolve_relative_path (src_repo_dir, "refs/heads");
      dest_dir = g_file_resolve_relative_path (dest_repo_dir, "refs/heads");
      if (!copy_dir_contents_recurse (src_dir, dest_dir, NULL, error))
        goto out;
      g_clear_object (&src_dir);
      g_clear_object (&dest_dir);
      
      src_dir = g_file_resolve_relative_path (src_repo_dir, "tags");
      dest_dir = g_file_resolve_relative_path (dest_repo_dir, "tags");
      if (!copy_dir_contents_recurse (src_dir, dest_dir, NULL, error))
        goto out;
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
