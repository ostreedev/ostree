/*
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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
#include "ostree.h"
#include "otutil.h"
#include "ostree-core-private.h"

static gboolean opt_delete;
static char *opt_gpg_homedir;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-gpg-sign.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "delete", 'd', 0, G_OPTION_ARG_NONE, &opt_delete, "Delete signatures having any of the GPG KEY-IDs" },
  { "gpg-homedir", 0, 0, G_OPTION_ARG_FILENAME, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "HOMEDIR" },
  { NULL }
};

static void
usage_error (GOptionContext *context, const char *message, GError **error)
{
  g_autofree char *help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s", help);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
}

static gboolean
delete_signatures (OstreeRepo *repo,
                   const char *commit_checksum,
                   const char * const *key_ids,
                   guint n_key_ids,
                   guint *out_n_deleted,
                   GCancellable *cancellable,
                   GError **error)
{
  GVariantDict metadata_dict;
  g_autoptr(OstreeGpgVerifyResult) result = NULL;
  g_autoptr(GVariant) old_metadata = NULL;
  g_autoptr(GVariant) new_metadata = NULL;
  g_autoptr(GVariant) signature_data = NULL;
  GVariantIter iter;
  GVariant *child;
  GQueue signatures = G_QUEUE_INIT;
  GQueue trash = G_QUEUE_INIT;
  guint n_deleted = 0;
  guint ii;
  gboolean ret = FALSE;
  GError *local_error = NULL;

  /* XXX Should this code be a new OstreeRepo function in libostree?
   *     Feels slightly too low-level here, and I have to know about
   *     the metadata key name and format which are both declared in
   *     ostree-core-private.h.
   *
   *     OTOH, would this really be a useful addition to libostree?
   */

  if (!ostree_repo_read_commit_detached_metadata (repo,
                                                  commit_checksum,
                                                  &old_metadata,
                                                  cancellable,
                                                  error))
    goto out;

  g_variant_dict_init (&metadata_dict, old_metadata);

  signature_data = g_variant_dict_lookup_value (&metadata_dict,
                                                _OSTREE_METADATA_GPGSIGS_NAME,
                                                G_VARIANT_TYPE ("aay"));

  /* Taking the approach of deleting whatever matches we find for the
   * provided key IDs, even if we don't find a match for EVERY key ID.
   * So no signatures means no matches, which is okay... I guess. */
  if (signature_data == NULL)
    {
      g_variant_dict_clear (&metadata_dict);
      goto shortcut;
    }

  /* Parse the signatures on this commit by running a verify operation
   * on it.  Use the result to match key IDs to signatures for deletion.
   *
   * XXX Reading detached metadata from disk twice here.  Another reason
   *     to move this into libostree?
   */
  result = ostree_repo_verify_commit_ext (repo, commit_checksum,
                                          NULL, NULL,
                                          cancellable, &local_error);
  if (result == NULL)
    {
      g_variant_dict_clear (&metadata_dict);
      goto out;
    }

  /* Convert the GVariant array to a GQueue. */
  g_variant_iter_init (&iter, signature_data);
  while ((child = g_variant_iter_next_value (&iter)) != NULL)
    {
      /* Takes ownership of the child. */
      g_queue_push_tail (&signatures, child);
    }

  /* Signature count and ordering of signatures in the GQueue and
   * OstreeGpgVerifyResult must agree.  We use this below to mark
   * items in the GQueue for deletion based on the index returned
   * by ostree_gpg_verify_result_lookup(). */
  g_assert_cmpuint (ostree_gpg_verify_result_count_all (result), ==, signatures.length);

  /* Build a trash queue which points at nodes in the signature queue. */
  for (ii = 0; ii < n_key_ids; ii++)
    {
      guint index;

      if (ostree_gpg_verify_result_lookup (result, key_ids[ii], &index))
        {
          GList *link = g_queue_peek_nth_link (&signatures, index);

          /* Avoid duplicates in the trash queue. */
          if (g_queue_find (&trash, link) == NULL)
            g_queue_push_tail (&trash, link);
        }
    }

  n_deleted = trash.length;

  /* Reduce the signature queue by emptying the trash. */
  while (!g_queue_is_empty (&trash))
    {
      GList *link = g_queue_pop_head (&trash);
      g_variant_unref (link->data);
      g_queue_delete_link (&signatures, link);
    }

  /* Update the metadata dictionary. */
  if (g_queue_is_empty (&signatures))
    {
      g_variant_dict_remove (&metadata_dict, _OSTREE_METADATA_GPGSIGS_NAME);
    }
  else
    {
      GVariantBuilder signature_builder;

      g_variant_builder_init (&signature_builder, G_VARIANT_TYPE ("aay"));

      while (!g_queue_is_empty (&signatures))
        {
          GVariant *child = g_queue_pop_head (&signatures);
          g_variant_builder_add_value (&signature_builder, child);
          g_variant_unref (child);
        }

      g_variant_dict_insert_value (&metadata_dict,
                                   _OSTREE_METADATA_GPGSIGS_NAME,
                                   g_variant_builder_end (&signature_builder));
    }

  /* Commit the new metadata. */
  new_metadata = g_variant_dict_end (&metadata_dict);
  if (!ostree_repo_write_commit_detached_metadata (repo,
                                                   commit_checksum,
                                                   new_metadata,
                                                   cancellable,
                                                   error))
    goto out;

shortcut:

  if (out_n_deleted != NULL)
    *out_n_deleted = n_deleted;

  ret = TRUE;

out:
  return ret;
}

gboolean
ostree_builtin_gpg_sign (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  g_autofree char *resolved_commit = NULL;
  const char *commit;
  char **key_ids;
  int n_key_ids, ii;
  gboolean ret = FALSE;

  context = g_option_context_new ("COMMIT KEY-ID... - Sign a commit");

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "Need a COMMIT to sign", error);
      goto out;
    }

  if (argc < 3)
    {
      usage_error (context, "Need at least one GPG KEY-ID to sign with", error);
      goto out;
    }

  commit = argv[1];
  key_ids = argv + 2;
  n_key_ids = argc - 2;

  if (!ostree_repo_resolve_rev (repo, commit, FALSE, &resolved_commit, error))
    goto out;

  if (opt_delete)
    {
      guint n_deleted = 0;

      if (delete_signatures (repo, resolved_commit,
                             (const char * const *) key_ids, n_key_ids,
                             &n_deleted, cancellable, error))
        {
          g_print ("Signatures deleted: %u\n", n_deleted);
          ret = TRUE;
        }

      goto out;
    }

  for (ii = 0; ii < n_key_ids; ii++)
    {
      if (!ostree_repo_sign_commit (repo, resolved_commit, key_ids[ii],
                                    opt_gpg_homedir, cancellable, error))
        goto out;
    }

  ret = TRUE;

out:
  return ret;
}
