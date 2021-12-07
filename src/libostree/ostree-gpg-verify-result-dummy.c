/*
 * Copyright (C) 2015 Red Hat, Inc.
 * Copyright (C) 2019 Collabora Ltd.
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

#include "libglnx.h"

#include "ostree-gpg-verify-result.h"

/**
 * SECTION: ostree-gpg-verify-result
 * @title: GPG signature verification results
 * @short_description: Dummy functions for detached GPG signatures
 *
 * This file contain dummy functions for GPG signatures checks to
 * provide API backward compatibility.
 */

#ifndef OSTREE_DISABLE_GPGME
#error This file should not be compiled if GPG support is enabled
#endif

/**
 * OstreeGpgVerifyResult:
 *
 * Private instance structure.
 */
struct OstreeGpgVerifyResult {
  GObject parent;
};


typedef struct {
  GObjectClass parent_class;
} OstreeGpgVerifyResultClass;

static void ostree_gpg_verify_result_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (OstreeGpgVerifyResult,
                         ostree_gpg_verify_result,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                ostree_gpg_verify_result_initable_iface_init))

static void
ostree_gpg_verify_result_class_init (OstreeGpgVerifyResultClass *class)
{
}

static void
ostree_gpg_verify_result_initable_iface_init (GInitableIface *iface)
{
}

static void
ostree_gpg_verify_result_init (OstreeGpgVerifyResult *result)
{
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
  g_critical ("%s: GPG feature is disabled in a build time", __FUNCTION__);
  return 0;
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
  g_critical ("%s: GPG feature is disabled in a build time", __FUNCTION__);
  return 0;
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
  g_critical ("%s: GPG feature is disabled in a build time", __FUNCTION__);
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
  g_critical ("%s: GPG feature is disabled in a build time", __FUNCTION__);
  return NULL;
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

  g_critical ("%s: GPG feature is disabled in a build time", __FUNCTION__);
  return NULL;
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

  g_critical ("%s: GPG feature is disabled in a build time", __FUNCTION__);

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
  const char *type_string;

  g_return_if_fail (variant != NULL);
  g_return_if_fail (output_buffer != NULL);

  /* Verify the variant's type string.  This code is
   * not prepared to handle just any random GVariant. */
  type_string = g_variant_get_type_string (variant);
  g_return_if_fail (strcmp (type_string, "(bbbbbsxxsssssxx)") == 0);

  g_string_append (output_buffer,
                   "GPG feature is disabled in a build time\n");

  g_critical ("%s: GPG feature is disabled in a build time", __FUNCTION__);
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

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "'%s': GPG feature is disabled in a build time",
               __FUNCTION__);
  return FALSE;
}

G_DEFINE_QUARK (OstreeGpgError, ostree_gpg_error)
