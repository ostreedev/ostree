/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

#include "ot-builtins.h"
#include "ostree.h"

#include <glib/gi18n.h>

static gboolean print_packfile;
static gboolean print_compose;
static char* print_variant_type;

static GOptionEntry options[] = {
  { "print-packfile", 0, 0, G_OPTION_ARG_NONE, &print_packfile, "If given, argument given is a packfile", NULL },
  { "print-compose", 0, 0, G_OPTION_ARG_NONE, &print_compose, "If given, show the branches which make up the given compose commit", NULL },
  { "print-variant-type", 0, 0, G_OPTION_ARG_STRING, &print_variant_type, "If given, argument should be a filename and it will be interpreted as this type", NULL },
  { NULL }
};

static void
print_variant (GVariant *variant)
{
  char *formatted_variant = NULL;
  GVariant *byteswapped = NULL;

  if (G_BYTE_ORDER != G_BIG_ENDIAN)
    {
      byteswapped = g_variant_byteswap (variant);
      formatted_variant = g_variant_print (byteswapped, TRUE);
    }
  else
    {
      formatted_variant = g_variant_print (variant, TRUE);
    }
  g_print ("%s\n", formatted_variant);

  g_free (formatted_variant);
  if (byteswapped)
    g_variant_unref (byteswapped);
}

static gboolean
do_print_variant_generic (const GVariantType *type,
                          const char *filename,
                          GError **error)
{
  gboolean ret = FALSE;
  GFile *f = NULL;
  GVariant *variant = NULL;

  f = ot_util_new_file_for_path (filename);

  if (!ot_util_variant_map (f, type, &variant, error))
    goto out;

  print_variant (variant);

  ret = TRUE;
 out:
  if (variant)
    g_variant_unref (variant);
  g_clear_object (&f);
  return ret;
}

static gboolean
show_repo_meta (OstreeRepo  *repo,
                const char *rev,
                const char *resolved_rev,
                GError **error)
{
  OstreeSerializedVariantType type;
  gboolean ret = FALSE;
  GVariant *variant = NULL;

  if (!ostree_repo_load_variant (repo, resolved_rev, &type, &variant, error))
    goto out;
  g_print ("Object: %s\nType: %d\n", resolved_rev, type);
  print_variant (variant);

  ret = TRUE;
 out:
  if (variant)
    g_variant_unref (variant);
  return ret;
}

static gboolean
do_print_packfile (OstreeRepo  *repo,
                   const char *checksum,
                   GError **error)
{
  gboolean ret = FALSE;
  GVariant *variant = NULL;
  char *path = NULL;
  GInputStream *content = NULL;
  GFile *file = NULL;

  path = ostree_repo_get_object_path (repo, checksum, OSTREE_OBJECT_TYPE_FILE);
  if (!path)
    goto out;
  file = ot_util_new_file_for_path (path);

  if (!ostree_parse_packed_file (file, &variant, &content, NULL, error))
    goto out;
  
  print_variant (variant);

  ret = TRUE;
 out:
  g_free (path);
  g_clear_object (&file);
  g_clear_object (&content);
  if (variant)
    g_variant_unref (variant);
  return ret;
}

static gboolean
do_print_compose (OstreeRepo  *repo,
                  const char *rev,
                  const char *resolved_rev,
                  GError **error)
{
  gboolean ret = FALSE;
  GVariant *variant = NULL;
  GVariant *metadata = NULL;
  GVariant *compose_contents = NULL;
  GVariantIter *viter = NULL;
  GHashTable *metadata_hash = NULL;
  const char *branch;
  const char *branchrev;

  if (!ostree_repo_load_variant_checked (repo, OSTREE_SERIALIZED_COMMIT_VARIANT,
                                         resolved_rev, &variant, error))
    goto out;
      
  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  metadata = g_variant_get_child_value (variant, 1);
  metadata_hash = ot_util_variant_asv_to_hash_table (metadata);
  
  compose_contents = g_hash_table_lookup (metadata_hash, "ostree-compose");
  if (!compose_contents)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Commit %s does not have compose metadata key \"ostree-compose\"", resolved_rev);
      goto out;
    }

  g_variant_get_child (compose_contents, 0, "a(ss)", &viter);
  while (g_variant_iter_next (viter, "(&s&s)", &branch, &branchrev))
    {
      g_print ("%s %s\n", branch, branchrev);
    }

  ret = TRUE;
 out:
  if (variant)
    g_variant_unref (variant);
  if (viter)
    g_variant_iter_free (viter);
  if (metadata)
    g_variant_unref (metadata);
  if (metadata_hash)
    g_hash_table_destroy (metadata_hash);
  return ret;
}

gboolean
ostree_builtin_show (int argc, char **argv, const char *repo_path, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  OstreeRepo *repo = NULL;
  const char *rev = "master";
  char *resolved_rev = NULL;

  context = g_option_context_new ("- Output a metadata object");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (argc > 1)
    rev = argv[1];

  if (print_packfile)
    {
      if (!do_print_packfile (repo, rev, error))
        goto out;
    }
  else if (print_compose)
    {
      if (!ostree_repo_resolve_rev (repo, rev, &resolved_rev, error))
        goto out;

      if (!do_print_compose (repo, rev, resolved_rev, error))
        goto out;
    }
  else if (print_variant_type)
    {
      if (!do_print_variant_generic (G_VARIANT_TYPE (print_variant_type), rev, error))
        goto out;
    }
  else
    {
      if (!ostree_repo_resolve_rev (repo, rev, &resolved_rev, error))
        goto out;

      if (!show_repo_meta (repo, rev, resolved_rev, error))
        goto out;
    }
 
  ret = TRUE;
 out:
  g_free (resolved_rev);
  if (context)
    g_option_context_free (context);
  g_clear_object (&repo);
  return ret;
}
