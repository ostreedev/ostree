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

static gboolean opt_delete;
static gboolean opt_list;
static char *opt_create;

static GOptionEntry options[] = {
  { "delete", 0, 0, G_OPTION_ARG_NONE, &opt_delete, "Delete refs which match PREFIX, rather than listing them", NULL },
  { "list", 0, 0, G_OPTION_ARG_NONE, &opt_list, "Do not remove the prefix from the refs", NULL },
  { "create", 0, 0, G_OPTION_ARG_STRING, &opt_create, "Create a new ref for an existing commit", "NEWREF" },
  { NULL }
};

static gboolean do_ref (OstreeRepo *repo, const char *refspec_prefix, GCancellable *cancellable, GError **error)
{
  g_autoptr(GHashTable) refs = NULL;
  GHashTableIter hashiter;
  gpointer hashkey, hashvalue;
  gboolean ret = FALSE;

  if (opt_delete || opt_list)
    {
      if (!ostree_repo_list_refs_ext (repo, refspec_prefix, &refs, OSTREE_REPO_LIST_REFS_EXT_NONE,
                                      cancellable, error))
        goto out;
    }
  else if (opt_create)
    {
      if (!ostree_repo_list_refs_ext (repo, NULL, &refs, OSTREE_REPO_LIST_REFS_EXT_NONE,
                                      cancellable, error))
        goto out;
    }
  else if (!ostree_repo_list_refs (repo, refspec_prefix, &refs, cancellable, error))
    goto out;

  if (!opt_delete && !opt_create)
    {
      g_hash_table_iter_init (&hashiter, refs);
      while (g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue))
        {
          const char *ref = hashkey;
          g_print ("%s\n", ref);
        }
    }
  else if (opt_create)
    {
      g_autofree char *checksum = NULL;
      g_autofree char *checksum_existing = NULL;
      g_autofree char *remote = NULL;
      g_autofree char *ref = NULL;

      if (!ostree_repo_resolve_rev_ext (repo, opt_create, TRUE, OSTREE_REPO_RESOLVE_REV_EXT_NONE, &checksum_existing, error))
        {
          if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY))
            {
              /* A folder exists with the specified ref name,
               * which is handled by _ostree_repo_write_ref */
              g_clear_error (error);
            }
          else goto out;
        }

      if (checksum_existing != NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "--create specified but ref %s already exists", opt_create);
          goto out;
        }

      if (!ostree_repo_resolve_rev (repo, refspec_prefix, FALSE, &checksum, error))
        goto out;

      if (!ostree_parse_refspec (opt_create, &remote, &ref, error))
        goto out;

      if (!ostree_repo_set_ref_immediate (repo, remote, ref, checksum,
                                          cancellable, error))
        goto out;
    }
  else
    /* delete */
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
  return ret;
}

gboolean
ostree_builtin_refs (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  int i;

  context = g_option_context_new ("[PREFIX] - List refs");

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc >= 2)
    {
      if (opt_create && argc > 2)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "You must specify only 1 existing ref when creating a new ref");
          goto out;
        }
      for (i = 1; i < argc; i++)
        if (!do_ref (repo, argv[i], cancellable, error))
          goto out;
    }
  else
    {
      /* Require a prefix when deleting to help avoid accidents. */
      if (opt_delete)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "At least one PREFIX is required when deleting refs");
          goto out;
        }
      else if (opt_create)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "You must specify an existing ref when creating a new ref");
          goto out;
        }

      ret = do_ref (repo, NULL, cancellable, error);
    }

  ret = TRUE;
 out:
  if (repo)
    ostree_repo_abort_transaction (repo, cancellable, NULL);
  return ret;
}
