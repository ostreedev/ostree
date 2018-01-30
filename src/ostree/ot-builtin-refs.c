/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
static gboolean opt_alias;
static char *opt_create;
#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
static gboolean opt_collections;
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-refs.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "delete", 0, 0, G_OPTION_ARG_NONE, &opt_delete, "Delete refs which match PREFIX, rather than listing them", NULL },
  { "list", 0, 0, G_OPTION_ARG_NONE, &opt_list, "Do not remove the prefix from the refs", NULL },
  { "alias", 'A', 0, G_OPTION_ARG_NONE, &opt_alias, "If used with --create, create an alias, otherwise just list aliases", NULL },
  { "create", 0, 0, G_OPTION_ARG_STRING, &opt_create, "Create a new ref for an existing commit", "NEWREF" },
#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
  { "collections", 'c', 0, G_OPTION_ARG_NONE, &opt_collections, "Enable listing collection IDs for refs", NULL },
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */
  { NULL }
};

#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
static gboolean
do_ref_with_collections (OstreeRepo    *repo,
                         const char    *refspec_prefix,
                         GCancellable  *cancellable,
                         GError       **error)
{
  g_autoptr(GHashTable) refs = NULL;  /* (element-type OstreeCollectionRef utf8) */
  GHashTableIter hashiter;
  gpointer hashkey, hashvalue;
  gboolean ret = FALSE;

  if (!ostree_repo_list_collection_refs (repo,
                                         (!opt_create) ? refspec_prefix : NULL,
                                         &refs, OSTREE_REPO_LIST_REFS_EXT_NONE,
                                         cancellable, error))
    goto out;

  if (!opt_delete && !opt_create)
    {
      g_hash_table_iter_init (&hashiter, refs);
      while (g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue))
        {
          const OstreeCollectionRef *ref = hashkey;
          g_print ("(%s, %s)\n", ref->collection_id, ref->ref_name);
        }
    }
  else if (opt_create)
    {
      g_autofree char *checksum = NULL;
      g_autofree char *checksum_existing = NULL;

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

      /* This is technically an abuse of the refspec syntax: collection IDs
       * should not be treated like remote names. */
      g_auto(GStrv) parts = g_strsplit (opt_create, ":", 2);
      const char *collection_id = parts[0];
      const char *ref_name = parts[1];
      if (!ostree_validate_collection_id (collection_id, error))
        goto out;
      if (!ostree_validate_rev (ref_name, error))
        goto out;

      const OstreeCollectionRef ref = { (gchar *) collection_id, (gchar *) ref_name };
      if (!ostree_repo_set_collection_ref_immediate (repo, &ref, checksum,
                                                     cancellable, error))
        goto out;
    }
  else
    /* delete */
    {
      g_hash_table_iter_init (&hashiter, refs);
      while (g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue))
        {
          const OstreeCollectionRef *ref = hashkey;

          if (!ostree_repo_set_collection_ref_immediate (repo, ref, NULL,
                                                         cancellable, error))
            goto out;
        }
    }
  ret = TRUE;
 out:
  return ret;
}
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */

static gboolean do_ref (OstreeRepo *repo, const char *refspec_prefix, GCancellable *cancellable, GError **error)
{
  g_autoptr(GHashTable) refs = NULL;
  g_autoptr(GHashTable) ref_aliases = NULL;
  GHashTableIter hashiter;
  gpointer hashkey, hashvalue;
  gboolean ret = FALSE;
  gboolean is_list;

#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
  if (opt_collections)
    return do_ref_with_collections (repo, refspec_prefix, cancellable, error);
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */

  /* If we're doing aliasing, we need the full list of aliases mostly to allow
   * replacing existing aliases.
   */
  if (opt_alias)
    {
      if (!ostree_repo_list_refs_ext (repo, NULL, &ref_aliases,
                                      OSTREE_REPO_LIST_REFS_EXT_ALIASES,
                                      cancellable, error))
        goto out;
    }

  is_list = !(opt_delete || opt_create);

  if (opt_delete || opt_list || (!opt_create && opt_alias))
    {
      OstreeRepoListRefsExtFlags flags = OSTREE_REPO_LIST_REFS_EXT_NONE;
      if (opt_alias)
        flags |= OSTREE_REPO_LIST_REFS_EXT_ALIASES;
      if (!ostree_repo_list_refs_ext (repo, refspec_prefix, &refs, flags,
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

  if (is_list)
    {
      GLNX_HASH_TABLE_FOREACH_KV (refs, const char *, ref, const char *, value)
        {
          if (opt_alias)
            g_print ("%s -> %s\n", ref, value);
          else
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

      /* We want to allow replacing an existing alias */
      gboolean replacing_alias = opt_alias && g_hash_table_contains (ref_aliases, opt_create);
      if (!replacing_alias && checksum_existing != NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "--create specified but ref %s already exists", opt_create);
          goto out;
        }

      if (!ostree_parse_refspec (opt_create, &remote, &ref, error))
        goto out;

      if (opt_alias)
        {
          if (remote)
            return glnx_throw (error, "Cannot create alias to remote ref: %s", remote);
          if (!ostree_repo_set_alias_ref_immediate (repo, remote, ref, refspec_prefix,
                                                    cancellable, error))
            goto out;
        }
      else
        {
          if (!ostree_repo_resolve_rev (repo, refspec_prefix, FALSE, &checksum, error))
            goto out;

          if (!ostree_repo_set_ref_immediate (repo, remote, ref, checksum,
                                              cancellable, error))
            goto out;
        }
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
ostree_builtin_refs (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  int i;

  context = g_option_context_new ("[PREFIX]");

  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
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

      if (!do_ref (repo, NULL, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  if (repo)
    ostree_repo_abort_transaction (repo, cancellable, NULL);
  return ret;
}
