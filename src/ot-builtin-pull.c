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

#include <libsoup/soup-gnome.h>

static char *repo_path;

static GOptionEntry options[] = {
  { "repo", 0, 0, G_OPTION_ARG_FILENAME, &repo_path, "Repository path", "repo" },
  { NULL }
};

static void
usage_error (GOptionContext *context, const char *message, GError **error)
{
  gchar *help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s\n", help);
  g_free (help);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       message);
}

static gboolean
fetch_uri (OstreeRepo  *repo,
           SoupSession *soup,
           SoupURI     *uri,
           char       **temp_filename,
           GError     **error)
{
  gboolean ret = FALSE;
  SoupMessage *msg = NULL;
  guint response;
  char *template = NULL;
  int fd;
  SoupBuffer *buf = NULL;
  GFile *tempf = NULL;
  
  msg = soup_message_new_from_uri (SOUP_METHOD_GET, uri);
  
  response = soup_session_send_message (soup, msg);
  if (response != 200)
    {
      char *uri_string = soup_uri_to_string (uri, FALSE);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to retrieve '%s': %d %s",
                   uri_string, response, msg->reason_phrase);
      g_free (uri_string);
      goto out;
    }

  template = g_strdup_printf ("%s/tmp-fetchXXXXXX", ostree_repo_get_path (repo));
  
  fd = g_mkstemp (template);
  if (fd < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }
  close (fd);
  tempf = ot_util_new_file_for_path (template);

  buf = soup_message_body_flatten (msg->response_body);

  if (!g_file_replace_contents (tempf, buf->data, buf->length, NULL, FALSE, 0, NULL, NULL, error))
    goto out;
  
  *temp_filename = template;
  template = NULL;

  ret = TRUE;
 out:
  g_free (template);
  g_clear_object (&msg);
  g_clear_object (&tempf);
  return ret;
}

static gboolean
store_object (OstreeRepo  *repo,
              SoupSession *soup,
              SoupURI     *baseuri,
              const char  *object,
              OstreeObjectType objtype,
              gboolean    *did_exist,
              GError     **error)
{
  gboolean ret = FALSE;
  char *filename = NULL;
  char *objpath = NULL;
  char *relpath = NULL;
  SoupURI *obj_uri = NULL;

  objpath = ostree_get_relative_object_path (object, objtype, TRUE);
  obj_uri = soup_uri_copy (baseuri);
  relpath = g_build_filename (soup_uri_get_path (obj_uri), objpath, NULL);
  soup_uri_set_path (obj_uri, relpath);

  if (!fetch_uri (repo, soup, obj_uri, &filename, error))
    goto out;

  if (!ostree_repo_store_packfile (repo, object, filename, objtype, error))
    goto out;

  ret = TRUE;
 out:
  if (obj_uri)
    soup_uri_free (obj_uri);
  if (filename)
    (void) unlink (filename);
  g_free (filename);
  g_free (objpath);
  g_free (relpath);
  return ret;
}

