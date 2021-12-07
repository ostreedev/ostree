/*
 * Copyright (C) 2015 Red Hat, Inc.
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
 */

#include "config.h"

#include <string.h>

#include "libglnx.h"

#include "ostree-gpg-verify-result-private.h"

/**
 * SECTION: ostree-gpg-verify-result
 * @title: GPG signature verification results
 * @short_description: Inspect detached GPG signatures
 *
 * #OstreeGpgVerifyResult contains verification details for GPG signatures
 * read from a detached #OstreeRepo metadata object.
 *
 * Use ostree_gpg_verify_result_count_all() and
 * ostree_gpg_verify_result_count_valid() to quickly check overall signature
 * validity.
 *
 * Use ostree_gpg_verify_result_lookup() to find a signature by the key ID
 * or fingerprint of the signing key.
 *
 * For more in-depth inspection, such as presenting signature details to the
 * user, pass an array of attribute values to ostree_gpg_verify_result_get()
 * or get all signature details with ostree_gpg_verify_result_get_all().
 */

typedef struct {
  GObjectClass parent_class;
} OstreeGpgVerifyResultClass;

/* This must stay synchronized with the enum declaration. */
static OstreeGpgSignatureAttr all_signature_attrs[] = {
  OSTREE_GPG_SIGNATURE_ATTR_VALID,
  OSTREE_GPG_SIGNATURE_ATTR_SIG_EXPIRED,
  OSTREE_GPG_SIGNATURE_ATTR_KEY_EXPIRED,
  OSTREE_GPG_SIGNATURE_ATTR_KEY_REVOKED,
  OSTREE_GPG_SIGNATURE_ATTR_KEY_MISSING,
  OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT,
  OSTREE_GPG_SIGNATURE_ATTR_TIMESTAMP,
  OSTREE_GPG_SIGNATURE_ATTR_EXP_TIMESTAMP,
  OSTREE_GPG_SIGNATURE_ATTR_PUBKEY_ALGO_NAME,
  OSTREE_GPG_SIGNATURE_ATTR_HASH_ALGO_NAME,
  OSTREE_GPG_SIGNATURE_ATTR_USER_NAME,
  OSTREE_GPG_SIGNATURE_ATTR_USER_EMAIL,
  OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT_PRIMARY,
  OSTREE_GPG_SIGNATURE_ATTR_KEY_EXP_TIMESTAMP,
  OSTREE_GPG_SIGNATURE_ATTR_KEY_EXP_TIMESTAMP_PRIMARY,
};

static void ostree_gpg_verify_result_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (OstreeGpgVerifyResult,
                         ostree_gpg_verify_result,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                ostree_gpg_verify_result_initable_iface_init))

static gboolean
signature_is_valid (gpgme_signature_t signature)
{
  /* Mimic the way librepo tests for a valid signature, checking both
   * summary and status fields.
   *
   * - VALID summary flag means the signature is fully valid.
   * - GREEN summary flag means the signature is valid with caveats.
   * - No summary but also no error means the signature is valid but
   *   the signing key is not certified with a trusted signature.
   */
  return (signature->summary & GPGME_SIGSUM_VALID) ||
         (signature->summary & GPGME_SIGSUM_GREEN) ||
         (signature->summary == 0 && signature->status == GPG_ERR_NO_ERROR);
}

static gboolean
signing_key_is_revoked (gpgme_signature_t signature)
{
  /* In my testing, GPGME does not set the GPGME_SIGSUM_KEY_REVOKED summary
   * bit on a revoked signing key but rather GPGME_SIGSUM_SYS_ERROR and the
   * status field shows GPG_ERR_CERT_REVOKED.  Turns out GPGME is expecting
   * GPG_ERR_CERT_REVOKED in the validity_reason field which would then set
   * the summary bit.
   *
   * Reported to GPGME: https://bugs.g10code.com/gnupg/issue1929
   */

  return (signature->summary & GPGME_SIGSUM_KEY_REVOKED) ||
         ((signature->summary & GPGME_SIGSUM_SYS_ERROR) &&
          gpgme_err_code (signature->status) == GPG_ERR_CERT_REVOKED);
}

