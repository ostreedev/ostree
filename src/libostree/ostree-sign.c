/* vim:set et sw=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e2s: */

/*
 * Copyright © 2019 Collabora Ltd.
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
 */

/**
 * SECTION:ostree-sign
 * @title: Signature management
 * @short_description: Sign and verify commits
 *
 * An #OstreeSign interface allows to select and use any available engine
 * for signing or verifying the commit object or summary file.
 */

#include "config.h"

#include "libglnx.h"
#include "otutil.h"
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "ostree-autocleanups.h"
#include "ostree-core.h"
#include "ostree-sign-dummy.h"
#include "ostree-sign-ed25519.h"
#include "ostree-sign-private.h"
#include "ostree-sign.h"

#include "ostree-autocleanups.h"
#include "ostree-repo-private.h"

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "OSTreeSign"

typedef struct
{
  gchar *name;
  GType type;
} _sign_type;

_sign_type sign_types[] = {
#if defined(HAVE_ED25519)
  { OSTREE_SIGN_NAME_ED25519, 0 },
#endif
  { "dummy", 0 }
};

enum
{
#if defined(HAVE_ED25519)
  SIGN_ED25519,
#endif
  SIGN_DUMMY
};

G_DEFINE_INTERFACE (OstreeSign, ostree_sign, G_TYPE_OBJECT)

static void
ostree_sign_default_init (OstreeSignInterface *iface)
{
  g_debug ("OstreeSign initialization");
}

/**
 * ostree_sign_metadata_key:
 * @self: an #OstreeSign object
 *
 * Return the pointer to the name of the key used in (detached) metadata for
 * current signing engine.
 *
 * Returns: (transfer none): pointer to the metadata key name,
 * @NULL in case of error (unlikely).
 *
 * Since: 2020.2
 */
const gchar *
ostree_sign_metadata_key (OstreeSign *self)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (OSTREE_SIGN_GET_IFACE (self)->metadata_key == NULL)
    return NULL;

  return OSTREE_SIGN_GET_IFACE (self)->metadata_key (self);
}

/**
 * ostree_sign_metadata_format:
 * @self: an #OstreeSign object
 *
 * Return the pointer to the string with format used in (detached) metadata for
 * current signing engine.
 *
 * Returns: (transfer none): pointer to the metadata format,
 * @NULL in case of error (unlikely).
 *
 * Since: 2020.2
 */
const gchar *
ostree_sign_metadata_format (OstreeSign *self)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (OSTREE_SIGN_GET_IFACE (self)->metadata_format == NULL)
    return NULL;

  return OSTREE_SIGN_GET_IFACE (self)->metadata_format (self);
}

/**
 * ostree_sign_clear_keys:
 * @self: an #OstreeSign object
 * @error: a #GError
 *
 * Clear all previously preloaded secret and public keys.
 *
 * Returns: @TRUE in case if no errors, @FALSE in case of error
 *
 * Since: 2020.2
 */
gboolean
ostree_sign_clear_keys (OstreeSign *self, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (OSTREE_SIGN_GET_IFACE (self)->clear_keys == NULL)
    return glnx_throw (error, "not implemented");

  return OSTREE_SIGN_GET_IFACE (self)->clear_keys (self, error);
}

/**
 * ostree_sign_set_sk:
 * @self: an #OstreeSign object
 * @secret_key: secret key to be added
 * @error: a #GError
 *
 * Set the secret key to be used for signing data, commits and summary.
 *
 * The @secret_key argument depends of the particular engine implementation.
 *
 * Returns: @TRUE in case if the key could be set successfully,
 * @FALSE in case of error (@error will contain the reason).
 *
 * Since: 2020.2
 */
gboolean
ostree_sign_set_sk (OstreeSign *self, GVariant *secret_key, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (OSTREE_SIGN_GET_IFACE (self)->set_sk == NULL)
    return glnx_throw (error, "not implemented");

  return OSTREE_SIGN_GET_IFACE (self)->set_sk (self, secret_key, error);
}

