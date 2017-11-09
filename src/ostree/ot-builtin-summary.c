/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include "ostree-repo-private.h"
#include "ot-dump.h"
#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

static gboolean opt_update, opt_view, opt_raw;
static char **opt_key_ids;
static char *opt_gpg_homedir;
static char **opt_metadata;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-summary.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "update", 'u', 0, G_OPTION_ARG_NONE, &opt_update, "Update the summary", NULL },
  { "view", 'v', 0, G_OPTION_ARG_NONE, &opt_view, "View the local summary file", NULL },
  { "raw", 0, 0, G_OPTION_ARG_NONE, &opt_raw, "View the raw bytes of the summary file", NULL },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_key_ids, "GPG Key ID to sign the summary with", "KEY-ID"},
  { "gpg-homedir", 0, 0, G_OPTION_ARG_FILENAME, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "HOMEDIR"},
  { "add-metadata", 'm', 0, G_OPTION_ARG_STRING_ARRAY, &opt_metadata, "Additional metadata field to add to the summary", "KEY=VALUE" },
  { NULL }
};

/* Take arguments of the form KEY=VALUE and put them into an a{sv} variant. The
 * value arguments must be parsable using g_variant_parse(). */
static GVariant *
build_additional_metadata (const char * const  *args,
                           GError             **error)
{
  g_autoptr(GVariantBuilder) builder = NULL;

  builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);

  for (gsize i = 0; args[i] != NULL; i++)
    {
      const gchar *equals = strchr (args[i], '=');
      g_autofree gchar *key = NULL;
      const gchar *value_str;
      g_autoptr(GVariant) value = NULL;

      if (equals == NULL)
        return glnx_null_throw (error,
                                "Missing '=' in KEY=VALUE metadata '%s'", args[i]);

      key = g_strndup (args[i], equals - args[i]);
      value_str = equals + 1;

      value = g_variant_parse (NULL, value_str, NULL, NULL, error);
      if (value == NULL)
        return glnx_prefix_error_null (error, "Error parsing variant ‘%s’: ", value_str);

      g_variant_builder_add (builder, "{sv}", key, value);
    }

  return g_variant_ref_sink (g_variant_builder_end (builder));
}

gboolean
ostree_builtin_summary (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  OstreeDumpFlags flags = OSTREE_DUMP_NONE;

  context = g_option_context_new ("");

  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    return FALSE;

  if (opt_update)
    {
      g_autoptr(GVariant) additional_metadata = NULL;

      if (!ostree_ensure_repo_writable (repo, error))
        return FALSE;

      if (opt_metadata != NULL)
        {
          additional_metadata = build_additional_metadata ((const char * const *) opt_metadata, error);
          if (additional_metadata == NULL)
            return FALSE;
        }

#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
      const char *collection_id = ostree_repo_get_collection_id (repo);
#else  /* if !OSTREE_ENABLE_EXPERIMENTAL_API */
      const char *collection_id = NULL;
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */

      /* Write out a new metadata commit for the repository. */
      if (collection_id != NULL)
        {
#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
          OstreeCollectionRef collection_ref = { (gchar *) collection_id, (gchar *) OSTREE_REPO_METADATA_REF };
          g_autofree char *old_ostree_metadata_checksum = NULL;
          g_autofree gchar *new_ostree_metadata_checksum = NULL;
          g_autoptr(OstreeMutableTree) mtree = NULL;
          g_autoptr(OstreeRepoFile) repo_file = NULL;
          g_autoptr(GVariantDict) new_summary_commit_dict = NULL;
          g_autoptr(GVariant) new_summary_commit = NULL;

          if (!ostree_repo_resolve_rev (repo, OSTREE_REPO_METADATA_REF,
                                        TRUE, &old_ostree_metadata_checksum, error))
            return FALSE;

          /* Add bindings to the metadata. */
          new_summary_commit_dict = g_variant_dict_new (additional_metadata);
          g_variant_dict_insert (new_summary_commit_dict, OSTREE_COMMIT_META_KEY_COLLECTION_BINDING,
                                 "s", collection_ref.collection_id);
          g_variant_dict_insert_value (new_summary_commit_dict, OSTREE_COMMIT_META_KEY_REF_BINDING,
                                       g_variant_new_strv ((const gchar * const *) &collection_ref.ref_name, 1));
          new_summary_commit = g_variant_dict_end (new_summary_commit_dict);

          if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
            return FALSE;

          /* Set up an empty mtree. */
          mtree = ostree_mutable_tree_new ();

          glnx_unref_object GFileInfo *fi = g_file_info_new ();
          g_file_info_set_attribute_uint32 (fi, "unix::uid", 0);
          g_file_info_set_attribute_uint32 (fi, "unix::gid", 0);
          g_file_info_set_attribute_uint32 (fi, "unix::mode", (0755 | S_IFDIR));

          g_autofree guchar *csum_raw = NULL;
          g_autofree char *csum = NULL;

          g_autoptr(GVariant) dirmeta = ostree_create_directory_metadata (fi, NULL /* xattrs */);

          if (!ostree_repo_write_metadata (repo, OSTREE_OBJECT_TYPE_DIR_META, NULL,
                                           dirmeta, &csum_raw, cancellable, error))
            return FALSE;

          csum = ostree_checksum_from_bytes (csum_raw);
          ostree_mutable_tree_set_metadata_checksum (mtree, csum);

          if (!ostree_repo_write_mtree (repo, mtree, (GFile **) &repo_file, NULL, error))
            return FALSE;

          if (!ostree_repo_write_commit (repo, old_ostree_metadata_checksum,
                                         NULL  /* subject */, NULL  /* body */,
                                         new_summary_commit, repo_file, &new_ostree_metadata_checksum,
                                         NULL, error))
            return FALSE;

          if (opt_key_ids != NULL)
            {
              for (const char * const *iter = (const char * const *) opt_key_ids;
                   iter != NULL && *iter != NULL; iter++)
                {
                  const char *key_id = *iter;

                  if (!ostree_repo_sign_commit (repo,
                                                new_ostree_metadata_checksum,
                                                key_id,
                                                opt_gpg_homedir,
                                                cancellable,
                                                error))
                    return FALSE;
                }
            }

          ostree_repo_transaction_set_collection_ref (repo, &collection_ref,
                                                      new_ostree_metadata_checksum);

          if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
            return FALSE;
#else  /* if !OSTREE_ENABLE_EXPERIMENTAL_API */
          g_assert_not_reached ();
          return FALSE;
#endif  /* OSTREE_ENABLE_EXPERIMENTAL_API */
        }

      /* Regenerate and sign the conventional summary file. */
      if (!ostree_repo_regenerate_summary (repo, additional_metadata, cancellable, error))
        return FALSE;

      if (opt_key_ids)
        {
          if (!ostree_repo_add_gpg_signature_summary (repo,
                                                      (const gchar **) opt_key_ids,
                                                      opt_gpg_homedir,
                                                      cancellable,
                                                      error))
            return FALSE;
        }
    }
  else if (opt_view || opt_raw)
    {
      g_autoptr(GBytes) summary_data = NULL;

      if (opt_raw)
        flags |= OSTREE_DUMP_RAW;

      glnx_autofd int fd = -1;
      if (!glnx_openat_rdonly (repo->repo_dir_fd, "summary", TRUE, &fd, error))
        return FALSE;
      summary_data = ot_fd_readall_or_mmap (fd, 0, error);
      if (!summary_data)
        return FALSE;

      ot_dump_summary_bytes (summary_data, flags);
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No option specified; use -u to update summary");
      return FALSE;
    }

  return TRUE;
}