static void
ostree_gpg_verify_result_finalize (GObject *object)
{
  OstreeGpgVerifyResult *result = OSTREE_GPG_VERIFY_RESULT (object);

  if (result->context != NULL)
    gpgme_release (result->context);

  if (result->details != NULL)
    gpgme_result_unref (result->details);

  G_OBJECT_CLASS (ostree_gpg_verify_result_parent_class)->finalize (object);
}

static gboolean
ostree_gpg_verify_result_initable_init (GInitable     *initable,
                                        GCancellable  *cancellable,
                                        GError       **error)
{
  OstreeGpgVerifyResult *result = OSTREE_GPG_VERIFY_RESULT (initable);
  gpgme_error_t gpg_error;
  gboolean ret = FALSE;

  gpg_error = gpgme_new (&result->context);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      ot_gpgme_throw (gpg_error, error, "Unable to create context");
      goto out;
    }

  ret = TRUE;

out:
  return ret;
}

static void
ostree_gpg_verify_result_class_init (OstreeGpgVerifyResultClass *class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (class);
  object_class->finalize = ostree_gpg_verify_result_finalize;
}

static void
ostree_gpg_verify_result_init (OstreeGpgVerifyResult *result)
{
}

static void
ostree_gpg_verify_result_initable_iface_init (GInitableIface *iface)
{
  iface->init = ostree_gpg_verify_result_initable_init;
}

/**
 * ostree_gpg_verify_result_count_all:
 * @result: an #OstreeGpgVerifyResult
 *
 * Counts all the signatures in @result.
 *
 * Returns: signature count
 */
guint
ostree_gpg_verify_result_count_all (OstreeGpgVerifyResult *result)
{
  gpgme_signature_t signature;
  guint count = 0;

  g_return_val_if_fail (OSTREE_IS_GPG_VERIFY_RESULT (result), 0);

  for (signature = result->details->signatures;
       signature != NULL;
       signature = signature->next)
    {
      count++;
    }

  return count;
}

/**
 * ostree_gpg_verify_result_count_valid:
 * @result: an #OstreeGpgVerifyResult
 *
 * Counts only the valid signatures in @result.
 *
 * Returns: valid signature count
 */
guint
ostree_gpg_verify_result_count_valid (OstreeGpgVerifyResult *result)
{
  gpgme_signature_t signature;
  guint count = 0;

  g_return_val_if_fail (OSTREE_IS_GPG_VERIFY_RESULT (result), 0);

  for (signature = result->details->signatures;
       signature != NULL;
       signature = signature->next)
    {
      if (signature_is_valid (signature))
        count++;
    }

  return count;
}

/**
 * ostree_gpg_verify_result_lookup:
 * @result: an #OstreeGpgVerifyResult
 * @key_id: a GPG key ID or fingerprint
 * @out_signature_index: (out): return location for the index of the signature
 *                              signed by @key_id, or %NULL
 *
 * Searches @result for a signature signed by @key_id.  If a match is found,
 * the function returns %TRUE and sets @out_signature_index so that further
 * signature details can be obtained through ostree_gpg_verify_result_get().
 * If no match is found, the function returns %FALSE and leaves
 * @out_signature_index unchanged.
 *
 * Returns: %TRUE on success, %FALSE on failure
 **/
gboolean
ostree_gpg_verify_result_lookup (OstreeGpgVerifyResult *result,
                                 const gchar *key_id,
                                 guint *out_signature_index)
{
  g_auto(gpgme_key_t) lookup_key = NULL;
  gpgme_signature_t signature;
  guint signature_index;

  g_return_val_if_fail (OSTREE_IS_GPG_VERIFY_RESULT (result), FALSE);
  g_return_val_if_fail (key_id != NULL, FALSE);

  /* fetch requested key_id from keyring to canonicalise ID */
  (void) gpgme_get_key (result->context, key_id, &lookup_key, 0);

  if (lookup_key == NULL)
    {
      g_debug ("Could not find key ID %s to lookup signature.", key_id);
      return FALSE;
    }

  for (signature = result->details->signatures, signature_index = 0;
       signature != NULL;
       signature = signature->next, signature_index++)
    {
      g_auto(gpgme_key_t) signature_key = NULL;

      (void) gpgme_get_key (result->context, signature->fpr, &signature_key, 0);

      if (signature_key == NULL)
        {
          g_debug ("Could not find key when looking up signature from %s.", signature->fpr);
          continue;
        }

      /* the first subkey in the list is the primary key */
      if (!g_strcmp0 (lookup_key->subkeys->fpr,
                      signature_key->subkeys->fpr))
        {
          if (out_signature_index != NULL)
            *out_signature_index = signature_index;
          /* Note early return */
          return TRUE;
        }

    }

  return FALSE;
}