/**
 * ostree_sign_set_pk:
 * @self: an #OstreeSign object
 * @public_key: single public key to be added
 * @error: a #GError
 *
 * Set the public key for verification. It is expected what all
 * previously pre-loaded public keys will be dropped.
 *
 * The @public_key argument depends of the particular engine implementation.
 *
 * Returns: @TRUE in case if the key could be set successfully,
 * @FALSE in case of error (@error will contain the reason).
 *
 * Since: 2020.2
 */
gboolean
ostree_sign_set_pk (OstreeSign *self, GVariant *public_key, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (OSTREE_SIGN_GET_IFACE (self)->set_pk == NULL)
    return glnx_throw (error, "not implemented");

  return OSTREE_SIGN_GET_IFACE (self)->set_pk (self, public_key, error);
}

/**
 * ostree_sign_add_pk:
 * @self: an #OstreeSign object
 * @public_key: single public key to be added
 * @error: a #GError
 *
 * Add the public key for verification. Could be called multiple times for
 * adding all needed keys to be used for verification.
 *
 * The @public_key argument depends of the particular engine implementation.
 *
 * Returns: @TRUE in case if the key could be added successfully,
 * @FALSE in case of error (@error will contain the reason).
 *
 * Since: 2020.2
 */
gboolean
ostree_sign_add_pk (OstreeSign *self, GVariant *public_key, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (OSTREE_SIGN_GET_IFACE (self)->add_pk == NULL)
    return glnx_throw (error, "not implemented");

  return OSTREE_SIGN_GET_IFACE (self)->add_pk (self, public_key, error);
}

/**
 * ostree_sign_load_pk:
 * @self: an #OstreeSign object
 * @options: any options
 * @error: a #GError
 *
 * Load public keys for verification from anywhere.
 * It is expected that all keys would be added to already pre-loaded keys.
 *
 * The @options argument depends of the particular engine implementation.
 *
 * For example, @ed25515 engine could use following string-formatted options:
 * - @filename -- single file to use to load keys from
 * - @basedir -- directory containing subdirectories
 *   'trusted.ed25519.d' and 'revoked.ed25519.d' with appropriate
 *   public keys. Used for testing and re-definition of system-wide
 *   directories if defaults are not suitable for any reason.
 *
 * Returns: @TRUE in case if at least one key could be load successfully,
 * @FALSE in case of error (@error will contain the reason).
 *
 * Since: 2020.2
 */
/*
 * No need to have similar function for secret keys load -- it is expected
 * what the signing software will load the secret key in it's own way.
 */
gboolean
ostree_sign_load_pk (OstreeSign *self, GVariant *options, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (OSTREE_SIGN_GET_IFACE (self)->load_pk == NULL)
    return glnx_throw (error, "not implemented");

  return OSTREE_SIGN_GET_IFACE (self)->load_pk (self, options, error);
}

/**
 * ostree_sign_data:
 * @self: an #OstreeSign object
 * @data: the raw data to be signed with pre-loaded secret key
 * @signature: (out): in case of success will contain signature
 * @cancellable: A #GCancellable
 * @error: a #GError
 *
 * Sign the given @data with pre-loaded secret key.
 *
 * Depending of the signing engine used you will need to load
 * the secret key with #ostree_sign_set_sk.
 *
 * Returns: @TRUE if @data has been signed successfully,
 * @FALSE in case of error (@error will contain the reason).
 *
 * Since: 2020.2
 */
gboolean
ostree_sign_data (OstreeSign *self, GBytes *data, GBytes **signature, GCancellable *cancellable,
                  GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (OSTREE_SIGN_GET_IFACE (self)->data == NULL)
    return glnx_throw (error, "not implemented");

  return OSTREE_SIGN_GET_IFACE (self)->data (self, data, signature, cancellable, error);
}