static gboolean
store_tree_recurse (OstreeRepo   *repo,
                    SoupSession  *soup,
                    SoupURI      *base_uri,
                    const char   *rev,
                    GError      **error)
{
  gboolean ret = FALSE;
  GVariant *tree = NULL;
  GVariant *files_variant = NULL;
  GVariant *dirs_variant = NULL;
  OstreeSerializedVariantType metatype;
  gboolean did_exist;
  int i, n;

  if (!store_object (repo, soup, base_uri, rev, OSTREE_OBJECT_TYPE_META, &did_exist, error))
    goto out;

  if (!did_exist)
    {
      if (!ostree_repo_load_variant (repo, rev, &metatype, &tree, error))
        goto out;
      
      if (metatype != OSTREE_SERIALIZED_TREE_VARIANT)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Tree metadata '%s' has wrong type %d, expected %d",
                       rev, metatype, OSTREE_SERIALIZED_TREE_VARIANT);
          goto out;
        }
      
      /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
      g_variant_get_child (tree, 2, "@a(ss)", &files_variant);
      g_variant_get_child (tree, 3, "@a(sss)", &dirs_variant);
      
      n = g_variant_n_children (files_variant);
      for (i = 0; i < n; i++)
        {
          const char *filename;
          const char *checksum;

          g_variant_get_child (files_variant, i, "(ss)", &filename, &checksum);

          if (!store_object (repo, soup, base_uri, checksum, OSTREE_OBJECT_TYPE_FILE, &did_exist, error))
            goto out;
        }
      
      for (i = 0; i < n; i++)
        {
          const char *dirname;
          const char *tree_checksum;
          const char *meta_checksum;

          g_variant_get_child (dirs_variant, i, "(sss)",
                               &dirname, &tree_checksum, &meta_checksum);

          if (!store_tree_recurse (repo, soup, base_uri, tree_checksum, error))
            goto out;

          if (!store_object (repo, soup, base_uri, meta_checksum, OSTREE_OBJECT_TYPE_META, &did_exist, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  if (tree)
    g_variant_unref (tree);
  if (files_variant)
    g_variant_unref (files_variant);
  if (dirs_variant)
    g_variant_unref (dirs_variant);
  return ret;
}

static gboolean
store_commit_recurse (OstreeRepo   *repo,
                      SoupSession  *soup,
                      SoupURI      *base_uri,
                      const char   *rev,
                      GError      **error)
{
  gboolean ret = FALSE;
  GVariant *commit = NULL;
  OstreeSerializedVariantType metatype;
  const char *tree_contents_checksum;
  const char *tree_meta_checksum;
  gboolean did_exist;

  if (!store_object (repo, soup, base_uri, rev, OSTREE_OBJECT_TYPE_META, &did_exist, error))
    goto out;

  if (!did_exist)
    {
      if (!ostree_repo_load_variant (repo, rev, &metatype, &commit, error))
        goto out;
      
      if (metatype != OSTREE_SERIALIZED_COMMIT_VARIANT)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Commit '%s' has wrong type %d, expected %d",
                       rev, metatype, OSTREE_SERIALIZED_COMMIT_VARIANT);
          goto out;
        }
      
      /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
      g_variant_get_child (commit, 6, "&s", &tree_contents_checksum);
      g_variant_get_child (commit, 7, "&s", &tree_meta_checksum);
      
      if (!store_object (repo, soup, base_uri, tree_meta_checksum, OSTREE_OBJECT_TYPE_META, &did_exist, error))
        goto out;
      
      if (!store_tree_recurse (repo, soup, base_uri, tree_contents_checksum, error))
        goto out;
    }

  ret = TRUE;
 out:
  if (commit)
    g_variant_unref (commit);
  return ret;
}
                      
gboolean
ostree_builtin_pull (int argc, char **argv, const char *prefix, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  const char *remote;
  const char *branch;
  char *remote_branch_ref_path = NULL;
  char *key = NULL;
  char *baseurl = NULL;
  char *refpath = NULL;
  char *temppath = NULL;
  GKeyFile *config = NULL;
  SoupURI *base_uri = NULL;
  SoupURI *target_uri = NULL;
  SoupSession *soup = NULL;
  char *rev = NULL;

  context = g_option_context_new ("REMOTE BRANCH - Download data from remote repository");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (repo_path == NULL)
    repo_path = ".";

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (argc < 3)
    {
      usage_error (context, "REMOTE and BRANCH must be specified", error);
      goto out;
    }

  remote = argv[1];
  branch = argv[2];

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
  target_uri = soup_uri_copy (base_uri);
  g_free (refpath);
  refpath = g_build_filename (soup_uri_get_path (target_uri), "refs", "heads", branch, NULL);
  soup_uri_set_path (target_uri, refpath);
  
  soup = soup_session_sync_new_with_options (SOUP_SESSION_USER_AGENT, "ostree ",
                                             SOUP_SESSION_ADD_FEATURE_BY_TYPE, SOUP_TYPE_GNOME_FEATURES_2_26,
                                             SOUP_SESSION_ADD_FEATURE_BY_TYPE, SOUP_TYPE_CONTENT_DECODER,
                                             SOUP_SESSION_ADD_FEATURE_BY_TYPE, SOUP_TYPE_COOKIE_JAR,
                                             NULL);
  if (!fetch_uri (repo, soup, target_uri, &temppath, error))
    goto out;

  rev = ot_util_get_file_contents_utf8 (temppath, error);
  if (!rev)
    goto out;
  g_strchomp (rev);

  if (!ostree_validate_checksum_string (rev, error))
    goto out;

  if (!store_commit_recurse (repo, soup, base_uri, rev, error))
    goto out;

  if (!ostree_repo_write_ref (repo, FALSE, branch, rev, error))
    goto out;
 
  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  if (temppath)
    (void) unlink (temppath);
  g_free (temppath);
  g_free (key);
  g_free (rev);
  g_free (baseurl);
  g_free (refpath);
  g_free (remote_branch_ref_path);
  g_clear_object (&soup);
  if (base_uri)
    soup_uri_free (base_uri);
  if (target_uri)
    soup_uri_free (target_uri);
  g_clear_object (&repo);
  g_clear_object (&soup);
  return ret;
}