/**
 * ostree_gpg_verify_result_get:
 * @result: an #OstreeGpgVerifyResult
 * @signature_index: which signature to get attributes from
 * @attrs: (array length=n_attrs): Array of requested attributes
 * @n_attrs: Length of the @attrs array
 *
 * Builds a #GVariant tuple of requested attributes for the GPG signature at
 * @signature_index in @result.  See the #OstreeGpgSignatureAttr description
 * for the #GVariantType of each available attribute.
 *
 * It is a programmer error to request an invalid #OstreeGpgSignatureAttr or
 * an invalid @signature_index.  Use ostree_gpg_verify_result_count_all() to
 * find the number of signatures in @result.
 *
 * Returns: a new, floating, #GVariant tuple
 **/
GVariant *
ostree_gpg_verify_result_get (OstreeGpgVerifyResult *result,
                              guint signature_index,
                              OstreeGpgSignatureAttr *attrs,
                              guint n_attrs)
{
  GVariantBuilder builder;
  g_auto(gpgme_key_t) key = NULL;
  gpgme_signature_t signature;
  guint ii;

  g_return_val_if_fail (OSTREE_IS_GPG_VERIFY_RESULT (result), NULL);
  g_return_val_if_fail (attrs != NULL, NULL);
  g_return_val_if_fail (n_attrs > 0, NULL);

  signature = result->details->signatures;
  while (signature != NULL && signature_index > 0)
    {
      signature = signature->next;
      signature_index--;
    }

  g_return_val_if_fail (signature != NULL, NULL);

  /* Lookup the signing key if we need it.  Note, failure to find
   * the key is not a fatal error.  There's an attribute for that
   * (OSTREE_GPG_SIGNATURE_ATTR_KEY_MISSING). */
  for (ii = 0; ii < n_attrs; ii++)
    {
      if (attrs[ii] == OSTREE_GPG_SIGNATURE_ATTR_USER_NAME ||
          attrs[ii] == OSTREE_GPG_SIGNATURE_ATTR_USER_EMAIL ||
          attrs[ii] == OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT_PRIMARY ||
          attrs[ii] == OSTREE_GPG_SIGNATURE_ATTR_KEY_EXP_TIMESTAMP ||
          attrs[ii] == OSTREE_GPG_SIGNATURE_ATTR_KEY_EXP_TIMESTAMP_PRIMARY)
        {
          (void) gpgme_get_key (result->context, signature->fpr, &key, 0);
          break;
        }
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);

  for (ii = 0; ii < n_attrs; ii++)
    {
      GVariant *child;
      gboolean v_boolean;
      const char *v_string = NULL;
      gint64 v_int64;

      switch (attrs[ii])
        {
          case OSTREE_GPG_SIGNATURE_ATTR_VALID:
            v_boolean = signature_is_valid (signature);
            child = g_variant_new_boolean (v_boolean);
            break;

          case OSTREE_GPG_SIGNATURE_ATTR_SIG_EXPIRED:
            v_boolean = ((signature->summary & GPGME_SIGSUM_SIG_EXPIRED) != 0);
            child = g_variant_new_boolean (v_boolean);
            break;

          case OSTREE_GPG_SIGNATURE_ATTR_KEY_EXPIRED:
            v_boolean = ((signature->summary & GPGME_SIGSUM_KEY_EXPIRED) != 0);
            child = g_variant_new_boolean (v_boolean);
            break;

          case OSTREE_GPG_SIGNATURE_ATTR_KEY_REVOKED:
            v_boolean = signing_key_is_revoked (signature);
            child = g_variant_new_boolean (v_boolean);
            break;

          case OSTREE_GPG_SIGNATURE_ATTR_KEY_MISSING:
            v_boolean = ((signature->summary & GPGME_SIGSUM_KEY_MISSING) != 0);
            child = g_variant_new_boolean (v_boolean);
            break;

          case OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT:
            child = g_variant_new_string (signature->fpr);
            break;

          case OSTREE_GPG_SIGNATURE_ATTR_TIMESTAMP:
            child = g_variant_new_int64 ((gint64) signature->timestamp);
            break;

          case OSTREE_GPG_SIGNATURE_ATTR_EXP_TIMESTAMP:
            child = g_variant_new_int64 ((gint64) signature->exp_timestamp);
            break;

          case OSTREE_GPG_SIGNATURE_ATTR_PUBKEY_ALGO_NAME:
            v_string = gpgme_pubkey_algo_name (signature->pubkey_algo);
            if (v_string == NULL)
              v_string = "[unknown name]";
            child = g_variant_new_string (v_string);
            break;

          case OSTREE_GPG_SIGNATURE_ATTR_HASH_ALGO_NAME:
            v_string = gpgme_hash_algo_name (signature->hash_algo);
            if (v_string == NULL)
              v_string = "[unknown name]";
            child = g_variant_new_string (v_string);
            break;

          case OSTREE_GPG_SIGNATURE_ATTR_USER_NAME:
            if (key != NULL && key->uids != NULL)
              v_string = key->uids->name;
            if (v_string == NULL)
              v_string = "[unknown name]";
            child = g_variant_new_string (v_string);
            break;

          case OSTREE_GPG_SIGNATURE_ATTR_USER_EMAIL:
            if (key != NULL && key->uids != NULL)
              v_string = key->uids->email;
            if (v_string == NULL)
              v_string = "[unknown email]";
            child = g_variant_new_string (v_string);
            break;

          case OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT_PRIMARY:
            if (key != NULL && key->subkeys != NULL)
              v_string = key->subkeys->fpr;
            if (v_string == NULL)
              v_string = "";
            child = g_variant_new_string (v_string);
            break;

          case OSTREE_GPG_SIGNATURE_ATTR_KEY_EXP_TIMESTAMP:
            v_int64 = 0;
            if (key != NULL)
              {
                gpgme_subkey_t subkey = key->subkeys;

                while (subkey != NULL && (g_strcmp0 (subkey->fpr, signature->fpr) != 0))
                  subkey = subkey->next;

                if (subkey != NULL)
                  v_int64 = subkey->expires;
              }
            child = g_variant_new_int64 (v_int64);
            break;

          case OSTREE_GPG_SIGNATURE_ATTR_KEY_EXP_TIMESTAMP_PRIMARY:
            if (key != NULL && key->subkeys != NULL)
              v_int64 = key->subkeys->expires;
            else
              v_int64 = 0;
            child = g_variant_new_int64 (v_int64);
            break;

          default:
            g_critical ("Invalid signature attribute (%d)", attrs[ii]);
            g_variant_builder_clear (&builder);
            return NULL;
        }

      g_variant_builder_add_value (&builder, child);
    }

  return g_variant_builder_end (&builder);
}

