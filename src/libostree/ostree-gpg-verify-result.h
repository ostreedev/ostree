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

#pragma once

#include <gio/gio.h>
#include <ostree-types.h>

G_BEGIN_DECLS

#define OSTREE_TYPE_GPG_VERIFY_RESULT \
  (ostree_gpg_verify_result_get_type ())
#define OSTREE_GPG_VERIFY_RESULT(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSTREE_TYPE_GPG_VERIFY_RESULT, OstreeGpgVerifyResult))
#define OSTREE_IS_GPG_VERIFY_RESULT(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSTREE_TYPE_GPG_VERIFY_RESULT))

typedef struct OstreeGpgVerifyResult OstreeGpgVerifyResult;

/**
 * OstreeGpgSignatureAttr:
 * @OSTREE_GPG_SIGNATURE_ATTR_VALID:
 *   [#G_VARIANT_TYPE_BOOLEAN] Is the signature valid?
 * @OSTREE_GPG_SIGNATURE_ATTR_SIG_EXPIRED:
 *   [#G_VARIANT_TYPE_BOOLEAN] Has the signature expired?
 * @OSTREE_GPG_SIGNATURE_ATTR_KEY_EXPIRED:
 *   [#G_VARIANT_TYPE_BOOLEAN] Has the signing key expired?
 * @OSTREE_GPG_SIGNATURE_ATTR_KEY_REVOKED:
 *   [#G_VARIANT_TYPE_BOOLEAN] Has the signing key been revoked?
 * @OSTREE_GPG_SIGNATURE_ATTR_KEY_MISSING:
 *   [#G_VARIANT_TYPE_BOOLEAN] Is the signing key missing?
 * @OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT:
 *   [#G_VARIANT_TYPE_STRING] Fingerprint of the signing key
 * @OSTREE_GPG_SIGNATURE_ATTR_TIMESTAMP:
 *   [#G_VARIANT_TYPE_INT64] Signature creation Unix timestamp
 * @OSTREE_GPG_SIGNATURE_ATTR_EXP_TIMESTAMP:
 *   [#G_VARIANT_TYPE_INT64] Signature expiration Unix timestamp (0 if no
 *   expiration)
 * @OSTREE_GPG_SIGNATURE_ATTR_PUBKEY_ALGO_NAME:
 *   [#G_VARIANT_TYPE_STRING] Name of the public key algorithm used to create
 *   the signature
 * @OSTREE_GPG_SIGNATURE_ATTR_HASH_ALGO_NAME:
 *   [#G_VARIANT_TYPE_STRING] Name of the hash algorithm used to create the
 *   signature
 * @OSTREE_GPG_SIGNATURE_ATTR_USER_NAME:
 *   [#G_VARIANT_TYPE_STRING] The name of the signing key's primary user
 * @OSTREE_GPG_SIGNATURE_ATTR_USER_EMAIL:
 *   [#G_VARIANT_TYPE_STRING] The email address of the signing key's primary
 *   user
 *
 * Signature attributes available from an #OstreeGpgVerifyResult.
 * The attribute's #GVariantType is shown in brackets.
 **/
typedef enum {
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
} OstreeGpgSignatureAttr;

_OSTREE_PUBLIC
GType ostree_gpg_verify_result_get_type (void);

_OSTREE_PUBLIC
guint ostree_gpg_verify_result_count_all (OstreeGpgVerifyResult *result);

_OSTREE_PUBLIC
guint ostree_gpg_verify_result_count_valid (OstreeGpgVerifyResult *result);

_OSTREE_PUBLIC
gboolean ostree_gpg_verify_result_lookup (OstreeGpgVerifyResult *result,
                                          const gchar *key_id,
                                          guint *out_signature_index);

_OSTREE_PUBLIC
GVariant * ostree_gpg_verify_result_get (OstreeGpgVerifyResult *result,
                                         guint signature_index,
                                         OstreeGpgSignatureAttr *attrs,
                                         guint n_attrs);

_OSTREE_PUBLIC
GVariant * ostree_gpg_verify_result_get_all (OstreeGpgVerifyResult *result,
                                             guint signature_index);

/**
 * OstreeGpgSignatureFormatFlags:
 * @OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT:
 *   Use the default output format
 *
 * Formatting flags for ostree_gpg_verify_result_describe().  Currently
 * there's only one possible output format, but this enumeration allows
 * for future variations.
 **/
typedef enum {
  OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT = 0
} OstreeGpgSignatureFormatFlags;

_OSTREE_PUBLIC
void ostree_gpg_verify_result_describe (OstreeGpgVerifyResult *result,
                                        guint signature_index,
                                        GString *output_buffer,
                                        const gchar *line_prefix,
                                        OstreeGpgSignatureFormatFlags flags);

_OSTREE_PUBLIC
void ostree_gpg_verify_result_describe_variant (GVariant *variant,
                                                GString *output_buffer,
                                                const gchar *line_prefix,
                                                OstreeGpgSignatureFormatFlags flags);

_OSTREE_PUBLIC
gboolean ostree_gpg_verify_result_require_valid_signature (OstreeGpgVerifyResult *result,
                                                           GError **error);

G_END_DECLS
