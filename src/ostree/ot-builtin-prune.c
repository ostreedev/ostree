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

static gboolean opt_no_prune;
static gint opt_depth = -1;
static gboolean opt_refs_only;

static GOptionEntry options[] = {
  { "no-prune", 0, 0, G_OPTION_ARG_NONE, &opt_no_prune, "Only display unreachable objects; don't delete", NULL },
  { "refs-only", 0, 0, G_OPTION_ARG_NONE, &opt_refs_only, "Only compute reachability via refs", NULL },
  { "depth", 0, 0, G_OPTION_ARG_INT, &opt_depth, "Only traverse DEPTH parents for each commit (default: -1=infinite)", "DEPTH" },
  { NULL }
};

typedef struct {
  OstreeRepo *repo;
  GHashTable *reachable;
  guint n_reachable_meta;
  guint n_reachable_content;
  guint n_unreachable_meta;
  guint n_unreachable_content;
  guint64 freed_bytes;
} OtPruneData;

static gboolean
maybe_prune_loose_object (OtPruneData    *data,
                          const char    *checksum,
                          OstreeObjectType objtype,
                          GCancellable    *cancellable,
                          GError         **error)
{
  gboolean ret = FALSE;
  ot_lvariant GVariant *key = NULL;
  ot_lobj GFile *objf = NULL;

  key = ostree_object_name_serialize (checksum, objtype);

  objf = ostree_repo_get_object_path (data->repo, checksum, objtype);

  if (!g_hash_table_lookup_extended (data->reachable, key, NULL, NULL))
    {
      if (!opt_no_prune)
        {
          ot_lobj GFileInfo *info = NULL;

          if ((info = g_file_query_info (objf, OSTREE_GIO_FAST_QUERYINFO,
                                         G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                         cancellable, error)) == NULL)
            goto out;

          if (!gs_file_unlink (objf, cancellable, error))
            goto out;

          data->freed_bytes += g_file_info_get_size (info);
        }
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        data->n_unreachable_meta++;
      else
        data->n_unreachable_content++;
    }
  else
    {
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        data->n_reachable_meta++;
      else
        data->n_reachable_content++;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_builtin_prune (int argc, char **argv, GFile *repo_path, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  GHashTableIter hash_iter;
  gpointer key, value;
  GCancellable *cancellable = NULL;
  ot_lhash GHashTable *objects = NULL;
  ot_lobj OstreeRepo *repo = NULL;
  ot_lhash GHashTable *all_refs = NULL;
  OtPruneData data;

  memset (&data, 0, sizeof (data));

  context = g_option_context_new ("- Search for unreachable objects");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  data.repo = repo;
  data.reachable = ostree_traverse_new_reachable ();

  if (opt_refs_only)
    {
      if (!ostree_repo_list_all_refs (repo, &all_refs, cancellable, error))
        goto out;
      
      g_hash_table_iter_init (&hash_iter, all_refs);
      
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *checksum = value;
          
          // g_print ("Computing reachable, currently %u total, from %s: %s\n", g_hash_table_size (data.reachable), name, checksum);
          if (!ostree_traverse_commit (repo, checksum, opt_depth, data.reachable, cancellable, error))
            goto out;
        }
    }

  if (!ostree_repo_list_objects (repo, OSTREE_REPO_LIST_OBJECTS_ALL, &objects, cancellable, error))
    goto out;

  if (!opt_refs_only)
    {
      g_hash_table_iter_init (&hash_iter, objects);
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          GVariant *serialized_key = key;
          const char *checksum;
          OstreeObjectType objtype;

          ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

          if (objtype != OSTREE_OBJECT_TYPE_COMMIT)
            continue;
          
          if (!ostree_traverse_commit (repo, checksum, opt_depth, data.reachable, cancellable, error))
            goto out;
        }
    }

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

      if (!is_loose)
        continue;

      if (!maybe_prune_loose_object (&data, checksum, objtype, cancellable, error))
        goto out;
    }

  g_print ("Total reachable: %u meta, %u content\n",
           data.n_reachable_meta, data.n_reachable_content);
  if (opt_no_prune)
    g_print ("Total unreachable: %u meta, %u content\n",
             data.n_unreachable_meta, data.n_unreachable_content);
  else
    g_print ("Freed %" G_GUINT64_FORMAT " bytes from %u meta, %u content objects\n",
             data.freed_bytes, data.n_unreachable_meta, data.n_unreachable_content);
    

  ret = TRUE;
 out:
  if (data.reachable)
    g_hash_table_unref (data.reachable);
  if (context)
    g_option_context_free (context);
  return ret;
}
