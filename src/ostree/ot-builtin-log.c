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

#include "ot-builtins.h"
#include "ot-dump.h"
#include "ostree.h"
#include "otutil.h"

static gboolean opt_raw;

static GOptionEntry options[] = {
  { "raw", 0, 0, G_OPTION_ARG_NONE, &opt_raw, "Show raw variant data" },
  { NULL }
};

static gboolean
log_commit (OstreeRepo     *repo,
            const gchar    *checksum,
            gboolean        is_recurse,
            OstreeDumpFlags flags,
            GError        **error)
{
  gs_unref_variant GVariant *variant = NULL;
  gs_unref_variant GVariant *metadata = NULL;
  gs_unref_variant GVariant *value = NULL;
  gs_free gchar *parent = NULL;
  gboolean ret = FALSE;
  GError *local_error = NULL;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                 &variant, &local_error))
    {
      if (is_recurse && g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_print ("<< History beyond this commit not fetched >>\n");
          g_clear_error (&local_error);
          ret = TRUE;
        }
      else
        {
          g_propagate_error (error, local_error);
        }
      goto out;
    }

  g_print ("%s %s\n", ostree_object_type_to_string (OSTREE_OBJECT_TYPE_COMMIT),
           checksum);

  /* If we have a version, dump it ... */
  metadata = g_variant_get_child_value (variant, 0);
  if ((value = g_variant_lookup_value (metadata, "version", NULL)))
    {
      g_print ("Version: %s\n", g_variant_get_string (value, NULL));
    }

  ot_dump_object (OSTREE_OBJECT_TYPE_COMMIT, checksum, variant,
                  flags | OSTREE_DUMP_SKIP_OBJ_TYPE);

  /* Get the parent of this commit */
  parent = ostree_commit_get_parent (variant);
  if (parent && !log_commit (repo, parent, TRUE, flags, error))
    goto out;

  ret = TRUE;
out:
  return ret;
}

gboolean
ostree_builtin_log (int           argc,
                    char        **argv,
                    OstreeRepo   *repo,
                    GCancellable *cancellable,
                    GError      **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  const char *rev;
  gs_free char *checksum = NULL;
  OstreeDumpFlags flags = OSTREE_DUMP_NONE;

  context = g_option_context_new ("REF - Show log starting at commit or ref");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (opt_raw)
    flags |= OSTREE_DUMP_RAW;

  if (argc <= 1)
    {
      ot_util_usage_error (context, "A ref argument is required", error);
      goto out;
    }
  rev = argv[1];

  if (!ostree_repo_resolve_rev (repo, rev, FALSE, &checksum, error))
    goto out;

  if (!log_commit (repo, checksum, FALSE, flags, error))
    goto out;

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
