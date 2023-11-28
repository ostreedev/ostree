/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ostree.h"
#include "ot-builtins.h"
#include "ot-dump.h"
#include "otutil.h"

static gboolean opt_print_related;
static char *opt_print_variant_type;
static char *opt_print_metadata_key;
static char *opt_print_detached_metadata_key;
static gboolean opt_list_metadata_keys;
static gboolean opt_list_detached_metadata_keys;
static gboolean opt_print_sizes;
static gboolean opt_raw;
static gboolean opt_print_hex;
static gboolean opt_no_byteswap;
static char *opt_gpg_homedir;
static char *opt_gpg_verify_remote;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-show.xml) when changing the option list.
 */

static GOptionEntry options[]
    = { { "print-related", 0, 0, G_OPTION_ARG_NONE, &opt_print_related,
          "Show the \"related\" commits", NULL },
        { "print-variant-type", 0, 0, G_OPTION_ARG_STRING, &opt_print_variant_type,
          "Memory map OBJECT (in this case a filename) to the GVariant type string", "TYPE" },
        { "list-metadata-keys", 0, 0, G_OPTION_ARG_NONE, &opt_list_metadata_keys,
          "List the available metadata keys", NULL },
        { "print-metadata-key", 0, 0, G_OPTION_ARG_STRING, &opt_print_metadata_key,
          "Print string value of metadata key", "KEY" },
        {
            "print-hex",
            0,
            0,
            G_OPTION_ARG_NONE,
            &opt_print_hex,
            "For byte array valued keys, output an unquoted hexadecimal string",
            NULL,
        },
        { "list-detached-metadata-keys", 0, 0, G_OPTION_ARG_NONE, &opt_list_detached_metadata_keys,
          "List the available detached metadata keys", NULL },
        { "print-detached-metadata-key", 0, 0, G_OPTION_ARG_STRING,
          &opt_print_detached_metadata_key, "Print string value of detached metadata key", "KEY" },
        { "print-sizes", 0, 0, G_OPTION_ARG_NONE, &opt_print_sizes, "Show the commit size metadata",
          NULL },
        { "raw", 0, 0, G_OPTION_ARG_NONE, &opt_raw, "Show raw variant data" },
        { "no-byteswap", 'B', 0, G_OPTION_ARG_NONE, &opt_no_byteswap,
          "Do not automatically convert variant data from big endian" },
        { "gpg-homedir", 0, 0, G_OPTION_ARG_FILENAME, &opt_gpg_homedir,
          "GPG Homedir to use when looking for keyrings", "HOMEDIR" },
        { "gpg-verify-remote", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_verify_remote,
          "Use REMOTE name for GPG configuration", "REMOTE" },
        { NULL } };

static gboolean
do_print_variant_generic (const GVariantType *type, const char *filename, GError **error)
{
  g_autoptr (GVariant) variant = NULL;

  glnx_autofd int fd = -1;
  if (!glnx_openat_rdonly (AT_FDCWD, filename, TRUE, &fd, error))
    return FALSE;
  if (!ot_variant_read_fd (fd, 0, type, FALSE, &variant, error))
    return FALSE;

  ot_dump_variant (variant);
  return TRUE;
}

static gboolean
do_print_related (OstreeRepo *repo, const char *rev, const char *resolved_rev, GError **error)
{
  g_autoptr (GVariant) variant = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, resolved_rev, &variant, error))
    return FALSE;

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  g_autoptr (GVariant) related = g_variant_get_child_value (variant, 2);
  g_autoptr (GVariantIter) viter = g_variant_iter_new (related);

  const char *name;
  GVariant *csum_v;
  while (g_variant_iter_loop (viter, "(&s@ay)", &name, &csum_v))
    {
      g_autofree char *checksum = ostree_checksum_from_bytes_v (csum_v);
      g_print ("%s %s\n", name, checksum);
    }
  return TRUE;
}

