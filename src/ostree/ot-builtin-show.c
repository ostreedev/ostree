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

static gboolean print_compose;
static char* print_variant_type;

static GOptionEntry options[] = {
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
  ot_clear_gvariant (&byteswapped);
}

static gboolean
do_print_variant_generic (const GVariantType *type,
                          const char *filename,
                          GError **error)
{
  gboolean ret = FALSE;
  GFile *f = NULL;
  GVariant *variant = NULL;

  f = ot_gfile_new_for_path (filename);

  if (!ot_util_variant_map (f, type, &variant, error))
    goto out;

  print_variant (variant);

  ret = TRUE;
 out:
  ot_clear_gvariant (&variant);
  g_clear_object (&f);
  return ret;
}

static gboolean
show_repo_meta (OstreeRepo  *repo,
                const char *rev,
                const char *resolved_rev,
                GError **error)
{
  gboolean ret = FALSE;
  GVariant *variant = NULL;
  GFile *object_path = NULL;
  GInputStream *in = NULL;
  char buf[8192];
  gsize bytes_read;
  OstreeObjectType objtype;

  for (objtype = OSTREE_OBJECT_TYPE_RAW_FILE; objtype <= OSTREE_OBJECT_TYPE_COMMIT; objtype++)
    {
      g_clear_object (&object_path);
      
      object_path = ostree_repo_get_object_path (repo, resolved_rev, objtype);
      
      if (!g_file_query_exists (object_path, NULL))
        continue;
      
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        {
          if (!ostree_repo_load_variant (repo, objtype, resolved_rev, &variant, error))
            continue;

          g_print ("Object: %s\nType: %d\n", resolved_rev, objtype);
          print_variant (variant);
          break;
        }
      else if (objtype == OSTREE_OBJECT_TYPE_RAW_FILE)
        {
          in = (GInputStream*)g_file_read (object_path, NULL, error);
          if (!in)
            continue;

          do {
            if (!g_input_stream_read_all (in, buf, sizeof (buf), &bytes_read, NULL, error))
              goto out;
            g_print ("%s", buf);
          } while (bytes_read > 0);
        }
      else if (objtype == OSTREE_OBJECT_TYPE_ARCHIVED_FILE_CONTENT)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "Can't show archived files yet");
          goto out;
        }
      else
        g_assert_not_reached ();
    }


  ret = TRUE;
 out:
  ot_clear_gvariant (&variant);
  g_clear_object (&in);
  g_clear_object (&object_path);
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

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
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
  ot_clear_gvariant (&variant);
  if (viter)
    g_variant_iter_free (viter);
  ot_clear_gvariant (&metadata);
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
  const char *rev;
  char *resolved_rev = NULL;

  context = g_option_context_new ("OBJECT - Output a metadata object");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (repo, error))
    goto out;

  if (argc <= 1)
    {
      ot_util_usage_error (context, "An object argument is required", error);
      goto out;
    }
  rev = argv[1];

  if (print_compose)
    {
      if (!ostree_repo_resolve_rev (repo, rev, FALSE, &resolved_rev, error))
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
      if (!ostree_repo_resolve_rev (repo, rev, FALSE, &resolved_rev, error))
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
