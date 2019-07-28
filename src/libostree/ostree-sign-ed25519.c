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
 * Authors:
 *  - Denis Pynkin (d4s) <denis.pynkin@collabora.com>
 */

#include "config.h"

#include "ostree-sign-ed25519.h"
#ifdef HAVE_LIBSODIUM
#include <sodium.h>
#endif

#define OSTREE_SIGN_ED25519_NAME "ed25519"

#define OSTREE_SIGN_METADATA_ED25519_KEY "ostree.sign.ed25519"
#define OSTREE_SIGN_METADATA_ED25519_TYPE "aay"

struct _OstreeSignEd25519
{
  GObject parent;
  gboolean initialized;
  guchar *secret_key;
  guchar *public_key;
};

static void
ostree_sign_ed25519_iface_init (OstreeSignInterface *self);

G_DEFINE_TYPE_WITH_CODE (OstreeSignEd25519, ostree_sign_ed25519, G_TYPE_OBJECT,
        G_IMPLEMENT_INTERFACE (OSTREE_TYPE_SIGN, ostree_sign_ed25519_iface_init));

static void
ostree_sign_ed25519_iface_init (OstreeSignInterface *self)
{
  g_debug ("%s enter", __FUNCTION__);

  self->data = ostree_sign_ed25519_data;
  self->get_name = ostree_sign_ed25519_get_name;
  self->metadata_key = ostree_sign_ed25519_metadata_key;
  self->metadata_format = ostree_sign_ed25519_metadata_format;
  self->metadata_verify = ostree_sign_ed25519_metadata_verify;
  self->set_sk = ostree_sign_ed25519_set_sk;
  self->set_pk = ostree_sign_ed25519_set_pk;
}

static void
ostree_sign_ed25519_class_init (OstreeSignEd25519Class *self)
{
  g_debug ("%s enter", __FUNCTION__);
  GObjectClass *object_class = G_OBJECT_CLASS(self);
}

static void
ostree_sign_ed25519_init (OstreeSignEd25519 *self)
{
  g_debug ("%s enter", __FUNCTION__);

  self->initialized = TRUE;
  self->secret_key = NULL;
  self->public_key = NULL;

#ifdef HAVE_LIBSODIUM
  if (sodium_init() < 0)
    {
      self->initialized = FALSE;
      g_warning ("libsodium library couldn't be initialized");
    }
#else
  g_error ("ed25519 signature isn't supported");
#endif /* HAVE_LIBSODIUM */
}

gboolean ostree_sign_ed25519_data (OstreeSign *self,
                                   GBytes *data,
                                   GBytes **signature,
                                   GCancellable *cancellable,
                                   GError **error)
{

  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);
  OstreeSignEd25519 *sign = ostree_sign_ed25519_get_instance_private(OSTREE_SIGN_ED25519(self));

#ifdef HAVE_LIBSODIUM
  g_autofree guchar *sig = NULL;
#endif

  if ((sign->initialized != TRUE) || (sign->secret_key == NULL))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Not able to sign: libsodium library isn't initialized properly");
      goto err;
    }
#ifdef HAVE_LIBSODIUM
  unsigned long long sig_size = 0;

  sig = g_malloc0(crypto_sign_BYTES);

  if (crypto_sign_detached (sig,
                            &sig_size,
                            g_bytes_get_data (data, NULL),
                            g_bytes_get_size (data),
                            sign->secret_key))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Not able to sign the object");
      goto err;
    }

  g_debug ("sign: data hash = 0x%x", g_bytes_hash(data));
  *signature = g_bytes_new (sig, sig_size);
  return TRUE;
#endif /* HAVE_LIBSODIUM */
err:
  return FALSE;
}

gchar * ostree_sign_ed25519_get_name (OstreeSign *self)
{
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);

  g_autofree gchar *name = g_strdup (OSTREE_SIGN_ED25519_NAME);

  return g_steal_pointer (&name);
}

gchar * ostree_sign_ed25519_metadata_key (OstreeSign *self)
{
  g_debug ("%s enter", __FUNCTION__);

  g_autofree gchar *key = g_strdup(OSTREE_SIGN_METADATA_ED25519_KEY);
  return g_steal_pointer (&key);
}

gchar * ostree_sign_ed25519_metadata_format (OstreeSign *self)
{
  g_debug ("%s enter", __FUNCTION__);

  g_autofree gchar *type = g_strdup (OSTREE_SIGN_METADATA_ED25519_TYPE);
  return g_steal_pointer (&type);
}

gboolean ostree_sign_ed25519_metadata_verify (OstreeSign *self,
                                            GBytes     *data,
                                            GVariant   *signatures,
                                            GError     **error)
{
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);
  g_return_val_if_fail (data != NULL, FALSE);
  gboolean ret = FALSE;

  OstreeSignEd25519 *sign = ostree_sign_ed25519_get_instance_private(OSTREE_SIGN_ED25519(self));

  if (signatures == NULL)
    {
      g_set_error_literal (error,
                           G_IO_ERROR, G_IO_ERROR_FAILED,
                           "signature: ed25519: commit have no signatures of my type");
      goto err;
    }

  if (!g_variant_is_of_type (signatures, (GVariantType *) OSTREE_SIGN_METADATA_ED25519_TYPE))
    {
      g_set_error_literal (error,
                           G_IO_ERROR, G_IO_ERROR_FAILED,
                           "signature: ed25519: wrong type passed for verification");
      goto err;
    }

  if ((sign->initialized != TRUE) || (sign->public_key == NULL))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Not able to verify: libsodium library isn't initialized properly");
      goto err;
    }