static gboolean
get_metadata (OstreeRepo *repo, const char *resolved_rev, gboolean detached,
              GVariant **out_metadata, GError **error)
{
  g_assert (out_metadata != NULL);

  g_autoptr (GVariant) commit = NULL;
  g_autoptr (GVariant) metadata = NULL;

  if (!detached)
    {
      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, resolved_rev, &commit, error))
        return FALSE;
      /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
      metadata = g_variant_get_child_value (commit, 0);
    }
  else
    {
      if (!ostree_repo_read_commit_detached_metadata (repo, resolved_rev, &metadata, NULL, error))
        return FALSE;
      if (metadata == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "No detached metadata for commit %s", resolved_rev);
          return FALSE;
        }
    }

  *out_metadata = g_steal_pointer (&metadata);

  return TRUE;
}

static gint
strptr_cmp (gconstpointer a, gconstpointer b)
{
  const char *a_str = *((const char **)a);
  const char *b_str = *((const char **)b);

  return g_strcmp0 (a_str, b_str);
}

static gboolean
do_list_metadata_keys (OstreeRepo *repo, const char *resolved_rev, gboolean detached,
                       GError **error)
{
  g_autoptr (GVariant) metadata = NULL;
  if (!get_metadata (repo, resolved_rev, detached, &metadata, error))
    return FALSE;

  GVariantIter iter;
  const char *key = NULL;
  g_autoptr (GPtrArray) keys = g_ptr_array_new ();
  g_variant_iter_init (&iter, metadata);
  while (g_variant_iter_loop (&iter, "{&s@v}", &key, NULL))
    g_ptr_array_add (keys, (gpointer)key);

  g_ptr_array_sort (keys, strptr_cmp);
  for (guint i = 0; i < keys->len; i++)
    {
      key = keys->pdata[i];
      g_print ("%s\n", key);
    }

  return TRUE;
}

static gboolean
do_print_metadata_key (OstreeRepo *repo, const char *resolved_rev, gboolean detached,
                       const char *key, GError **error)
{
  g_autoptr (GVariant) metadata = NULL;
  if (!get_metadata (repo, resolved_rev, detached, &metadata, error))
    return FALSE;

  g_autoptr (GVariant) value = g_variant_lookup_value (metadata, key, NULL);
  if (!value)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No such metadata key '%s'", key);
      return FALSE;
    }

  if (opt_print_hex && g_variant_is_of_type (value, (GVariantType *)"ay"))
    {
      g_autofree char *buf = g_malloc (g_variant_get_size (value) * 2 + 1);
      ot_bin2hex (buf, g_variant_get_data (value), g_variant_get_size (value));
      g_print ("%s\n", buf);
      return TRUE;
    }

  if (opt_no_byteswap)
    {
      g_autofree char *formatted = g_variant_print (value, TRUE);
      g_print ("%s\n", formatted);
    }
  else
    ot_dump_variant (value);
  return TRUE;
}

static gboolean
do_print_sizes (OstreeRepo *repo, const char *rev, GError **error)
{
  g_autoptr (GVariant) commit = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, rev, &commit, error))
    {
      g_prefix_error (error, "Failed to read commit: ");
      return FALSE;
    }

  g_autoptr (GPtrArray) sizes = NULL;
  if (!ostree_commit_get_object_sizes (commit, &sizes, error))
    return FALSE;

  gint64 new_archived = 0;
  gint64 new_unpacked = 0;
  gsize new_objects = 0;
  gint64 archived = 0;
  gint64 unpacked = 0;
  gsize objects = 0;
  for (guint i = 0; i < sizes->len; i++)
    {
      OstreeCommitSizesEntry *entry = sizes->pdata[i];

      archived += entry->archived;
      unpacked += entry->unpacked;
      objects++;

      gboolean exists;
      if (!ostree_repo_has_object (repo, entry->objtype, entry->checksum, &exists, NULL, error))
        return FALSE;

      if (!exists)
        {
          /* Object not in local repo */
          new_archived += entry->archived;
          new_unpacked += entry->unpacked;
          new_objects++;
        }
    }

  g_autofree char *new_archived_str = g_format_size (new_archived);
  g_autofree char *archived_str = g_format_size (archived);
  g_autofree char *new_unpacked_str = g_format_size (new_unpacked);
  g_autofree char *unpacked_str = g_format_size (unpacked);
  g_print ("Compressed size (needed/total): %s/%s\n"
           "Unpacked size (needed/total): %s/%s\n"
           "Number of objects (needed/total): %" G_GSIZE_FORMAT "/%" G_GSIZE_FORMAT "\n",
           new_archived_str, archived_str, new_unpacked_str, unpacked_str, new_objects, objects);

  return TRUE;
}

