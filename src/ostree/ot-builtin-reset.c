/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Stef Walter <stefw@redhat.com>
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

static GOptionEntry options[] = {
  { NULL }
};

static gchar *
find_current_ref (OstreeRepo   *repo,
                  const gchar  *ref,
                  GCancellable *cancellable,
                  GError      **error)
{
  gs_unref_hashtable GHashTable *refs = NULL;
  gchar *ret;

  if (!ostree_repo_list_refs (repo, NULL, &refs, cancellable, error))
    return NULL;
  ret = g_strdup (g_hash_table_lookup (refs, ref));
  if (ret == NULL)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR, "The ref was not found: %s", ref);

  return ret;
}

static gboolean
check_revision_is_parent (OstreeRepo   *repo,
                          const char   *descendant,
                          const char   *ancestor,
                          GCancellable *cancellable,
                          GError      **error)
{
  gs_free char *parent = NULL;
  gs_unref_variant GVariant *variant = NULL;
  gboolean ret = FALSE;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 descendant, &variant, error))
    goto out;

  parent = ostree_commit_get_parent (variant);
  if (!parent)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "The ref does not have this commit as an ancestor: %s", ancestor);
      goto out;
    }

  if (!g_str_equal (parent, ancestor) &&
      !check_revision_is_parent (repo, parent, ancestor, cancellable, error))
    goto out;

  ret = TRUE;
out:
  return ret;
}

gboolean
ostree_builtin_reset (int           argc,
                      char        **argv,
                      GCancellable *cancellable,
                      GError      **error)
{
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;
  gboolean ret = FALSE;
  const char *ref;
  const char *target = NULL;
  gs_free gchar *current = NULL;
  gs_free gchar *checksum = NULL;

  context = g_option_context_new ("[ARG] - Reset a ref to a previous commit");

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (!ostree_ensure_repo_writable (repo, error))
    goto out;

  if (argc <= 2)
    {
      ot_util_usage_error (context, "A ref and commit argument is required", error);
      goto out;
    }
  ref = argv[1];
  target = argv[2];

  if (!ostree_repo_resolve_rev (repo, target, FALSE, &checksum, error))
    goto out;

  current = find_current_ref (repo, ref, cancellable, error);
  if (current == NULL)
    goto out;

  if (!check_revision_is_parent (repo, current, checksum, cancellable, error))
    goto out;

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  ostree_repo_transaction_set_ref (repo, NULL, ref, checksum);

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  if (repo)
    ostree_repo_abort_transaction (repo, cancellable, NULL);
  return ret;
}
