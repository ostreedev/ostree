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

#define G_LOG_DOMAIN "OSTreeSign"

#define OSTREE_SIGN_ED25519_NAME "ed25519"

#define OSTREE_SIGN_METADATA_ED25519_KEY "ostree.sign.ed25519"
#define OSTREE_SIGN_METADATA_ED25519_TYPE "aay"

#if 0
#define SIGNIFY_COMMENT_HEADER "untrusted comment:"
#define SIGNIFY_ID_LENGTH 8
#define SIGNIFY_MAGIC_ED25519 "Ed"
#endif

struct _OstreeSignEd25519
{
  GObject parent;
  gboolean initialized;
  guchar *secret_key;
  GList *public_keys;
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
  self->add_pk = ostree_sign_ed25519_add_pk;
  self->load_pk = ostree_sign_ed25519_load_pk;
}

static void
ostree_sign_ed25519_finalize (GObject *object)
{
  g_debug ("%s enter", __FUNCTION__);
#if 0
  OstreeSignEd25519 *self = OSTREE_SIGN_ED25519 (object);

  if (self->public_keys != NULL)
    g_list_free_full (self->public_keys, g_object_unref);
  if (self->secret_key != NULL)
    free(self->secret_key);
#endif
  G_OBJECT_CLASS (ostree_sign_ed25519_parent_class)->finalize (object);
}

static void
ostree_sign_ed25519_class_init (OstreeSignEd25519Class *self)
{
  g_debug ("%s enter", __FUNCTION__);
  GObjectClass *object_class = G_OBJECT_CLASS (self);

  object_class->finalize = ostree_sign_ed25519_finalize;
}

static void
ostree_sign_ed25519_init (OstreeSignEd25519 *self)
{
  g_debug ("%s enter", __FUNCTION__);

  self->initialized = TRUE;
  self->secret_key = NULL;
  self->public_keys = NULL;

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

  if ((sign->initialized != TRUE) || (sign->public_keys == NULL))
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

      for (GList *public_key = sign->public_keys;
           public_key != NULL;
           public_key = public_key->next)
        {
          if (crypto_sign_verify_detached ((guchar *) g_variant_get_data (child),
                                           g_bytes_get_data (data, NULL),
                                           g_bytes_get_size (data),
                                           public_key->data) != 0)
            {
              /* Incorrect signature! */
              g_debug("Signature couldn't be verified with key '%s'",
                      sodium_bin2hex (hex, crypto_sign_PUBLICKEYBYTES*2+1, public_key->data, crypto_sign_PUBLICKEYBYTES));
            }
          else
            {
              ret = TRUE;
              g_debug ("Signature verified successfully with key '%s'",
                       sodium_bin2hex (hex, crypto_sign_PUBLICKEYBYTES*2+1, public_key->data, crypto_sign_PUBLICKEYBYTES));
              break;
            }
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
//  g_debug ("Set ed25519 secret key = %s", sodium_bin2hex (hex, crypto_sign_SECRETKEYBYTES*2+1, sign->secret_key, n_elements));

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

  OstreeSignEd25519 *sign = ostree_sign_ed25519_get_instance_private(OSTREE_SIGN_ED25519(self));

  /* Substitute the key(s) with a new one */
  if (sign->public_keys != NULL)
    {
      g_list_free_full (sign->public_keys, g_object_unref);
      sign->public_keys = NULL;
    }

  return ostree_sign_ed25519_add_pk (self, public_key, error);
}

gboolean ostree_sign_ed25519_add_pk (OstreeSign *self,
                                     GVariant *public_key,
                                     GError **error)
{
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);

#ifdef HAVE_LIBSODIUM
  OstreeSignEd25519 *sign = ostree_sign_ed25519_get_instance_private(OSTREE_SIGN_ED25519(self));
  g_autofree char * hex = NULL;
  gpointer key = NULL; 

  gsize n_elements = 0;
  key = (gpointer) g_variant_get_fixed_array (public_key, &n_elements, sizeof(guchar));

  hex = g_malloc0 (crypto_sign_PUBLICKEYBYTES*2 + 1);
  g_debug ("Read ed25519 public key = %s", sodium_bin2hex (hex, crypto_sign_PUBLICKEYBYTES*2+1, key, n_elements));

  if (n_elements != crypto_sign_PUBLICKEYBYTES)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Incorrect ed25519 public key");
      goto err;
    }

  key = g_memdup (key, n_elements);
  if (g_list_find (sign->public_keys, key) == NULL)
      sign->public_keys = g_list_prepend (sign->public_keys, key);

  return TRUE;

