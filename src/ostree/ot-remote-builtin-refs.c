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

static GOptionEntry option_entries[] = {
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

  remote_name = argv[1];

  if (!ostree_repo_remote_list_refs (repo, remote_name, &refs, cancellable, error))
    goto out;
  else
    {
      GHashTableIter hash_iter;
      gpointer key, value;
      g_autoptr(GPtrArray) sorted_keys = g_ptr_array_new ();
      guint i;

      g_hash_table_iter_init (&hash_iter, refs);
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        g_ptr_array_add (sorted_keys, key);

      for (i = 0; i < sorted_keys->len; i++)
        {
          g_print ("%s\n", (const char *) sorted_keys->pdata[i]);
        }
    }

  ret = TRUE;

out:
  g_option_context_free (context);

  return ret;
}
