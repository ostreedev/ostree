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
  g_autoptr(GBytes) summary_bytes = NULL;
  gboolean ret = FALSE;

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

  if (!ostree_repo_remote_fetch_summary (repo, remote_name,
                                         &summary_bytes, NULL,
                                         cancellable, error))
    goto out;

  if (summary_bytes == NULL)
    {
      g_print ("Remote refs not available; server has no summary file\n");
    }
  else
    {
      g_autoptr(GVariant) summary = NULL;
      g_autoptr(GVariant) ref_map = NULL;
      GVariantIter iter;
      GVariant *child;

      summary = g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                          summary_bytes, FALSE);

      ref_map = g_variant_get_child_value (summary, 0);

      /* Ref map should already be sorted by ref name. */
      g_variant_iter_init (&iter, ref_map);
      while ((child = g_variant_iter_next_value (&iter)) != NULL)
        {
          const char *ref_name = NULL;

          g_variant_get_child (child, 0, "&s", &ref_name);

          if (ref_name != NULL)
            g_print ("%s\n", ref_name);

          g_variant_unref (child);
        }
    }

  ret = TRUE;

out:
  g_option_context_free (context);

  return ret;
}
