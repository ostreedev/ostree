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

#include "ot-main.h"
#include "ot-builtins.h"
#include "ot-dump.h"
#include "ostree.h"
#include "otutil.h"

static gboolean opt_print_related;
static char* opt_print_variant_type;
static char* opt_print_metadata_key;
static char* opt_print_detached_metadata_key;
static gboolean opt_raw;
static char *opt_gpg_homedir;
static char *opt_gpg_verify_remote;

static GOptionEntry options[] = {
  { "print-related", 0, 0, G_OPTION_ARG_NONE, &opt_print_related, "Show the \"related\" commits", NULL },
  { "print-variant-type", 0, 0, G_OPTION_ARG_STRING, &opt_print_variant_type, "Memory map OBJECT (in this case a filename) to the GVariant type string", "TYPE" },
  { "print-metadata-key", 0, 0, G_OPTION_ARG_STRING, &opt_print_metadata_key, "Print string value of metadata key", "KEY" },
  { "print-detached-metadata-key", 0, 0, G_OPTION_ARG_STRING, &opt_print_detached_metadata_key, "Print string value of detached metadata key", "KEY" },
  { "raw", 0, 0, G_OPTION_ARG_NONE, &opt_raw, "Show raw variant data" },
  { "gpg-homedir", 0, 0, G_OPTION_ARG_FILENAME, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "HOMEDIR"},
  { "gpg-verify-remote", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_verify_remote, "Use REMOTE name for GPG configuration", "REMOTE"},
  { NULL }
};

static gboolean
do_print_variant_generic (const GVariantType *type,
                          const char *filename,
                          GError **error)
{
  g_autoptr(GVariant) variant = NULL;

  if (!ot_util_variant_map_at (AT_FDCWD, filename, type, TRUE, &variant, error))
    return FALSE;

  ot_dump_variant (variant);
  return TRUE;
}

static gboolean
do_print_related (OstreeRepo  *repo,
                  const char *rev,
                  const char *resolved_rev,
                  GError **error)
{
  g_autoptr(GVariant) variant = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 resolved_rev, &variant, error))
    return FALSE;

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  g_autoptr(GVariant) related = g_variant_get_child_value (variant, 2);
  g_autoptr(GVariantIter) viter = g_variant_iter_new (related);

  const char *name;
  GVariant* csum_v;
  while (g_variant_iter_loop (viter, "(&s@ay)", &name, &csum_v))
    {
      g_autofree char *checksum = ostree_checksum_from_bytes_v (csum_v);
      g_print ("%s %s\n", name, checksum);
    }
  return TRUE;
}

static gboolean
do_print_metadata_key (OstreeRepo     *repo,
                       const char     *resolved_rev,
                       gboolean        detached,
                       const char     *key,
                       GError        **error)
{
  g_autoptr(GVariant) commit = NULL;
  g_autoptr(GVariant) metadata = NULL;

  if (!detached)
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                     resolved_rev, &commit, error))
        return FALSE;
      /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
      metadata = g_variant_get_child_value (commit, 0);
    }
  else
    {
      if (!ostree_repo_read_commit_detached_metadata (repo, resolved_rev, &metadata,
                                                      NULL, error))
        return FALSE;
      if (metadata == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "No detached metadata for commit %s", resolved_rev);
          return FALSE;
        }
    }

  g_autoptr(GVariant) value = g_variant_lookup_value (metadata, key, NULL);
  if (!value)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No such metadata key '%s'", key);
      return FALSE;
    }

  ot_dump_variant (value);
  return TRUE;
}