/**
 * ostree_sign_data_verify:
 * @self: an #OstreeSign object
 * @data: the raw data to check
 * @signatures: the signatures to be checked
 * @out_success_message: (out) (nullable) (optional): success message returned by the signing
 * engine
 * @error: a #GError
 *
 * Verify given data against signatures with pre-loaded public keys.
 *
 * Depending of the signing engine used you will need to load
 * the public key(s) with #ostree_sign_set_pk, #ostree_sign_add_pk
 * or #ostree_sign_load_pk.
 *
 * Returns: @TRUE if @data has been signed at least with any single valid key,
 * @FALSE in case of error or no valid keys are available (@error will contain the reason).
 *
 * Since: 2020.2
 */
gboolean
ostree_sign_data_verify (OstreeSign *self, GBytes *data, GVariant *signatures,
                         char **out_success_message, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (OSTREE_SIGN_GET_IFACE (self)->data_verify == NULL)
    return glnx_throw (error, "not implemented");

  return OSTREE_SIGN_GET_IFACE (self)->data_verify (self, data, signatures, out_success_message,
                                                    error);
}

/*
 * Adopted version of _ostree_detached_metadata_append_gpg_sig ()
 */
static GVariant *
_sign_detached_metadata_append (OstreeSign *self, GVariant *existing_metadata,
                                GBytes *signature_bytes, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (signature_bytes == NULL)
    return glnx_null_throw (error, "Invalid NULL signature bytes");

  GVariantDict metadata_dict;
  g_autoptr (GVariant) signature_data = NULL;
  g_autoptr (GVariantBuilder) signature_builder = NULL;

  g_variant_dict_init (&metadata_dict, existing_metadata);

  const gchar *signature_key = ostree_sign_metadata_key (self);
  GVariantType *signature_format = (GVariantType *)ostree_sign_metadata_format (self);

  signature_data = g_variant_dict_lookup_value (&metadata_dict, signature_key,
                                                (GVariantType *)signature_format);

  /* signature_data may be NULL */
  signature_builder = ot_util_variant_builder_from_variant (signature_data, signature_format);

  g_variant_builder_add (signature_builder, "@ay", ot_gvariant_new_ay_bytes (signature_bytes));

  g_variant_dict_insert_value (&metadata_dict, signature_key,
                               g_variant_builder_end (signature_builder));

  return g_variant_ref_sink (g_variant_dict_end (&metadata_dict));
}

/**
 * ostree_sign_commit_verify:
 * @self: an #OstreeSign object
 * @repo: an #OsreeRepo object
 * @commit_checksum: SHA256 of given commit to verify
 * @out_success_message: (out) (nullable) (optional): success message returned by the signing
 * engine
 * @cancellable: A #GCancellable
 * @error: a #GError
 *
 * Verify if commit is signed with known key.
 *
 * Depending of the signing engine used you will need to load
 * the public key(s) for verification with #ostree_sign_set_pk,
 * #ostree_sign_add_pk and/or #ostree_sign_load_pk.
 *
 * Returns: @TRUE if commit has been verified successfully,
 * @FALSE in case of error or no valid keys are available (@error will contain the reason).
 *
 * Since: 2020.2
 */
gboolean
ostree_sign_commit_verify (OstreeSign *self, OstreeRepo *repo, const gchar *commit_checksum,
                           char **out_success_message, GCancellable *cancellable, GError **error)

