/*
 * Copyright (C) 2015 Red Hat, Inc.
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "otutil.h"

#include "ot-remote-builtins.h"

static gboolean opt_revision;
static char *opt_cache_dir;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-remote.xml) when changing the option list.
 */

static GOptionEntry option_entries[]
    = { { "revision", 'r', 0, G_OPTION_ARG_NONE, &opt_revision, "Show revisions in listing", NULL },
        { "cache-dir", 0, 0, G_OPTION_ARG_FILENAME, &opt_cache_dir, "Use custom cache dir", NULL },
        { NULL } };

gboolean
ot_remote_builtin_refs (int argc, char **argv, OstreeCommandInvocation *invocation,
                        GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("NAME");

  g_autoptr (OstreeRepo) repo = NULL;
  if (!ostree_option_context_parse (context, option_entries, &argc, &argv, invocation, &repo,
                                    cancellable, error))
    return FALSE;

  if (argc < 2)
    {
      ot_util_usage_error (context, "NAME must be specified", error);
      return FALSE;
    }

  if (opt_cache_dir)
    {
      if (!ostree_repo_set_cache_dir (repo, AT_FDCWD, opt_cache_dir, cancellable, error))
        return FALSE;
    }

  const char *remote_name = argv[1];
  g_autoptr (GHashTable) refs = NULL;

  if (!ostree_repo_remote_list_refs (repo, remote_name, &refs, cancellable, error))
    return FALSE;
  else
    {
      g_autoptr (GList) ordered_keys = NULL;
      GList *iter = NULL;

      ordered_keys = g_hash_table_get_keys (refs);
      ordered_keys = g_list_sort (ordered_keys, (GCompareFunc)strcmp);

      for (iter = ordered_keys; iter; iter = iter->next)
        {
          const char *ref = iter->data;

          if (opt_revision)
            {
              const char *rev = g_hash_table_lookup (refs, ref);
              g_print ("%s:%s\t%s\n", remote_name, ref, rev);
            }
          else
            {
              g_print ("%s:%s\n", remote_name, ref);
            }
        }
    }

  return TRUE;
}