/**
 * ostree_gpg_verify_result_get_all:
 * @result: an #OstreeGpgVerifyResult
 * @signature_index: which signature to get attributes from
 *
 * Builds a #GVariant tuple of all available attributes for the GPG signature
 * at @signature_index in @result.
 *
 * The child values in the returned #GVariant tuple are ordered to match the
 * #OstreeGpgSignatureAttr enumeration, which means the enum values can be
 * used as index values in functions like g_variant_get_child().  See the
 * #OstreeGpgSignatureAttr description for the #GVariantType of each
 * available attribute.
 *
 * <note>
 *   <para>
 *     The #OstreeGpgSignatureAttr enumeration may be extended in the future
 *     with new attributes, which would affect the #GVariant tuple returned by
 *     this function.  While the position and type of current child values in
 *     the #GVariant tuple will not change, to avoid backward-compatibility
 *     issues <emphasis>please do not depend on the tuple's overall size or
 *     type signature</emphasis>.
 *   </para>
 * </note>
 *
 * It is a programmer error to request an invalid @signature_index.  Use
 * ostree_gpg_verify_result_count_all() to find the number of signatures in
 * @result.
 *
 * Returns: a new, floating, #GVariant tuple
 **/
GVariant *
ostree_gpg_verify_result_get_all (OstreeGpgVerifyResult *result,
                                  guint signature_index)
{
  g_return_val_if_fail (OSTREE_IS_GPG_VERIFY_RESULT (result), NULL);

  return ostree_gpg_verify_result_get (result, signature_index,
                                       all_signature_attrs,
                                       G_N_ELEMENTS (all_signature_attrs));
}

