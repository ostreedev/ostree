/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-builtins.h"
#include "ostree.h"

#include <glib/gi18n.h>

#define FAST_QUERYINFO "standard::name,standard::type,standard::is-symlink,standard::symlink-target,unix::*"

static GOptionEntry options[] = {
  { NULL }
};

static void
rm_rf (GFile *path)
{
  GFileInfo *finfo = NULL;
  GFileEnumerator *path_enum = NULL;
  guint32 type;
  
  finfo = g_file_query_info (path, FAST_QUERYINFO,
                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                             NULL, NULL);
  if (!finfo)
    goto out;

  type = g_file_info_get_attribute_uint32 (finfo, "standard::type");
  if (type == G_FILE_TYPE_DIRECTORY)
    {
      path_enum = g_file_enumerate_children (path, FAST_QUERYINFO, 
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                             NULL, NULL);
      if (!path_enum)
        goto out;

      
      g_clear_object (&finfo);
      while ((finfo = g_file_enumerator_next_file (path_enum, NULL, NULL)) != NULL)
        {
          GFile *child = g_file_get_child (path, g_file_info_get_attribute_byte_string (finfo, "standard::name"));
          rm_rf (child);
          g_clear_object (&child);
          g_clear_object (&finfo);
        }
    }

  (void) g_file_delete (path, NULL, NULL);

 out:
  g_clear_object (&finfo);
  g_clear_object (&path_enum);
}

static gboolean
merge_dir (GFile    *destination,
           GFile    *src,
           GError  **error)
{
  gboolean ret = FALSE;
  char *dest_path = NULL;
  char *src_path = NULL;
  GError *temp_error = NULL;
  GFileInfo *src_fileinfo = NULL;
  GFileInfo *dest_fileinfo = NULL;
  GFileEnumerator *src_enum = NULL;
  GFile *dest_subfile = NULL;
  GFile *src_subfile = NULL;
  const char *name;
  guint32 type;

  dest_path = g_file_get_path (destination);
  src_path = g_file_get_path (src);

  dest_fileinfo = g_file_query_info (destination, FAST_QUERYINFO,
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                     NULL, &temp_error);
  if (dest_fileinfo)
    {
      type = g_file_info_get_attribute_uint32 (dest_fileinfo, "standard::type");
      if (type != G_FILE_TYPE_DIRECTORY)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Attempting to replace non-directory %s with directory %s",
                       dest_path, src_path);
          goto out;
        }

      src_enum = g_file_enumerate_children (src, FAST_QUERYINFO, 
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            NULL, error);
      if (!src_enum)
        goto out;

      while ((src_fileinfo = g_file_enumerator_next_file (src_enum, NULL, &temp_error)) != NULL)
        {
          type = g_file_info_get_attribute_uint32 (src_fileinfo, "standard::type");
          name = g_file_info_get_attribute_byte_string (src_fileinfo, "standard::name");
      
          dest_subfile = g_file_get_child (destination, name);
          src_subfile = g_file_get_child (src, name);

          if (type == G_FILE_TYPE_DIRECTORY)
            {
              if (!merge_dir (dest_subfile, src_subfile, error))
                goto out;
            }
          else
            {
              if (!g_file_delete (dest_subfile, NULL, &temp_error))
                {
                  if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                    g_clear_error (&temp_error);
                  else
                    {
                      g_propagate_error (error, temp_error);
                      goto out;
                    }
                }
              if (!g_file_move (src_subfile, dest_subfile, 0, NULL, NULL, NULL, error))
                goto out;
            }
        }
      if (temp_error)
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }
  else if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_clear_error (&temp_error);
      if (!g_file_move (src, destination, 0, NULL, NULL, NULL, error))
        goto out;
    }
  else
    goto out;

  ret = TRUE;
 out:
  g_free (dest_path);
  g_free (src_path);
  g_clear_object (&src_fileinfo);
  g_clear_object (&dest_fileinfo);
  g_clear_object (&src_enum);
  g_clear_object (&dest_subfile);
  g_clear_object (&src_subfile);
  return ret;
}

static gboolean
compose_branch_on_dir (OstreeRepo *repo,
                       GFile *destination,
                       const char *branch,
                       GError **error)
{
  char *destpath = NULL;
  char *branchpath = NULL;
  GFile *branchf = NULL;
  GFileEnumerator *enumerator = NULL;
  gboolean ret = FALSE;
  char *branchrev = NULL;

  if (!ostree_repo_resolve_rev (repo, branch, &branchrev, error))
    goto out;
  
  destpath = g_file_get_path (destination);
  if (g_str_has_suffix (destpath, "/"))
    destpath[strlen (destpath) - 1] = '\0';
  branchpath = g_strconcat (destpath, "-tmp-checkout-", branchrev, NULL);
  branchf = ot_util_new_file_for_path (branchpath);

  g_print ("Checking out %s (commit %s)...\n", branch, branchrev);
  if (!ostree_repo_checkout (repo, branchrev, branchpath, error))
    goto out;
  g_print ("...done\n", branch);
  g_print ("Merging over destination...\n", branch, branchrev);
  if (!merge_dir (destination, branchf, error))
    goto out;

  ret = TRUE;
 out:
  if (branchf)
    rm_rf (branchf);
  g_clear_object (&enumerator);
  g_clear_object (&branchf);
  g_free (branchrev);
  g_free (destpath);
  g_free (branchpath);
  return ret;
}

gboolean
ostree_builtin_compose (int argc, char **argv, const char *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  OstreeCheckout *checkout = NULL;
  const char *destination;
  GFile *destf = NULL;
  int i;

  context = g_option_context_new ("DESTINATION BRANCH1 BRANCH2 ... - Merge multiple commits into a single filesystem tree");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  destination = argv[1];
  
  destf = ot_util_new_file_for_path (destination);
  
  if (argc < 3)
    {
      gchar *help = g_option_context_get_help (context, TRUE, NULL);
      g_printerr ("%s\n", help);
      g_free (help);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "DESTINATION and at least one COMMIT must be specified");
      goto out;
    }
  
  for (i = 2; i < argc; i++)
    {
      const char *branch = argv[i];
      
      if (!compose_branch_on_dir (repo, destf, branch, error))
        goto out;
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  g_clear_object (&repo);
  g_clear_object (&checkout);
  g_clear_object (&destf);
  return ret;
}