{
  g_assert (OSTREE_IS_SIGN (self));

  g_autoptr (GVariant) commit_variant = NULL;
  /* Load the commit */
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit_checksum, &commit_variant,
                                 error))
    return glnx_prefix_error (error, "Failed to read commit");

  /* Load the metadata */
  g_autoptr (GVariant) metadata = NULL;
  if (!ostree_repo_read_commit_detached_metadata (repo, commit_checksum, &metadata, cancellable,
                                                  error))
    return glnx_prefix_error (error, "Failed to read detached metadata");

  g_autoptr (GBytes) signed_data = g_variant_get_data_as_bytes (commit_variant);

  g_autoptr (GVariant) signatures = NULL;

  const gchar *signature_key = ostree_sign_metadata_key (self);
  GVariantType *signature_format = (GVariantType *)ostree_sign_metadata_format (self);

  if (metadata)
    signatures = g_variant_lookup_value (metadata, signature_key, signature_format);

  return ostree_sign_data_verify (self, signed_data, signatures, out_success_message, error);
}

/**
 * ostree_sign_get_name:
 * @self: an #OstreeSign object
 *
 * Return the pointer to the name of currently used/selected signing engine.
 *
 * Returns: (transfer none): pointer to the name
 * @NULL in case of error (unlikely).
 *
 * Since: 2020.2
 */
const gchar *
ostree_sign_get_name (OstreeSign *self)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (OSTREE_SIGN_GET_IFACE (self)->get_name == NULL)
    return NULL;

  return OSTREE_SIGN_GET_IFACE (self)->get_name (self);
}

/**
 * ostree_sign_commit:
 * @self: an #OstreeSign object
 * @repo: an #OsreeRepo object
 * @commit_checksum: SHA256 of given commit to sign
 * @cancellable: A #GCancellable
 * @error: a #GError
 *
 * Add a signature to a commit.
 *
 * Depending of the signing engine used you will need to load
 * the secret key with #ostree_sign_set_sk.
 *
 * Returns: @TRUE if commit has been signed successfully,
 * @FALSE in case of error (@error will contain the reason).
 *
 * Since: 2020.2
 */
gboolean
ostree_sign_commit (OstreeSign *self, OstreeRepo *repo, const gchar *commit_checksum,
                    GCancellable *cancellable, GError **error)
{

  g_autoptr (GBytes) commit_data = NULL;
  g_autoptr (GBytes) signature = NULL;
  g_autoptr (GVariant) commit_variant = NULL;
  g_autoptr (GVariant) old_metadata = NULL;
  g_autoptr (GVariant) new_metadata = NULL;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit_checksum, &commit_variant,
                                 error))
    return glnx_prefix_error (error, "Failed to read commit");

  if (!ostree_repo_read_commit_detached_metadata (repo, commit_checksum, &old_metadata, cancellable,
                                                  error))
    return glnx_prefix_error (error, "Failed to read detached metadata");

  commit_data = g_variant_get_data_as_bytes (commit_variant);

  if (!ostree_sign_data (self, commit_data, &signature, cancellable, error))
    return glnx_prefix_error (error, "Not able to sign the cobject");

  new_metadata = _sign_detached_metadata_append (self, old_metadata, signature, error);
  if (new_metadata == NULL)
    return FALSE;

  if (!ostree_repo_write_commit_detached_metadata (repo, commit_checksum, new_metadata, cancellable,
                                                   error))
    return FALSE;

  return TRUE;
}

/**
 * ostree_sign_get_all:
 *
 * Return an array with newly allocated instances of all available
 * signing engines; they will not be initialized.
 *
 * Returns: (transfer full) (element-type OstreeSign): an array of signing engines
 *
 * Since: 2020.2
 */
GPtrArray *
ostree_sign_get_all (void)
{
  g_autoptr (GPtrArray) engines = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < G_N_ELEMENTS (sign_types); i++)
    {
      OstreeSign *engine = ostree_sign_get_by_name (sign_types[i].name, NULL);
      g_assert (engine);
      g_ptr_array_add (engines, engine);
    }

  return g_steal_pointer (&engines);
}

/**
 * ostree_sign_get_by_name:
 * @name: the name of desired signature engine
 * @error: return location for a #GError
 *
 * Create a new instance of a signing engine.
 *
 * Returns: (transfer full): New signing engine, or %NULL if the engine is not known
 *
 * Since: 2020.2
 */
