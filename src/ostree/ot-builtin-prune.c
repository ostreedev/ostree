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
  
  g_vprintf ("%s\n", args);

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

static char *
create_checksum_and_objtype (const char *checksum,
                             OstreeObjectType objtype)
{
  return g_strconcat (checksum, ".", ostree_object_type_to_string (objtype), NULL);
}

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

  key = create_checksum_and_objtype (sha256, OSTREE_OBJECT_TYPE_DIR_TREE);
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
          key = create_checksum_and_objtype (checksum, OSTREE_OBJECT_TYPE_RAW_FILE);
          g_hash_table_replace (inout_reachable, key, key);
        }
      else
        {
          key = create_checksum_and_objtype (checksum, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_META);
          g_hash_table_replace (inout_reachable, key, key);
          key = create_checksum_and_objtype (checksum, OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT);
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

      key = create_checksum_and_objtype (meta_checksum, OSTREE_OBJECT_TYPE_DIR_META);
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

      key = create_checksum_and_objtype (sha256, OSTREE_OBJECT_TYPE_COMMIT);
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
      key = create_checksum_and_objtype (meta_checksum, OSTREE_OBJECT_TYPE_DIR_META);
      g_hash_table_replace (inout_reachable, key, key);
    }

  ret = TRUE;
 out:
  ot_clear_gvariant (&commit);
  return ret;
}

static void
object_iter_callback (OstreeRepo    *repo,
                      const char    *checksum,
                      OstreeObjectType objtype,
                      GFile         *objf,
                      GFileInfo     *file_info,
                      gpointer       user_data)
{
  OtPruneData *data = user_data;
  char *key;

  key = create_checksum_and_objtype (checksum, objtype);

  if (!g_hash_table_lookup_extended (data->reachable, key, NULL, NULL))
    {
      if (delete)
        {
          (void) unlink (ot_gfile_get_path_cached (objf));
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

  g_free (key);
}

static gboolean
add_refs_recurse (OstreeRepo    *repo,
                  GFile         *base,
                  GFile         *dir,
                  GHashTable    *refs,
                  GCancellable  *cancellable,
                  GError       **error)
{
  gboolean ret = FALSE;
  GFileInfo *file_info = NULL;
  GFileEnumerator *enumerator = NULL;
  GFile *child = NULL;
  GError *temp_error = NULL;

  enumerator = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO,
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, error);
  if (!enumerator)
    goto out;

  while ((file_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)) != NULL)
    {
      g_clear_object (&child);
      child = g_file_get_child (dir, g_file_info_get_name (file_info));
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!add_refs_recurse (repo, base, child, refs, cancellable, error))
            goto out;
        }
      else if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
        {
          char *contents;
          gsize len;

          if (!g_file_load_contents (child, cancellable, &contents, &len, NULL, error))
            goto out;

          g_strchomp (contents);

          g_hash_table_insert (refs, g_file_get_relative_path (base, child), contents);
        }

      g_clear_object (&file_info);
    }
  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  g_clear_object (&file_info);
  g_clear_object (&child);
  return ret;
}

static gboolean
list_all_refs (OstreeRepo       *repo,
               GHashTable      **out_all_refs,
               GCancellable     *cancellable,
               GError          **error)
{
  gboolean ret = FALSE;
  GHashTable *ret_all_refs = NULL;
  GFile *heads_dir = NULL;

  ret_all_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  heads_dir = g_file_resolve_relative_path (ostree_repo_get_path (repo), "refs/heads");
  if (!add_refs_recurse (repo, heads_dir, heads_dir, ret_all_refs, cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_all_refs, &ret_all_refs);
 out:
  return ret;
}

gboolean
ostree_builtin_prune (int argc, char **argv, GFile *repo_path, GError **error)
{
  GOptionContext *context;
  OtPruneData data;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  GHashTable *all_refs = NULL;
  GHashTableIter hash_iter;
  gpointer key, value;
  GCancellable *cancellable = NULL;

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

  if (!list_all_refs (repo, &all_refs, cancellable, error))
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

  g_hash_table_iter_init (&hash_iter, data.reachable);

  if (!ostree_repo_iter_objects (repo, object_iter_callback, &data, error))
    goto out;

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
  return ret;
}
