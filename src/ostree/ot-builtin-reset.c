/*
 * Copyright (C) 2013 Stef Walter <stefw@redhat.com>
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
 * Author: Stef Walter <stefw@redhat.com>
 */

#include "config.h"

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-reset.xml) when changing the option list.
 */

static gboolean opt_create;
static char *opt_collection_id;

static GOptionEntry options[] = {
  { "create", 'c', 0, G_OPTION_ARG_NONE, &opt_create, "Create the ref if it doesn’t exist already", NULL },
  { "collection-id", 0, 0, G_OPTION_ARG_STRING, &opt_collection_id, "Use the collection ID for the ref", "COLLECTION-ID" },
  { NULL }
};

gboolean
ostree_builtin_reset (int           argc,
                      char        **argv,
                      OstreeCommandInvocation *invocation,
                      GCancellable *cancellable,
                      GError      **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  g_autoptr(GHashTable) known_refs = NULL;
  gboolean ret = FALSE;
  const char *refspec;
  const char *target = NULL;
  g_autofree char *checksum = NULL;

  /* FIXME: Add support for collection–refs. */
  context = g_option_context_new ("REFSPEC COMMIT");

  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    goto out;

  if (!ostree_ensure_repo_writable (repo, error))
    goto out;

  if (argc <= 2)
    {
      ot_util_usage_error (context, "A REFSPEC and COMMIT argument is required", error);
      goto out;
    }
  refspec = argv[1];
  target = argv[2];

  if (!opt_create)
    {
      if (opt_collection_id)
        {
          const OstreeCollectionRef collection_ref = { opt_collection_id, (char *)refspec };

          if (!ostree_repo_list_collection_refs (repo, opt_collection_id, &known_refs,
                                                 OSTREE_REPO_LIST_REFS_EXT_NONE,
                                                 cancellable, error))
            goto out;

          if (!g_hash_table_contains (known_refs, &collection_ref))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR, "Invalid ref '%s'", refspec);
              goto out;
            }
        }
      else
        {
          if (!ostree_repo_list_refs (repo, NULL, &known_refs, cancellable, error))
            goto out;

          if (!g_hash_table_contains (known_refs, refspec))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR, "Invalid ref '%s'", refspec);
              goto out;
            }
        }
    }

  if (!ostree_repo_resolve_rev (repo, target, FALSE, &checksum, error))
    goto out;

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  if (opt_collection_id)
    {
      const OstreeCollectionRef collection_ref = { opt_collection_id, (char *)refspec };
      ostree_repo_transaction_set_collection_ref (repo, &collection_ref, checksum);
    }
  else
    ostree_repo_transaction_set_refspec (repo, refspec, checksum);

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (repo)
    ostree_repo_abort_transaction (repo, cancellable, NULL);
  return ret;
}