OstreeSign *
ostree_sign_get_by_name (const gchar *name, GError **error)
{

  OstreeSign *sign = NULL;

  /* Get types if not initialized yet */
#if defined(HAVE_ED25519)
  if (sign_types[SIGN_ED25519].type == 0)
    sign_types[SIGN_ED25519].type = OSTREE_TYPE_SIGN_ED25519;
#endif
  if (sign_types[SIGN_DUMMY].type == 0)
    sign_types[SIGN_DUMMY].type = OSTREE_TYPE_SIGN_DUMMY;

  for (gint i = 0; i < G_N_ELEMENTS (sign_types); i++)
    {
      if (g_strcmp0 (name, sign_types[i].name) == 0)
        {
          g_debug ("Using '%s' signing engine", sign_types[i].name);
          sign = g_object_new (sign_types[i].type, NULL);
          break;
        }
    }

  if (sign == NULL)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Requested signature type is not implemented");

  return sign;
}

gboolean
_ostree_sign_summary_at (OstreeSign *self, OstreeRepo *repo, int dir_fd, GVariant *keys,
                         GCancellable *cancellable, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));
  g_assert (OSTREE_IS_REPO (repo));

  g_autoptr (GVariant) normalized = NULL;
  g_autoptr (GBytes) summary_data = NULL;
  g_autoptr (GVariant) metadata = NULL;

  glnx_autofd int fd = -1;
  if (!glnx_openat_rdonly (dir_fd, "summary", TRUE, &fd, error))
    return FALSE;
  summary_data = ot_fd_readall_or_mmap (fd, 0, error);
  if (!summary_data)
    return FALSE;

  /* Note that fd is reused below */
  glnx_close_fd (&fd);

  if (!ot_openat_ignore_enoent (dir_fd, "summary.sig", &fd, error))
    return FALSE;

  if (fd >= 0)
    {
      if (!ot_variant_read_fd (fd, 0, OSTREE_SUMMARY_SIG_GVARIANT_FORMAT, FALSE, &metadata, error))
        return FALSE;
    }

  if (g_variant_n_children (keys) == 0)
    return glnx_throw (error, "No keys passed for signing summary");

  GVariantIter *iter;
  GVariant *key;

  g_variant_get (keys, "av", &iter);
  while (g_variant_iter_loop (iter, "v", &key))
    {
      g_autoptr (GBytes) signature = NULL;

      if (!ostree_sign_set_sk (self, key, error))
        return FALSE;

      if (!ostree_sign_data (self, summary_data, &signature, cancellable, error))
        return FALSE;

      g_autoptr (GVariant) old_metadata = g_steal_pointer (&metadata);
      metadata = _sign_detached_metadata_append (self, old_metadata, signature, error);
      if (metadata == NULL)
        return FALSE;
    }
  g_variant_iter_free (iter);

  normalized = g_variant_get_normal_form (metadata);
  if (!_ostree_repo_file_replace_contents (repo, dir_fd, "summary.sig",
                                           g_variant_get_data (normalized),
                                           g_variant_get_size (normalized), cancellable, error))
    return FALSE;

  return TRUE;
}

/**
 * ostree_sign_summary:
 * @self: Self
 * @repo: ostree repository
 * @keys: keys -- GVariant containing keys as GVarints specific to signature type.
 * @cancellable: A #GCancellable
 * @error: a #GError
 *
 * Add a signature to a summary file.
 * Based on ostree_repo_add_gpg_signature_summary implementation.
 *
 * Returns: @TRUE if summary file has been signed with all provided keys
 *
 * Since: 2020.2
 */
gboolean
ostree_sign_summary (OstreeSign *self, OstreeRepo *repo, GVariant *keys, GCancellable *cancellable,
                     GError **error)
{
  return _ostree_sign_summary_at (self, repo, repo->repo_dir_fd, keys, cancellable, error);
}
