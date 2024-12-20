/*
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

#include "otcore.h"

/* Initialize global state; may be called multiple times and is idempotent. */
bool
otcore_spki_init (void)
{
  return true;
}

/* Validate a single spki signature.  If there is an unexpected state, such
 * as an ill-forumed public key or signature, a hard error will be returned.
 *
 * If the signature is not correct, this function will return successfully, but
 * `out_valid` will be set to `false`.
 *
 * If the signature is correct, `out_valid` will be `true`.
 */
gboolean
otcore_validate_spki_signature (GBytes *data, GBytes *public_key, GBytes *signature,
                                bool *out_valid, GError **error)
{
  // Since this is signature verification code, let's verify preconditions.
  g_assert (data);
  g_assert (public_key);
  g_assert (signature);
  g_assert (out_valid);
  // It is OK for error to be NULL, though according to GError rules.

#if defined(HAVE_OPENSSL)
  gsize public_key_size;
  const guint8 *public_key_buf = g_bytes_get_data (public_key, &public_key_size);

  gsize signature_size;
  const guint8 *signature_buf = g_bytes_get_data (signature, &signature_size);

  if (public_key_size > OSTREE_SIGN_MAX_METADATA_SIZE)
    return glnx_throw (
        error, "Invalid public key of %" G_GSIZE_FORMAT " bytes, expected <= %" G_GSIZE_FORMAT,
        public_key_size, (gsize)OSTREE_SIGN_MAX_METADATA_SIZE);

  if (signature_size > OSTREE_SIGN_MAX_METADATA_SIZE)
    return glnx_throw (
        error, "Invalid signature of %" G_GSIZE_FORMAT " bytes, expected <= %" G_GSIZE_FORMAT,
        signature_size, (gsize)OSTREE_SIGN_MAX_METADATA_SIZE);

  EVP_MD_CTX *ctx = EVP_MD_CTX_new ();
  if (!ctx)
    return glnx_throw (error, "openssl: failed to allocate context");

  const unsigned char *p = public_key_buf;
  EVP_PKEY *pkey = d2i_PUBKEY (NULL, &p, public_key_size);
  if (!pkey)
    {
      EVP_MD_CTX_free (ctx);
      return glnx_throw (error, "openssl: Failed to initialize spki key");
    }
  if (EVP_DigestVerifyInit (ctx, NULL, NULL, NULL, pkey) != 0
      && EVP_DigestVerify (ctx, signature_buf, signature_size, g_bytes_get_data (data, NULL),
                           g_bytes_get_size (data))
             != 0)
    {
      *out_valid = true;
    }
  EVP_PKEY_free (pkey);
  EVP_MD_CTX_free (ctx);
  return TRUE;
#else
  return glnx_throw (error, "spki signature validation requested, but support not compiled in");
#endif
}
