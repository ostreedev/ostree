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
otcore_ed25519_init (void)
{
#if defined(HAVE_LIBSODIUM)
  static gssize initstate;
  if (g_once_init_enter (&initstate))
    {
      int val = sodium_init () >= 0 ? 1 : -1;
      g_once_init_leave (&initstate, val);
    }
  switch (initstate)
    {
    case 1:
      return true;
    case -1:
      return false;
    default:
      g_assert_not_reached ();
    }
#else
  return true;
#endif
}

/* Validate a single ed25519 signature.  If there is an unexpected state, such
 * as an ill-forumed public key or signature, a hard error will be returned.
 *
 * If the signature is not correct, this function will return successfully, but
 * `out_valid` will be set to `false`.
 *
 * If the signature is correct, `out_valid` will be `true`.
 * */
gboolean
otcore_validate_ed25519_signature (GBytes *data, GBytes *public_key, GBytes *signature,
                                   bool *out_valid, GError **error)
{
  // Since this is signature verification code, let's verify preconditions.
  g_assert (data);
  g_assert (public_key);
  g_assert (signature);
  g_assert (out_valid);
  // It is OK for error to be NULL, though according to GError rules.

#if defined(HAVE_LIBSODIUM) || defined(HAVE_OPENSSL)
  // And strictly verify pubkey and signature lengths
  if (g_bytes_get_size (public_key) != OSTREE_SIGN_ED25519_PUBKEY_SIZE)
    return glnx_throw (error, "Invalid public key of %" G_GSIZE_FORMAT " expected %" G_GSIZE_FORMAT,
                       (gsize)g_bytes_get_size (public_key),
                       (gsize)OSTREE_SIGN_ED25519_PUBKEY_SIZE);
  const guint8 *public_key_buf = g_bytes_get_data (public_key, NULL);
  if (g_bytes_get_size (signature) != OSTREE_SIGN_ED25519_SIG_SIZE)
    return glnx_throw (
        error, "Invalid signature length of %" G_GSIZE_FORMAT " bytes, expected %" G_GSIZE_FORMAT,
        (gsize)g_bytes_get_size (signature), (gsize)OSTREE_SIGN_ED25519_SIG_SIZE);
  const guint8 *signature_buf = g_bytes_get_data (signature, NULL);

#endif

#if defined(HAVE_LIBSODIUM)
  // Note that libsodium assumes the passed byte arrays for the signature and public key
  // have at least the expected length, but we checked that above.
  if (crypto_sign_verify_detached (signature_buf, g_bytes_get_data (data, NULL),
                                   g_bytes_get_size (data), public_key_buf)
      == 0)
    {
      *out_valid = true;
    }
  return TRUE;
#elif defined(HAVE_OPENSSL)
  EVP_MD_CTX *ctx = EVP_MD_CTX_new ();
  if (!ctx)
    return glnx_throw (error, "openssl: failed to allocate context");
  EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key (EVP_PKEY_ED25519, NULL, public_key_buf,
                                                OSTREE_SIGN_ED25519_PUBKEY_SIZE);
  if (!pkey)
    {
      EVP_MD_CTX_free (ctx);
      return glnx_throw (error, "openssl: Failed to initialize ed5519 key");
    }
  if (EVP_DigestVerifyInit (ctx, NULL, NULL, NULL, pkey) != 0
      && EVP_DigestVerify (ctx, signature_buf, OSTREE_SIGN_ED25519_SIG_SIZE,
                           g_bytes_get_data (data, NULL), g_bytes_get_size (data))
             != 0)
    {
      *out_valid = true;
    }
  EVP_PKEY_free (pkey);
  EVP_MD_CTX_free (ctx);
  return TRUE;
#else
  return glnx_throw (error, "ed25519 signature validation requested, but support not compiled in");
#endif
}
