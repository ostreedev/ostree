/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-builtins.h"
#include "ostree.h"

#include <glib/gi18n.h>

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ostree_builtin_show (int argc, char **argv, const char *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  const char *rev = "master";
  char *resolved_rev = NULL;
  OstreeSerializedVariantType type;
  GVariant *variant = NULL;
  char *formatted_variant = NULL;

  context = g_option_context_new ("- Output a metadata object");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (argc > 1)
    rev = argv[1];

  if (!ostree_repo_resolve_rev (repo, rev, &resolved_rev, error))
    goto out;

  if (!ostree_repo_load_variant (repo, resolved_rev, &type, &variant, error))
    goto out;

  g_print ("Object: %s\nType: %d\n", resolved_rev, type);
  formatted_variant = g_variant_print (variant, TRUE);
  g_print ("%s\n", formatted_variant);
 
  ret = TRUE;
 out:
  g_free (resolved_rev);
  if (context)
    g_option_context_free (context);
  g_clear_object (&repo);
  if (variant)
    g_variant_unref (variant);
  g_free (formatted_variant);
  return ret;
}