static gboolean
print_object (OstreeRepo          *repo,
              OstreeObjectType     objtype,
              const char          *checksum,
              GError             **error)
{
  OstreeDumpFlags flags = OSTREE_DUMP_NONE;

  g_autoptr(GVariant) variant = NULL;
  if (!ostree_repo_load_variant (repo, objtype, checksum,
                                 &variant, error))
    return FALSE;
  if (opt_raw)
    flags |= OSTREE_DUMP_RAW;
  ot_dump_object (objtype, checksum, variant, flags);

  if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
    {
      glnx_unref_object OstreeGpgVerifyResult *result = NULL;
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GFile) gpg_homedir = opt_gpg_homedir ? g_file_new_for_path (opt_gpg_homedir) : NULL;

      if (opt_gpg_verify_remote)
        {
          result = ostree_repo_verify_commit_for_remote (repo, checksum, opt_gpg_verify_remote,
                                                         NULL, &local_error);
        }
      else
        {
          result = ostree_repo_verify_commit_ext (repo, checksum,
                                                  gpg_homedir, NULL, NULL,
                                                  &local_error);
        }

      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          /* Ignore */
        }
      else if (local_error != NULL)
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
      else
        {
          guint n_sigs = ostree_gpg_verify_result_count_all (result);
          g_print ("Found %u signature%s:\n", n_sigs, n_sigs == 1 ? "" : "s");

          g_autoptr(GString) buffer = g_string_sized_new (256);
          for (guint ii = 0; ii < n_sigs; ii++)
            {
              g_string_append_c (buffer, '\n');
              ostree_gpg_verify_result_describe (result, ii, buffer, "  ",
                                                 OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);
            }

          g_print ("%s", buffer->str);
        }
    }

  return TRUE;
}

static gboolean
print_if_found (OstreeRepo        *repo,
                OstreeObjectType   objtype,
                const char        *checksum,
                gboolean          *inout_was_found,
                GCancellable      *cancellable,
                GError           **error)
{
  gboolean have_object = FALSE;

  if (*inout_was_found)
    return TRUE;

  if (!ostree_repo_has_object (repo, objtype, checksum, &have_object,
                               cancellable, error))
    return FALSE;
  if (have_object)
    {
      if (!print_object (repo, objtype, checksum, error))
        return FALSE;
      *inout_was_found = TRUE;
    }

  return TRUE;
}

gboolean
ostree_builtin_show (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("OBJECT - Output a metadata object");

  glnx_unref_object OstreeRepo *repo = NULL;
  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    return FALSE;

  if (argc <= 1)
    {
      ot_util_usage_error (context, "An object argument is required", error);
      return FALSE;
    }
  const char *rev = argv[1];

  g_autofree char *resolved_rev = NULL;
  if (opt_print_metadata_key || opt_print_detached_metadata_key)
    {
      gboolean detached = opt_print_detached_metadata_key != NULL;
      const char *key = detached ? opt_print_detached_metadata_key : opt_print_metadata_key;
      if (!ostree_repo_resolve_rev (repo, rev, FALSE, &resolved_rev, error))
        return FALSE;

      if (!do_print_metadata_key (repo, resolved_rev, detached, key, error))
        return FALSE;
    }
  else if (opt_print_related)
    {
      if (!ostree_repo_resolve_rev (repo, rev, FALSE, &resolved_rev, error))
        return FALSE;

      if (!do_print_related (repo, rev, resolved_rev, error))
        return FALSE;
    }
  else if (opt_print_variant_type)
    {
      if (!do_print_variant_generic (G_VARIANT_TYPE (opt_print_variant_type), rev, error))
        return FALSE;
    }
  else
    {
      gboolean found = FALSE;
      if (!ostree_validate_checksum_string (rev, NULL))
        {
          if (!ostree_repo_resolve_rev (repo, rev, FALSE, &resolved_rev, error))
            return FALSE;
          if (!print_object (repo, OSTREE_OBJECT_TYPE_COMMIT, resolved_rev, error))
            return FALSE;
        }
      else
        {
          if (!print_if_found (repo, OSTREE_OBJECT_TYPE_COMMIT, rev,
                               &found, cancellable, error))
            return FALSE;
          if (!print_if_found (repo, OSTREE_OBJECT_TYPE_DIR_META, rev,
                               &found, cancellable, error))
            return FALSE;
          if (!print_if_found (repo, OSTREE_OBJECT_TYPE_DIR_TREE, rev,
                               &found, cancellable, error))
            return FALSE;
          if (!found)
            {
              g_autoptr(GFileInfo) finfo = NULL;
              g_autoptr(GVariant) xattrs = NULL;

              if (!ostree_repo_load_file (repo, rev, NULL, &finfo, &xattrs,
                                          cancellable, error))
                return FALSE;

              g_print ("Object: %s\nType: %s\n", rev, ostree_object_type_to_string (OSTREE_OBJECT_TYPE_FILE));
              GFileType filetype = g_file_info_get_file_type (finfo);
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
                  g_autofree char *xattr_string = g_variant_print (xattrs, TRUE);
                  g_print ("{ %s }\n", xattr_string);
                }
              else
                {
                  g_print ("(none)\n");
                }
            }
        }
    }

  return TRUE;
}
