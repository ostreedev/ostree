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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
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

#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include "libglnx.h"
#include "otutil.h"

#include "ostree-autocleanups.h"
#include "ostree-core.h"
#include "ostree-sign.h"
#include "ostree-sign-dummy.h"
#ifdef HAVE_LIBSODIUM
#include "ostree-sign-ed25519.h"
#endif

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "OSTreeSign"

typedef struct
{
  gchar *name;
  GType type;
} _sign_type;

_sign_type sign_types[] =
{
#if defined(HAVE_LIBSODIUM)
    {"ed25519", 0},
#endif
    {"dummy", 0}
};

enum
{
#if defined(HAVE_LIBSODIUM)
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
  g_debug ("%s enter", __FUNCTION__);

  g_return_val_if_fail (OSTREE_SIGN_GET_IFACE (self)->metadata_key != NULL, NULL);
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
  g_debug ("%s enter", __FUNCTION__);

  g_return_val_if_fail (OSTREE_SIGN_GET_IFACE (self)->metadata_format != NULL, NULL);
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
ostree_sign_clear_keys (OstreeSign *self,
                        GError **error)
{
  g_debug ("%s enter", __FUNCTION__);

  if (OSTREE_SIGN_GET_IFACE (self)->clear_keys == NULL)
    return TRUE;

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
ostree_sign_set_sk (OstreeSign *self,
                    GVariant *secret_key,
                    GError **error)
{
  g_debug ("%s enter", __FUNCTION__);

  if (OSTREE_SIGN_GET_IFACE (self)->set_sk == NULL)
    return TRUE;

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
ostree_sign_set_pk (OstreeSign *self,
                    GVariant *public_key,
                    GError **error)
{
  g_debug ("%s enter", __FUNCTION__);

  if (OSTREE_SIGN_GET_IFACE (self)->set_pk == NULL)
    return TRUE;

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
ostree_sign_add_pk (OstreeSign *self,
                    GVariant *public_key,
                    GError **error)
{
  g_debug ("%s enter", __FUNCTION__);

  if (OSTREE_SIGN_GET_IFACE (self)->add_pk == NULL)
    return TRUE;

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
ostree_sign_load_pk (OstreeSign *self,
                     GVariant *options,
                     GError **error)
{
  g_debug ("%s enter", __FUNCTION__);

  if (OSTREE_SIGN_GET_IFACE (self)->load_pk == NULL)
    return TRUE;

  return OSTREE_SIGN_GET_IFACE (self)->load_pk (self, options, error);
}

/**
 * ostree_sign_data:
 * @self: an #OstreeSign object
 * @data: the raw data to be signed with pre-loaded secret key
 * @signature: in case of success will contain signature
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
ostree_sign_data (OstreeSign *self,
                  GBytes *data,
                  GBytes **signature,
                  GCancellable *cancellable,
                  GError **error)
{

  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);
  g_return_val_if_fail (OSTREE_SIGN_GET_IFACE (self)->data != NULL, FALSE);

  return OSTREE_SIGN_GET_IFACE (self)->data (self, data, signature, cancellable, error);
}

/**
 * ostree_sign_data_verify:
 * @self: an #OstreeSign object
 * @data: the raw data to check
 * @signatures: the signatures to be checked
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
ostree_sign_data_verify (OstreeSign *self,
                         GBytes     *data,
                         GVariant   *signatures,
                         GError     **error)
{
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);
  g_return_val_if_fail (OSTREE_SIGN_GET_IFACE (self)->data_verify != NULL, FALSE);

  return OSTREE_SIGN_GET_IFACE (self)->data_verify(self, data, signatures, error);
}

/*
 * Adopted version of _ostree_detached_metadata_append_gpg_sig ()
 */
static GVariant *
_sign_detached_metadata_append (OstreeSign *self,
                                GVariant   *existing_metadata,
                                GBytes     *signature_bytes)
{
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (signature_bytes != NULL, FALSE);

  GVariantDict metadata_dict;
  g_autoptr(GVariant) signature_data = NULL;
  g_autoptr(GVariantBuilder) signature_builder = NULL;

  g_variant_dict_init (&metadata_dict, existing_metadata);

  const gchar *signature_key = ostree_sign_metadata_key(self);
  GVariantType *signature_format = (GVariantType *) ostree_sign_metadata_format(self);

  signature_data = g_variant_dict_lookup_value (&metadata_dict,
                                                signature_key,
                                                (GVariantType*)signature_format);

  /* signature_data may be NULL */
  signature_builder = ot_util_variant_builder_from_variant (signature_data, signature_format);

  g_variant_builder_add (signature_builder, "@ay", ot_gvariant_new_ay_bytes (signature_bytes));

  g_variant_dict_insert_value (&metadata_dict,
                               signature_key,
                               g_variant_builder_end (signature_builder));

  return  g_variant_dict_end (&metadata_dict);
}

/**
 * ostree_sign_commit_verify:
 * @self: an #OstreeSign object
 * @repo: an #OsreeRepo object
 * @commit_checksum: SHA256 of given commit to verify
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
ostree_sign_commit_verify (OstreeSign     *self,
                           OstreeRepo     *repo,
                           const gchar    *commit_checksum,
                           GCancellable   *cancellable,
                           GError         **error)

{
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);

  g_autoptr(GVariant) commit_variant = NULL;
  /* Load the commit */
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 commit_checksum, &commit_variant,
                                 error))
    return glnx_prefix_error (error, "Failed to read commit");

  /* Load the metadata */
  g_autoptr(GVariant) metadata = NULL;
  if (!ostree_repo_read_commit_detached_metadata (repo,
                                                  commit_checksum,
                                                  &metadata,
                                                  cancellable,
                                                  error))
    return glnx_prefix_error (error, "Failed to read detached metadata");

  g_autoptr(GBytes) signed_data = g_variant_get_data_as_bytes (commit_variant);

  g_autoptr(GVariant) signatures = NULL;

  const gchar *signature_key = ostree_sign_metadata_key(self);
  GVariantType *signature_format = (GVariantType *) ostree_sign_metadata_format(self);

  if (metadata)
    signatures = g_variant_lookup_value (metadata,
                                         signature_key,
                                         signature_format);


  return ostree_sign_data_verify (self,
                                  signed_data,
                                  signatures,
                                  error);
}

/**
 * ostree_sign_get_name:
 * @self: an #OstreeSign object
 *
 * Return the pointer to the name of currently used/selected signing engine.
 *
 * The list of available engines could be acquired with #ostree_sign_list_names.
 *
 * Returns: (transfer none): pointer to the name
 * @NULL in case of error (unlikely).
 *
 * Since: 2020.2
 */
const gchar * 
ostree_sign_get_name (OstreeSign *self)
{
    g_debug ("%s enter", __FUNCTION__);
    g_return_val_if_fail (OSTREE_IS_SIGN (self), NULL);
    g_return_val_if_fail (OSTREE_SIGN_GET_IFACE (self)->get_name != NULL, NULL);

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
ostree_sign_commit (OstreeSign     *self,
                    OstreeRepo     *repo,
                    const gchar    *commit_checksum,
                    GCancellable   *cancellable,
                    GError         **error)
{
  g_debug ("%s enter", __FUNCTION__);

  g_autoptr(GBytes) commit_data = NULL;
  g_autoptr(GBytes) signature = NULL;
  g_autoptr(GVariant) commit_variant = NULL;
  g_autoptr(GVariant) old_metadata = NULL;
  g_autoptr(GVariant) new_metadata = NULL;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 commit_checksum, &commit_variant, error))
    return glnx_prefix_error (error, "Failed to read commit");

  if (!ostree_repo_read_commit_detached_metadata (repo,
                                                  commit_checksum,
                                                  &old_metadata,
                                                  cancellable,
                                                  error))
    return glnx_prefix_error (error, "Failed to read detached metadata");

  commit_data = g_variant_get_data_as_bytes (commit_variant);

  if (!ostree_sign_data (self, commit_data, &signature,
                         cancellable, error))
    return glnx_prefix_error (error, "Not able to sign the cobject");

  new_metadata =
    _sign_detached_metadata_append (self, old_metadata, signature);

  if (!ostree_repo_write_commit_detached_metadata (repo,
                                                   commit_checksum,
                                                   new_metadata,
                                                   cancellable,
                                                   error))
    return FALSE;

  return TRUE;
}

/**
 * ostree_sign_list_names:
 *
 * Return an array with all available sign engines names.
 *
 * Returns: (transfer full): an array of strings, free when you used it
 *
 * Since: 2020.2
 */
GStrv
ostree_sign_list_names(void)
{
  g_debug ("%s enter", __FUNCTION__);

  GStrv names = g_new0 (char *, G_N_ELEMENTS(sign_types) + 1);
  gint i = 0;

  for (i=0; i < G_N_ELEMENTS(sign_types); i++)
  {
    names[i] = g_strdup(sign_types[i].name);
    g_debug ("Found '%s' signing engine", names[i]);
  }

  return names;
}

/**
 * ostree_sign_get_by_name:
 * @name: the name of desired signature engine
 * @error: return location for a #GError
 *
 * Tries to find and return proper signing engine by it's name.
 *
 * The list of available engines could be acquired with #ostree_sign_list_names.
 *
 * Returns: (transfer full): a constant, free when you used it
 *
 * Since: 2020.2
 */
OstreeSign *
ostree_sign_get_by_name (const gchar *name, GError **error)
{
  g_debug ("%s enter", __FUNCTION__);

  OstreeSign *sign = NULL;

  /* Get types if not initialized yet */
#if defined(HAVE_LIBSODIUM)
  if (sign_types[SIGN_ED25519].type == 0)
    sign_types[SIGN_ED25519].type = OSTREE_TYPE_SIGN_ED25519;
#endif
  if (sign_types[SIGN_DUMMY].type == 0)
    sign_types[SIGN_DUMMY].type = OSTREE_TYPE_SIGN_DUMMY;

  for (gint i=0; i < G_N_ELEMENTS(sign_types); i++)
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