err:
#endif /* HAVE_LIBSODIUM */
  return FALSE;
}


static gboolean
load_pk_from_stream (OstreeSign *self, GDataInputStream *key_data_in, GError **error)
{
  g_return_val_if_fail (key_data_in, FALSE);
#ifdef HAVE_LIBSODIUM
  gboolean ret = FALSE;

#if 0
/* Try to load the public key in signify format from the stream
 * https://www.openbsd.org/papers/bsdcan-signify.html
 * 
 * FIXME: Not sure if we need to support that format.
 * */
  g_autofree gchar * comment = NULL;
  while (TRUE)
    {
      gsize len = 0;
      g_autofree char *line = g_data_input_stream_read_line (key_data_in, &len, NULL, error);
      if (error)
        goto err;

      if (line)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Signify format for ed25519 public key not found");
          goto err;
        }
      
      if (comment == NULL)
        {
          /* Scan for the comment first and compare with prefix&suffix */
          if (g_str_has_prefix (line, SIGNIFY_COMMENT_HEADER) && g_str_has_suffix (line, "public key"))
            /* Save comment without the prefix and blank space */
            comment = g_strdup (line + strlen(SIGNIFY_COMMENT_HEADER) + 1);
        }
      else
        {
          /* Read the key itself */
          /* base64 encoded key */
          gsize keylen = 0;
          g_autofree guchar *key = g_base64_decode (line, &keylen);

          /* Malformed key */
          if (keylen != SIGNIFY_ID_LENGTH ||
              strncmp (line, SIGNIFY_MAGIC_ED25519, strlen(SIGNIFY_MAGIC_ED25519)) != 0)
            continue;

        }
    }
#endif /* 0 */

  /* Use simple file format with just a list of base64 public keys per line */
  while (TRUE)
    {
      gsize len = 0;
      g_autofree char *line = g_data_input_stream_read_line (key_data_in, &len, NULL, error);
      g_autoptr (GVariant) pk = NULL;

      if (*error != NULL)
        goto err;

      if (line == NULL)
          goto out;
      
      /* Read the key itself */
      /* base64 encoded key */
      gsize key_len = 0;
      g_autofree guchar *key = g_base64_decode (line, &key_len);

      pk = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, key, key_len, sizeof(guchar));
      if (ostree_sign_ed25519_add_pk (self, pk, error))
        {
          ret = TRUE;
          g_debug ("Added public key: %s", line);
        }
      else
        g_debug ("Invalid public key: %s", line);
    }

out:
  return ret;

err:
#endif /* HAVE_LIBSODIUM */
  return FALSE;
}

gboolean
ostree_sign_ed25519_load_pk (OstreeSign *self,
                             GVariant *options,
                             GError **error)
{
  g_debug ("%s enter", __FUNCTION__);

  OstreeSignEd25519 *sign = ostree_sign_ed25519_get_instance_private(OSTREE_SIGN_ED25519(self));

  g_autoptr (GFile) keyfile = NULL;
  g_autoptr (GFileInputStream) key_stream_in = NULL;
  g_autoptr (GDataInputStream) key_data_in = NULL;

  const gchar *remote_name = NULL;
  const gchar *filename = NULL;

  /* Clear already loaded keys */
  if (sign->public_keys != NULL)
    {
      g_list_free_full (sign->public_keys, g_object_unref);
      sign->public_keys = NULL;
    }

  /* Check if the name of remote is provided */
  if (! g_variant_lookup (options, "remote", "&s", &remote_name))
    remote_name = OSTREE_SIGN_ALL_REMOTES;

  /* Read filename or use will-known if not provided */
  if (! g_variant_lookup (options, "filename", "&s", &filename))
    {
      // TODO: define well-known places and load file(s)
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Please provide a filename to load");
      goto err;
    }

  keyfile = g_file_new_for_path (filename);
  key_stream_in = g_file_read (keyfile, NULL, error);
  if (key_stream_in == NULL)
    goto err;
 
  key_data_in = g_data_input_stream_new (G_INPUT_STREAM(key_stream_in));
  g_assert (key_data_in != NULL);

  if (!load_pk_from_stream (self, key_data_in, error))
    goto err;

  return TRUE;
err:
  return FALSE;
}

