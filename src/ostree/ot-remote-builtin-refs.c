/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
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
 */

#include "config.h"

#include "otutil.h"

#include "ot-main.h"
#include "ot-remote-builtins.h"

static char* opt_cache_dir;

static GOptionEntry option_entries[] = {
  { "cache-dir", 0, 0, G_OPTION_ARG_STRING, &opt_cache_dir, "Use custom cache dir", NULL },
};

gboolean
ot_remote_builtin_refs (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  glnx_unref_object OstreeRepo *repo = NULL;
  const char *remote_name;
  gboolean ret = FALSE;
  g_autoptr(GHashTable) refs = NULL;

  context = g_option_context_new ("NAME - List remote refs");

  if (!ostree_option_context_parse (context, option_entries, &argc, &argv,
                                    OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "NAME must be specified", error);
      goto out;
    }

  if (opt_cache_dir)
    {
      if (!ostree_repo_set_cache_dir (repo, AT_FDCWD, opt_cache_dir, cancellable, error))
        goto out;
    }

  remote_name = argv[1];

  if (!ostree_repo_remote_list_refs (repo, remote_name, &refs, cancellable, error))
    goto out;
  else
    {
      g_autoptr(GList) ordered_keys = NULL;
      GList *iter = NULL;

      ordered_keys = g_hash_table_get_keys (refs);
      ordered_keys = g_list_sort (ordered_keys, (GCompareFunc) strcmp);

      for (iter = ordered_keys; iter; iter = iter->next)
        {
          g_print ("%s:%s\n", remote_name, (const char *) iter->data);
        }
    }

  ret = TRUE;

out:
  g_option_context_free (context);

  return ret;
}