/**
 * ostree_gpg_verify_result_describe:
 * @result: an #OstreeGpgVerifyResult
 * @signature_index: which signature to describe
 * @output_buffer: a #GString to hold the description
 * @line_prefix: (allow-none): optional line prefix string
 * @flags: flags to adjust the description format
 *
 * Appends a brief, human-readable description of the GPG signature at
 * @signature_index in @result to the @output_buffer.  The description
 * spans multiple lines.  A @line_prefix string, if given, will precede
 * each line of the description.
 *
 * The @flags argument is reserved for future variations to the description
 * format.  Currently must be 0.
 *
 * It is a programmer error to request an invalid @signature_index.  Use
 * ostree_gpg_verify_result_count_all() to find the number of signatures in
 * @result.
 */
void
ostree_gpg_verify_result_describe (OstreeGpgVerifyResult *result,
                                   guint signature_index,
                                   GString *output_buffer,
                                   const gchar *line_prefix,
                                   OstreeGpgSignatureFormatFlags flags)
{
  g_autoptr(GVariant) variant = NULL;

  g_return_if_fail (OSTREE_IS_GPG_VERIFY_RESULT (result));

  variant = ostree_gpg_verify_result_get_all (result, signature_index);

  ostree_gpg_verify_result_describe_variant (variant, output_buffer, line_prefix, flags);
}

static void
append_expire_info (GString *output_buffer,
                    const gchar *line_prefix,
                    const gchar *exp_type,
                    gint64 exp_timestamp,
                    gboolean expired)
{
  if (line_prefix != NULL)
    g_string_append (output_buffer, line_prefix);

  g_autoptr(GDateTime) date_time_utc = g_date_time_new_from_unix_utc (exp_timestamp);
  if (date_time_utc == NULL)
    {
      g_string_append_printf (output_buffer,
                              "%s expiry timestamp (%" G_GINT64_FORMAT ") is invalid\n",
                              exp_type,
                              exp_timestamp);
      return;
    }

  g_autoptr(GDateTime) date_time_local = g_date_time_to_local (date_time_utc);
  g_autofree char *formatted_date_time = g_date_time_format (date_time_local, "%c");

  if (expired)
    {
      g_string_append_printf (output_buffer,
                              "%s expired %s\n",
                              exp_type,
                              formatted_date_time);
    }
  else
    {
      g_string_append_printf (output_buffer,
                              "%s expires %s\n",
                              exp_type,
                              formatted_date_time);
    }
}

/**
 * ostree_gpg_verify_result_describe_variant:
 * @variant: a #GVariant from ostree_gpg_verify_result_get_all()
 * @output_buffer: a #GString to hold the description
 * @line_prefix: (allow-none): optional line prefix string
 * @flags: flags to adjust the description format
 *
 * Similar to ostree_gpg_verify_result_describe() but takes a #GVariant of
 * all attributes for a GPG signature instead of an #OstreeGpgVerifyResult
 * and signature index.
 *
 * The @variant <emphasis>MUST</emphasis> have been created by
 * ostree_gpg_verify_result_get_all().
 */