#ifdef HAVE_LIBSODIUM
  g_debug ("verify: data hash = 0x%x", g_bytes_hash(data));

  for (gsize i = 0; i < g_variant_n_children(signatures); i++)
    {
      g_autoptr (GVariant) child = g_variant_get_child_value (signatures, i);
      g_autoptr (GBytes) signature = g_variant_get_data_as_bytes(child);

      g_autofree char * hex = g_malloc0 (crypto_sign_PUBLICKEYBYTES*2 + 1);

      g_debug("Read signature %d: %s", (gint)i, g_variant_print(child, TRUE));

      if (crypto_sign_verify_detached ((guchar *) g_variant_get_data (child),
                                       g_bytes_get_data (data, NULL),
                                       g_bytes_get_size (data),
                                       sign->public_key) != 0)
        {
          /* Incorrect signature! */
          g_debug("Signature couldn't be verified with key '%s'", 
                  sodium_bin2hex (hex, crypto_sign_PUBLICKEYBYTES*2+1, sign->public_key, crypto_sign_PUBLICKEYBYTES));
        }
      else
        {
          ret = TRUE;
          g_debug ("Signature verified successfully with key '%s'",
                   sodium_bin2hex (hex, crypto_sign_PUBLICKEYBYTES*2+1, sign->public_key, crypto_sign_PUBLICKEYBYTES));
        }
    }

  if (ret != TRUE)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Not able to verify: no valid signatures found");
#endif /* HAVE_LIBSODIUM */

  return ret;
err:
  return FALSE;
}

gboolean
ostree_sign_ed25519_keypair_generate (OstreeSign *self,
                                      GVariant **out_secret_key,
                                      GVariant **out_public_key,
                                      GError **error)
 {
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);

  OstreeSignEd25519 *sign = ostree_sign_ed25519_get_instance_private(OSTREE_SIGN_ED25519(self));

  if (sign->initialized != TRUE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Not able to sign -- libsodium library isn't initialized properly");
      goto err;
    }

#ifdef HAVE_LIBSODIUM
  unsigned char pk[crypto_sign_PUBLICKEYBYTES];
  unsigned char sk[crypto_sign_SECRETKEYBYTES];

  if (crypto_sign_keypair(pk, sk))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Not able to generate keypair");
      goto err;
    }

  *out_secret_key = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, sk, crypto_sign_SECRETKEYBYTES, sizeof(guchar));
  *out_public_key = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, pk, crypto_sign_PUBLICKEYBYTES, sizeof(guchar));

  return TRUE;
#endif /* HAVE_LIBSODIUM */

err:
  return FALSE;
}

gboolean ostree_sign_ed25519_set_sk (OstreeSign *self,
                                     GVariant *secret_key,
                                     GError **error)
{
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);

#ifdef HAVE_LIBSODIUM
  OstreeSignEd25519 *sign = ostree_sign_ed25519_get_instance_private(OSTREE_SIGN_ED25519(self));
  g_autofree char * hex = NULL;

  g_free (sign->secret_key);

  gsize n_elements = 0;
  sign->secret_key = (guchar *) g_variant_get_fixed_array (secret_key, &n_elements, sizeof(guchar));

  if (n_elements != crypto_sign_SECRETKEYBYTES)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Incorrect ed25519 secret key");
      goto err;
    }

  hex = g_malloc0 (crypto_sign_SECRETKEYBYTES*2 + 1);
  g_debug ("Set ed25519 secret key = %s", sodium_bin2hex (hex, crypto_sign_SECRETKEYBYTES*2+1, sign->secret_key, n_elements));

  return TRUE;

err:
#endif /* HAVE_LIBSODIUM */
  return FALSE;
}

gboolean ostree_sign_ed25519_set_pk (OstreeSign *self,
                                     GVariant *public_key,
                                     GError **error)
{
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);

#ifdef HAVE_LIBSODIUM
  OstreeSignEd25519 *sign = ostree_sign_ed25519_get_instance_private(OSTREE_SIGN_ED25519(self));
  g_autofree char * hex = NULL;

  gsize n_elements = 0;
  g_free (sign->public_key);
  sign->public_key = (guchar *) g_variant_get_fixed_array (public_key, &n_elements, sizeof(guchar));

  hex = g_malloc0 (crypto_sign_PUBLICKEYBYTES*2 + 1);
  g_debug ("Read ed25519 public key = %s", sodium_bin2hex (hex, crypto_sign_PUBLICKEYBYTES*2+1, sign->public_key, n_elements));

  if (n_elements != crypto_sign_PUBLICKEYBYTES)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Incorrect ed25519 public key");
      goto err;
    }

  g_debug ("Set ed25519 public key = %s", sodium_bin2hex (hex, crypto_sign_PUBLICKEYBYTES*2+1, sign->public_key, n_elements));

  return TRUE;

err:
#endif /* HAVE_LIBSODIUM */
  return FALSE;
}