static gboolean
print_object (OstreeRepo *repo, OstreeObjectType objtype, const char *checksum, GError **error)
{
  OstreeDumpFlags flags = OSTREE_DUMP_NONE;

  g_autoptr (GVariant) variant = NULL;
  if (!ostree_repo_load_variant (repo, objtype, checksum, &variant, error))
    return FALSE;
  if (opt_raw)
    flags |= OSTREE_DUMP_RAW;
  if (opt_no_byteswap)
    flags |= OSTREE_DUMP_UNSWAPPED;
  ot_dump_object (objtype, checksum, variant, flags);

#ifndef OSTREE_DISABLE_GPGME
  if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
    {
      g_autoptr (OstreeGpgVerifyResult) result = NULL;
      g_autoptr (GError) local_error = NULL;
      g_autoptr (GFile) gpg_homedir
          = opt_gpg_homedir ? g_file_new_for_path (opt_gpg_homedir) : NULL;

      if (opt_gpg_verify_remote)
        {
          result = ostree_repo_verify_commit_for_remote (repo, checksum, opt_gpg_verify_remote,
                                                         NULL, &local_error);
        }
      else
        {
          result = ostree_repo_verify_commit_ext (repo, checksum, gpg_homedir, NULL, NULL,
                                                  &local_error);
        }

      if (g_error_matches (local_error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_NO_SIGNATURE))
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

          g_autoptr (GString) buffer = g_string_sized_new (256);
          for (guint ii = 0; ii < n_sigs; ii++)
            {
              g_string_append_c (buffer, '\n');
              ostree_gpg_verify_result_describe (result, ii, buffer, "  ",
                                                 OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);
            }

          g_print ("%s", buffer->str);
        }
    }
#endif /* OSTREE_DISABLE_GPGME */

  return TRUE;
}

static gboolean
print_if_found (OstreeRepo *repo, OstreeObjectType objtype, const char *checksum,
                gboolean *inout_was_found, GCancellable *cancellable, GError **error)
{
  gboolean have_object = FALSE;

  if (*inout_was_found)
    return TRUE;

  if (!ostree_repo_has_object (repo, objtype, checksum, &have_object, cancellable, error))
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
ostree_builtin_show (int argc, char **argv, OstreeCommandInvocation *invocation,
                     GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("OBJECT");

  g_autoptr (OstreeRepo) repo = NULL;
  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable,
                                    error))
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
  else if (opt_list_metadata_keys || opt_list_detached_metadata_keys)
    {
      if (!ostree_repo_resolve_rev (repo, rev, FALSE, &resolved_rev, error))
        return FALSE;
      if (!do_list_metadata_keys (repo, resolved_rev, opt_list_detached_metadata_keys, error))
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
  else if (opt_print_sizes)
    {
      if (!ostree_repo_resolve_rev (repo, rev, FALSE, &resolved_rev, error))
        return FALSE;

      if (!do_print_sizes (repo, resolved_rev, error))
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
          if (!print_if_found (repo, OSTREE_OBJECT_TYPE_COMMIT, rev, &found, cancellable, error))
            return FALSE;
          if (!print_if_found (repo, OSTREE_OBJECT_TYPE_DIR_META, rev, &found, cancellable, error))
            return FALSE;
          if (!print_if_found (repo, OSTREE_OBJECT_TYPE_DIR_TREE, rev, &found, cancellable, error))
            return FALSE;
          if (!found)
            {
              g_autoptr (GFileInfo) finfo = NULL;
              g_autoptr (GVariant) xattrs = NULL;

              if (!ostree_repo_load_file (repo, rev, NULL, &finfo, &xattrs, cancellable, error))
                return FALSE;

              g_print ("Object: %s\nType: %s\n", rev,
                       ostree_object_type_to_string (OSTREE_OBJECT_TYPE_FILE));
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
