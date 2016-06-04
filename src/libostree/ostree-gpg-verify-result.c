/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
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
  OSTREE_GPG_SIGNATURE_ATTR_USER_EMAIL
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
      ot_gpgme_error_to_gio_error (gpg_error, error);
      g_prefix_error (error, "Unable to create context: ");
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
  g_autofree char *key_id_upper = NULL;
  gpgme_signature_t signature;
  guint signature_index;
  gboolean ret = FALSE;

  g_return_val_if_fail (OSTREE_IS_GPG_VERIFY_RESULT (result), FALSE);
  g_return_val_if_fail (key_id != NULL, FALSE);

  /* signature->fpr is always upper-case. */
  key_id_upper = g_ascii_strup (key_id, -1);

  for (signature = result->details->signatures, signature_index = 0;
       signature != NULL;
       signature = signature->next, signature_index++)
    {
      if (signature->fpr == NULL)
        continue;

      if (g_str_has_suffix (signature->fpr, key_id_upper))
        {
          if (out_signature_index != NULL)
            *out_signature_index = signature_index;
          ret = TRUE;
          break;
        }
    }

  return ret;
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
  gpgme_key_t key = NULL;
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
          attrs[ii] == OSTREE_GPG_SIGNATURE_ATTR_USER_EMAIL)
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
            child = g_variant_new_string (v_string);
            break;

          case OSTREE_GPG_SIGNATURE_ATTR_HASH_ALGO_NAME:
            v_string = gpgme_hash_algo_name (signature->hash_algo);
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

          default:
            g_critical ("Invalid signature attribute (%d)", attrs[ii]);
            g_variant_builder_clear (&builder);
            return NULL;
        }

      g_variant_builder_add_value (&builder, child);
    }

  if (key != NULL)
    gpgme_key_unref (key);

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
  const char *type_string;
  const char *fingerprint;
  const char *pubkey_algo;
  const char *user_name;
  const char *user_email;
  const char *key_id;
  gboolean valid;
  gboolean sig_expired;
  gboolean key_missing;
  gsize len;

  g_return_if_fail (variant != NULL);
  g_return_if_fail (output_buffer != NULL);

  /* Verify the variant's type string.  This code is
   * not prepared to handle just any random GVariant. */
  type_string = g_variant_get_type_string (variant);
  g_return_if_fail (strcmp (type_string, "(bbbbbsxxssss)") == 0);

  /* The default format roughly mimics the verify output generated by
   * check_sig_and_print() in gnupg/g10/mainproc.c, though obviously
   * greatly simplified. */

  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_VALID,
                       "b", &valid);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_SIG_EXPIRED,
                       "b", &sig_expired);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_KEY_MISSING,
                       "b", &key_missing);
  g_variant_get_child (variant, OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT,
                       "&s", &fingerprint);
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

  len = strlen (fingerprint);
  key_id = (len > 16) ? fingerprint + len - 16 : fingerprint;

  date_time_utc = g_date_time_new_from_unix_utc (timestamp);
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

  if (exp_timestamp > 0)
    {
      date_time_utc = g_date_time_new_from_unix_utc (exp_timestamp);
      date_time_local = g_date_time_to_local (date_time_utc);
      formatted_date_time = g_date_time_format (date_time_local, "%c");

      if (line_prefix != NULL)
        g_string_append (output_buffer, line_prefix);

      if (sig_expired)
        {
          g_string_append_printf (output_buffer,
                                  "Signature expired %s\n",
                                  formatted_date_time);
        }
      else
        {
          g_string_append_printf (output_buffer,
                                  "Signature expires %s\n",
                                  formatted_date_time);
        }
    }
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
 */
gboolean
ostree_gpg_verify_result_require_valid_signature (OstreeGpgVerifyResult *result,
                                                  GError **error)
{
  if (result == NULL)
    return FALSE;

  if (ostree_gpg_verify_result_count_valid (result) == 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "GPG signatures found, but none are in trusted keyring");
      return FALSE;
    }

  return TRUE;
}
