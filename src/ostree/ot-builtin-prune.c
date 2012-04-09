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
static int depth = -1;

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
  guint n_reachable;
  guint n_unreachable;
} OtPruneData;


static gboolean
prune_loose_object (OtPruneData    *data,
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
      if (delete)
        {
          if (!ot_gfile_unlink (objf, cancellable, error))
            goto out;
          g_print ("Deleted: %s.%s\n", checksum, ostree_object_type_to_string (objtype));
        }
      else
        {
          g_print ("Unreachable: %s.%s\n", checksum, ostree_object_type_to_string (objtype));
        }
      data->n_unreachable++;
    }
  else
    data->n_reachable++;

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
  data.n_reachable = 0;
  data.n_unreachable = 0;

  if (!ostree_repo_list_all_refs (repo, &all_refs, cancellable, error))
    goto out;

  g_hash_table_iter_init (&hash_iter, all_refs);

  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *name = key;
      const char *checksum = value;

      log_verbose ("Computing reachable, currently %u total, from %s: %s", g_hash_table_size (data.reachable), name, checksum);
      if (!ostree_traverse_commit (repo, checksum, depth, data.reachable, cancellable, error))
        goto out;
    }

  if (!ostree_repo_list_objects (repo, OSTREE_REPO_LIST_OBJECTS_ALL, &objects, cancellable, error))
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

  g_print ("Total reachable: %u\n", data.n_reachable);
  g_print ("Total unreachable: %u\n", data.n_unreachable);

  ret = TRUE;
 out:
  if (data.reachable)
    g_hash_table_unref (data.reachable);
  if (context)
    g_option_context_free (context);
  return ret;
}
