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

#include <glib/gi18n.h>
#include <glib/gprintf.h>

static gboolean verbose;
static gboolean delete;
static int depth = 0;

static GOptionEntry options[] = {
  { "verbose", 0, 0, G_OPTION_ARG_NONE, &verbose, "Display progress", NULL },
  { "depth", 0, 0, G_OPTION_ARG_INT, &depth, "Only traverse commit objects by this count", NULL },
  { "delete", 0, 0, G_OPTION_ARG_NONE, &delete, "Remove no longer reachable objects", NULL },
  { NULL }
};

static void
log_verbose (const char  *fmt,
             ...) G_GNUC_PRINTF (1, 2);

static void
log_verbose (const char  *fmt,
             ...)
{
  va_list args;

  if (!verbose)
    return;

  va_start (args, fmt);
  
  g_vprintf (fmt, args);
  g_print ("\n");

  va_end (args);
}

typedef struct {
  OstreeRepo *repo;
  GHashTable *reachable;
  gboolean had_error;
  GError **error;
  guint n_reachable;
  guint n_unreachable;
} OtPruneData;

static gboolean
compute_reachable_objects_from_dir_contents (OstreeRepo      *repo,
                                             const char      *sha256,
                                             GHashTable      *inout_reachable,
                                             GCancellable    *cancellable,
                                             GError         **error)
{
  gboolean ret = FALSE;
  GVariant *tree = NULL;
  GVariant *files_variant = NULL;
  GVariant *dirs_variant = NULL;
  int n, i;
  char *key;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_DIR_TREE, sha256, &tree, error))
    goto out;

  key = ostree_object_to_string (sha256, OSTREE_OBJECT_TYPE_DIR_TREE);
  g_hash_table_replace (inout_reachable, key, key);

  /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
  files_variant = g_variant_get_child_value (tree, 2);
  n = g_variant_n_children (files_variant);
  for (i = 0; i < n; i++)
    {
      const char *filename;
      const char *checksum;
      
      g_variant_get_child (files_variant, i, "(&s&s)", &filename, &checksum);
      if (ostree_repo_get_mode (repo) == OSTREE_REPO_MODE_BARE)
        {
          key = ostree_object_to_string (checksum, OSTREE_OBJECT_TYPE_RAW_FILE);
          g_hash_table_replace (inout_reachable, key, key);
        }
      else
        {
          key = ostree_object_to_string (checksum, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META);
          g_hash_table_replace (inout_reachable, key, key);
          key = ostree_object_to_string (checksum, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT);
          g_hash_table_replace (inout_reachable, key, key);
        }
    }

  dirs_variant = g_variant_get_child_value (tree, 3);
  n = g_variant_n_children (dirs_variant);
  for (i = 0; i < n; i++)
    {
      const char *dirname;
      const char *tree_checksum;
      const char *meta_checksum;
      
      g_variant_get_child (dirs_variant, i, "(&s&s&s)",
                           &dirname, &tree_checksum, &meta_checksum);
      
      if (!compute_reachable_objects_from_dir_contents (repo, tree_checksum, inout_reachable,
                                                        cancellable, error))
        goto out;

      key = ostree_object_to_string (meta_checksum, OSTREE_OBJECT_TYPE_DIR_META);
      g_hash_table_replace (inout_reachable, key, key);
    }

  ret = TRUE;
 out:
  ot_clear_gvariant (&tree);
  ot_clear_gvariant (&files_variant);
  ot_clear_gvariant (&dirs_variant);
  return ret;
}