void
ostree_gpg_verify_result_describe_variant (GVariant *variant,
                                           GString *output_buffer,
                                           const gchar *line_prefix,
                                           OstreeGpgSignatureFormatFlags flags)
{
  g_autoptr(GDateTime) date_time_utc = NULL;
  g_autoptr(GDateTime) date_time_local = NULL;
  g_autofree char *formatted_date_time = NULL;
  gint64 timestamp;
  gint64 exp_timestamp;
  gint64 key_exp_timestamp;
  gint64 key_exp_timestamp_primary;
  const char *type_string;
  const char *fingerprint;
  const char *fingerprint_primary;
  const char *pubkey_algo;
  const char *user_name;
  const char *user_email;
  const char *key_id;
  gboolean valid;
  gboolean sig_expired;
  gboolean key_expired;
  gboolean key_revoked;
  gboolean key_missing;
  gsize len;

  g_return_if_fail (variant != NULL);
  g_return_if_fail (output_buffer != NULL);

  /* Verify the variant's type string.  This code is
   * not prepared to handle just any random GVariant. */
  type_string = g_variant_get_type_string (variant);
  g_return_if_fail (strcmp (type_string, "(bbbbbsxxsssssxx)") == 0);

  /* The default format roughly mimics the verify output generated by
   * check_sig_and_print() in gnupg/g10/mainproc.c, though obviously
   * greatly simplified. */

  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_VALID,
                       "b", &valid);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_SIG_EXPIRED,
                       "b", &sig_expired);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_KEY_EXPIRED,
                       "b", &key_expired);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_KEY_REVOKED,
                       "b", &key_revoked);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_KEY_MISSING,
                       "b", &key_missing);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT,
                       "&s", &fingerprint);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT_PRIMARY,
                       "&s", &fingerprint_primary);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_TIMESTAMP,
                       "x", &timestamp);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_EXP_TIMESTAMP,
                       "x", &exp_timestamp);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_PUBKEY_ALGO_NAME,
                       "&s", &pubkey_algo);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_USER_NAME,
                       "&s", &user_name);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_USER_EMAIL,
                       "&s", &user_email);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_KEY_EXP_TIMESTAMP,
                       "x", &key_exp_timestamp);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_KEY_EXP_TIMESTAMP_PRIMARY,
                       "x", &key_exp_timestamp_primary);

  len = strlen (fingerprint);
  key_id = (len > 16) ? fingerprint + len - 16 : fingerprint;

  date_time_utc = g_date_time_new_from_unix_utc (timestamp);
  if (date_time_utc == NULL)
    {
      g_string_append_printf (output_buffer,
                              "Can't check signature: timestamp %" G_GINT64_FORMAT " is invalid\n",
                              timestamp);
      return;
    }

  date_time_local = g_date_time_to_local (date_time_utc);
  formatted_date_time = g_date_time_format (date_time_local, "%c");

  if (line_prefix != NULL)
    g_string_append (output_buffer, line_prefix);

  g_string_append_printf (output_buffer,
                          "Signature made %s using %s key ID %s\n",
                          formatted_date_time, pubkey_algo, key_id);

  g_clear_pointer (&date_time_utc, g_date_time_unref);
  g_clear_pointer (&date_time_local, g_date_time_unref);
  g_clear_pointer (&formatted_date_time, g_free);

  if (line_prefix != NULL)
    g_string_append (output_buffer, line_prefix);

  if (key_missing)
    {
      g_string_append (output_buffer,
                       "Can't check signature: public key not found\n");
    }
  else if (valid)
    {
      g_string_append_printf (output_buffer,
                              "Good signature from \"%s <%s>\"\n",
                              user_name, user_email);
    }
  else if (key_revoked)
    {
      g_string_append (output_buffer, "Key revoked\n");
    }
  else if (sig_expired)
    {
      g_string_append_printf (output_buffer,
                              "Expired signature from \"%s <%s>\"\n",
                              user_name, user_email);
    }
  else
    {
      g_string_append_printf (output_buffer,
                              "BAD signature from \"%s <%s>\"\n",
                              user_name, user_email);
    }

  if (!key_missing && (g_strcmp0 (fingerprint, fingerprint_primary) != 0))
    {
      const char *key_id_primary;

      len = strlen (fingerprint_primary);
      key_id_primary = (len > 16) ? fingerprint_primary + len - 16 :
                                    fingerprint_primary;

      if (line_prefix != NULL)
        g_string_append (output_buffer, line_prefix);

      g_string_append_printf (output_buffer,
                              "Primary key ID %s\n", key_id_primary);
    }

  if (exp_timestamp > 0)
    append_expire_info (output_buffer, line_prefix, "Signature", exp_timestamp,
                        sig_expired);
  if (key_exp_timestamp > 0)
    append_expire_info (output_buffer, line_prefix, "Key", key_exp_timestamp,
                        key_expired);
  if (key_exp_timestamp_primary > 0 && (g_strcmp0 (fingerprint, fingerprint_primary) != 0))
    append_expire_info (output_buffer, line_prefix, "Primary key",
                        key_exp_timestamp_primary, key_expired);
}

