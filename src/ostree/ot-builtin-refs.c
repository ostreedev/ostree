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

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "libgsystem.h"

static gboolean opt_delete;

static GOptionEntry options[] = {
  { "delete", 0, 0, G_OPTION_ARG_NONE, &opt_delete, "Delete refs which match PREFIX, rather than listing them", NULL },
  { NULL }
};

gboolean
ostree_builtin_refs (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;
  const char *refspec_prefix = NULL;
  gs_unref_hashtable GHashTable *refs = NULL;
  GHashTableIter hashiter;
  gpointer hashkey, hashvalue;

  context = g_option_context_new ("[PREFIX] - List refs");

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc >= 2)
    refspec_prefix = argv[1];

  /* Require a prefix when deleting to help avoid accidents. */
  if (opt_delete && refspec_prefix == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "PREFIX is required when deleting refs");
      goto out;
    }

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
          g_autofree char *remote = NULL;
          g_autofree char *ref = NULL;

          if (!ostree_parse_refspec (refspec, &remote, &ref, error))
            goto out;
          
          if (!ostree_repo_set_ref_immediate (repo, remote, ref, NULL,
                                              cancellable, error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  if (repo)
    ostree_repo_abort_transaction (repo, cancellable, NULL);
  return ret;
}