static gboolean
compute_reachable_objects_from_commit (OstreeRepo      *repo,
                                       const char      *sha256,
                                       int              traverse_depth,
                                       GHashTable      *inout_reachable,
                                       GCancellable    *cancellable,
                                       GError         **error)
{
  gboolean ret = FALSE;
  GVariant *commit = NULL;
  const char *parent_checksum;
  const char *contents_checksum;
  const char *meta_checksum;
  char *key;

  if (depth == 0 || traverse_depth < depth)
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, sha256, &commit, error))
        goto out;

      key = ostree_object_to_string (sha256, OSTREE_OBJECT_TYPE_COMMIT);
      g_hash_table_replace (inout_reachable, key, key);

      /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
      g_variant_get_child (commit, 2, "&s", &parent_checksum);

      if (strlen (parent_checksum) > 0)
        {
          if (!compute_reachable_objects_from_commit (repo, parent_checksum, traverse_depth + 1, inout_reachable, cancellable, error))
            goto out;
        }

      g_variant_get_child (commit, 6, "&s", &contents_checksum);

      if (!compute_reachable_objects_from_dir_contents (repo, contents_checksum, inout_reachable, cancellable, error))
        goto out;

      g_variant_get_child (commit, 7, "&s", &meta_checksum);
      key = ostree_object_to_string (meta_checksum, OSTREE_OBJECT_TYPE_DIR_META);
      g_hash_table_replace (inout_reachable, key, key);
    }

  ret = TRUE;
 out:
  ot_clear_gvariant (&commit);
  return ret;
}

static gboolean
prune_loose_object (OtPruneData    *data,
                    const char    *checksum,
                    OstreeObjectType objtype,
                    GCancellable    *cancellable,
                    GError         **error)
{
  gboolean ret = FALSE;
  char *key;
  GFile *objf = NULL;

  key = ostree_object_to_string (checksum, objtype);

  objf = ostree_repo_get_object_path (data->repo, checksum, objtype);

  if (!g_hash_table_lookup_extended (data->reachable, key, NULL, NULL))
    {
      if (delete)
        {
          if (!g_file_delete (objf, cancellable, error))
            goto out;
          g_print ("Deleted: %s\n", key);
        }
      else
        {
          g_print ("Unreachable: %s\n", key);
        }
      data->n_unreachable++;
    }
  else
    data->n_reachable++;

  ret = TRUE;
 out:
  g_clear_object (&objf);
  g_free (key);
  return ret;
}


gboolean
ostree_builtin_prune (int argc, char **argv, GFile *repo_path, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  OtPruneData data;
  GHashTable *objects = NULL;
  OstreeRepo *repo = NULL;
  GHashTable *all_refs = NULL;
  GHashTableIter hash_iter;
  gpointer key, value;
  GCancellable *cancellable = NULL;

  memset (&data, 0, sizeof (data));

  context = g_option_context_new ("- Search for unreachable objects");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  data.repo = repo;
  data.reachable = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  data.had_error = FALSE;
  data.error = error;
  data.n_reachable = 0;
  data.n_unreachable = 0;

  if (!ostree_repo_list_all_refs (repo, &all_refs, cancellable, error))
    goto out;

  g_hash_table_iter_init (&hash_iter, all_refs);

  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      const char *sha256 = value;

      log_verbose ("Computing reachable, currently %u total, from %s: %s", g_hash_table_size (data.reachable), name, sha256);
      if (!compute_reachable_objects_from_commit (repo, sha256, 0, data.reachable, cancellable, error))
        goto out;
    }

  if (!ostree_repo_list_objects (repo, OSTREE_REPO_LIST_OBJECTS_ALL, &objects, cancellable, error))
    goto out;

  g_hash_table_iter_init (&hash_iter, objects);


  if (!ostree_repo_list_objects (repo, OSTREE_REPO_LIST_OBJECTS_ALL,
                                 &objects, cancellable, error))
    goto out;
  
  g_hash_table_iter_init (&hash_iter, objects);

  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      GVariant *objdata = value;
      const char *checksum;
      OstreeObjectType objtype;
      gboolean is_loose;

      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

      g_variant_get_child (objdata, 0, "b", &is_loose);

      if (is_loose)
        {
          if (!prune_loose_object (&data, checksum, objtype, cancellable, error))
            goto out;
        }
    }

  if (data.had_error)
    goto out;

  g_print ("Total reachable: %u\n", data.n_reachable);
  g_print ("Total unreachable: %u\n", data.n_unreachable);

  ret = TRUE;
 out:
  if (all_refs)
    g_hash_table_unref (all_refs);
  if (data.reachable)
    g_hash_table_unref (data.reachable);
  if (context)
    g_option_context_free (context);
  g_clear_object (&repo);
  if (objects)
    g_hash_table_unref (objects);
  return ret;
}
