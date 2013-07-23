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

static gboolean opt_print_related;
static char* opt_print_variant_type;
static char* opt_print_metadata_key;

static GOptionEntry options[] = {
  { "print-related", 0, 0, G_OPTION_ARG_NONE, &opt_print_related, "If given, show the \"related\" commits", NULL },
  { "print-variant-type", 0, 0, G_OPTION_ARG_STRING, &opt_print_variant_type, "If given, argument should be a filename and it will be interpreted as this type", NULL },
  { "print-metadata-key", 0, 0, G_OPTION_ARG_STRING, &opt_print_metadata_key, "Print string value of metadata key KEY for given commit", "KEY" },
  { NULL }
};

static void
print_variant (GVariant *variant)
{
  gs_free char *formatted_variant = NULL;
  gs_unref_variant GVariant *byteswapped = NULL;

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
}

static gboolean
do_print_variant_generic (const GVariantType *type,
                          const char *filename,
                          GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *f = NULL;
  gs_unref_variant GVariant *variant = NULL;

  f = g_file_new_for_path (filename);

  if (!ot_util_variant_map (f, type, TRUE, &variant, error))
    goto out;

  print_variant (variant);

  ret = TRUE;
 out:
  return ret;
}

static gboolean
show_repo_meta (OstreeRepo  *repo,
                const char *rev,
                const char *resolved_rev,
                GCancellable   *cancellable,
                GError **error)
{
  gboolean ret = FALSE;
  OstreeObjectType objtype;
  gs_unref_variant GVariant *variant = NULL;
  gs_unref_object GFile *object_path = NULL;
  gs_unref_object GInputStream *in = NULL;

  for (objtype = OSTREE_OBJECT_TYPE_FILE; objtype <= OSTREE_OBJECT_TYPE_COMMIT; objtype++)
    {
      g_clear_object (&object_path);
      
      object_path = ostree_repo_get_object_path (repo, resolved_rev, objtype);
      
      if (!g_file_query_exists (object_path, NULL))
        continue;
      
      g_print ("Object: %s\nType: %s\n", resolved_rev, ostree_object_type_to_string (objtype));

      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        {
          if (!ostree_repo_load_variant (repo, objtype, resolved_rev, &variant, error))
            continue;

          print_variant (variant);
          break;
        }
      else if (objtype == OSTREE_OBJECT_TYPE_FILE)
        {
          gs_unref_object GFileInfo *finfo = NULL;
          gs_unref_variant GVariant *xattrs = NULL;
          GFileType filetype;
          
          if (!ostree_repo_load_file (repo, resolved_rev, NULL, &finfo, &xattrs,
                                      cancellable, error))
            goto out;

          filetype = g_file_info_get_file_type (finfo);
          g_print ("File Type: ");
          switch (filetype)
            {
            case G_FILE_TYPE_REGULAR:
              g_print ("regular\n");
              g_print ("Size: %" G_GUINT64_FORMAT "\n", g_file_info_get_size (finfo));
              break;
            case G_FILE_TYPE_SYMBOLIC_LINK:
              g_print ("symlink\n");
              g_print ("Target: %s\n", g_file_info_get_symlink_target (finfo));
              break;
            default:
              g_printerr ("(unknown type %u)\n", (guint)filetype);
            }

          g_print ("Mode: 0%04o\n", g_file_info_get_attribute_uint32 (finfo, "unix::mode"));
          g_print ("Uid: %u\n", g_file_info_get_attribute_uint32 (finfo, "unix::uid"));
          g_print ("Gid: %u\n", g_file_info_get_attribute_uint32 (finfo, "unix::gid"));

          g_print ("Extended Attributes: ");
          if (xattrs)
            {
              gs_free char *xattr_string = g_variant_print (xattrs, TRUE);
              g_print ("{ %s }\n", xattr_string);
            }
          else
            {
              g_print ("(none)\n");
            }
        }
      else
        g_assert_not_reached ();
    }


  ret = TRUE;
 out:
  return ret;
}

static gboolean
do_print_related (OstreeRepo  *repo,
                  const char *rev,
                  const char *resolved_rev,
                  GError **error)
{
  gboolean ret = FALSE;
  const char *name;
  gs_unref_variant GVariant *csum_v = NULL;
  gs_unref_variant GVariant *variant = NULL;
  gs_unref_variant GVariant *related = NULL;
  GVariantIter *viter = NULL;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 resolved_rev, &variant, error))
    goto out;
      
  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  related = g_variant_get_child_value (variant, 2);
  
  viter = g_variant_iter_new (related);

  while (g_variant_iter_loop (viter, "(&s@ay)", &name, &csum_v))
    {
      gs_free char *checksum = ostree_checksum_from_bytes_v (csum_v);
      g_print ("%s %s\n", name, checksum);
    }
  csum_v = NULL;

  ret = TRUE;
 out:
  if (viter)
    g_variant_iter_free (viter);
  return ret;
}

static gboolean
do_print_metadata_key (OstreeRepo  *repo,
                       const char *resolved_rev,
                       const char *key,
                       GError **error)
{
  gboolean ret = FALSE;
  const char *value;
  gs_unref_variant GVariant *commit = NULL;
  gs_unref_variant GVariant *metadata = NULL;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 resolved_rev, &commit, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  metadata = g_variant_get_child_value (commit, 1);
  
  if (!g_variant_lookup (metadata, key, "&s", &value))
    goto out;

  g_print ("%s\n", value);

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_builtin_show (int argc, char **argv, GFile *repo_path, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  const char *rev;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_free char *resolved_rev = NULL;

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

  if (opt_print_metadata_key)
    {
      if (!ostree_repo_resolve_rev (repo, rev, FALSE, &resolved_rev, error))
        goto out;

      if (!do_print_metadata_key (repo, resolved_rev, opt_print_metadata_key, error))
        goto out;
    }
  else if (opt_print_related)
    {
      if (!ostree_repo_resolve_rev (repo, rev, FALSE, &resolved_rev, error))
        goto out;

      if (!do_print_related (repo, rev, resolved_rev, error))
        goto out;
    }
  else if (opt_print_variant_type)
    {
      if (!do_print_variant_generic (G_VARIANT_TYPE (opt_print_variant_type), rev, error))
        goto out;
    }
  else
    {
      if (!ostree_repo_resolve_rev (repo, rev, FALSE, &resolved_rev, error))
        goto out;

      if (!show_repo_meta (repo, rev, resolved_rev, cancellable, error))
        goto out;
    }
 
  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
