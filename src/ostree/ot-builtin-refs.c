/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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
#include "libgsystem.h"

static gboolean opt_delete;

static GOptionEntry options[] = {
  { "delete", 0, 0, G_OPTION_ARG_NONE, &opt_delete, "Delete refs which match PREFIX, rather than listing them", "PREFIX" },
  { NULL }
};

gboolean
ostree_builtin_refs (int argc, char **argv, OstreeRepo *repo, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  const char *refspec_prefix = NULL;
  gs_unref_hashtable GHashTable *refs = NULL;
  GHashTableIter hashiter;
  gpointer hashkey, hashvalue;

  context = g_option_context_new ("[PREFIX] - List refs");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc >= 2)
    refspec_prefix = argv[1];

  if (!ostree_repo_list_refs (repo, refspec_prefix, &refs,
                              cancellable, error))
    goto out;

  if (!opt_delete)
    {
      g_hash_table_iter_init (&hashiter, refs);
      while (g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue))
        {
          const char *ref = hashkey;
          g_print ("%s\n", ref);
        }
    }
  else
    {
      g_hash_table_iter_init (&hashiter, refs);
      while (g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue))
        {
          const char *refspec = hashkey;
          if (!ostree_repo_write_refspec (repo, refspec, NULL, error))
            goto out;
        }
    }
 
  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