/**
 * ostree_gpg_verify_result_require_valid_signature:
 * @result: (nullable): an #OstreeGpgVerifyResult
 * @error: A #GError
 *
 * Checks if the result contains at least one signature from the
 * trusted keyring.  You can call this function immediately after
 * ostree_repo_verify_summary() or ostree_repo_verify_commit_ext() -
 * it will handle the %NULL @result and filled @error too.
 *
 * Returns: %TRUE if @result was not %NULL and had at least one
 * signature from trusted keyring, otherwise %FALSE
 *
 * Since: 2016.6
 */
gboolean
ostree_gpg_verify_result_require_valid_signature (OstreeGpgVerifyResult *result,
                                                  GError **error)
{
  if (result == NULL)
    return FALSE;

  if (ostree_gpg_verify_result_count_valid (result) == 0)
    {
      /*
       * Join the description of each failed signature for the error message.
       * Only one error code can be returned, so if there was more than one
       * signature, use the error of the last one under the assumption that
       * it's the most recent and hopefully most likely to be made with a
       * valid key.
       */
      gint code = OSTREE_GPG_ERROR_NO_SIGNATURE;
      g_autoptr(GString) buffer = g_string_sized_new (256);
      guint nsigs = ostree_gpg_verify_result_count_all (result);

      if (nsigs == 0)
        /* In case an empty result was passed in */
        g_string_append (buffer, "No GPG signatures found");
      else
        {
          for (int i = nsigs - 1; i >= 0; i--)
            {
              g_autoptr(GVariant) info = ostree_gpg_verify_result_get_all (result, i);
              ostree_gpg_verify_result_describe_variant (info, buffer, "",
                                                         OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);

              if (i == nsigs - 1)
                {
                  gboolean key_missing, key_revoked, key_expired, sig_expired;
                  g_variant_get_child (info, OSTREE_GPG_SIGNATURE_ATTR_KEY_MISSING,
                                       "b", &key_missing);
                  g_variant_get_child (info, OSTREE_GPG_SIGNATURE_ATTR_KEY_REVOKED,
                                       "b", &key_revoked);
                  g_variant_get_child (info, OSTREE_GPG_SIGNATURE_ATTR_KEY_EXPIRED,
                                       "b", &key_expired);
                  g_variant_get_child (info, OSTREE_GPG_SIGNATURE_ATTR_SIG_EXPIRED,
                                       "b", &sig_expired);

                  if (key_missing)
                    code = OSTREE_GPG_ERROR_MISSING_KEY;
                  else if (key_revoked)
                    code = OSTREE_GPG_ERROR_REVOKED_KEY;
                  else if (key_expired)
                    code = OSTREE_GPG_ERROR_EXPIRED_KEY;
                  else if (sig_expired)
                    code = OSTREE_GPG_ERROR_EXPIRED_SIGNATURE;
                  else
                    /* Assume any other issue is a bad signature */
                    code = OSTREE_GPG_ERROR_INVALID_SIGNATURE;
                }
            }
        }

      /* Strip any trailing newlines */
      g_strchomp (buffer->str);
      g_set_error_literal (error, OSTREE_GPG_ERROR, code, buffer->str);
      return FALSE;
    }

  return TRUE;
}

G_DEFINE_QUARK (OstreeGpgError, ostree_gpg_error)
