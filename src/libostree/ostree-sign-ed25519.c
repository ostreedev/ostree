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

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "OSTreeSign"

#define OSTREE_SIGN_ED25519_NAME "ed25519"

#define OSTREE_SIGN_METADATA_ED25519_KEY "ostree.sign.ed25519"
#define OSTREE_SIGN_METADATA_ED25519_TYPE "aay"

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
  self->data_verify = ostree_sign_ed25519_data_verify;
  self->get_name = ostree_sign_ed25519_get_name;
  self->metadata_key = ostree_sign_ed25519_metadata_key;
  self->metadata_format = ostree_sign_ed25519_metadata_format;
  self->set_sk = ostree_sign_ed25519_set_sk;
  self->set_pk = ostree_sign_ed25519_set_pk;
  self->add_pk = ostree_sign_ed25519_add_pk;
  self->load_pk = ostree_sign_ed25519_load_pk;
}

static void
ostree_sign_ed25519_class_init (OstreeSignEd25519Class *self)
{
  g_debug ("%s enter", __FUNCTION__);
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
  guchar *sig = NULL;
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

  *signature = g_bytes_new_take (sig, sig_size);
  return TRUE;
#endif /* HAVE_LIBSODIUM */
err:
  return FALSE;
}

gboolean ostree_sign_ed25519_data_verify (OstreeSign *self,
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
      goto out;
    }

  if (!g_variant_is_of_type (signatures, (GVariantType *) OSTREE_SIGN_METADATA_ED25519_TYPE))
    {
      g_set_error_literal (error,
                           G_IO_ERROR, G_IO_ERROR_FAILED,
                           "signature: ed25519: wrong type passed for verification");
      goto out;
    }

  if (sign->initialized != TRUE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Not able to verify: libsodium library isn't initialized properly");
      goto out;
    }

#ifdef HAVE_LIBSODIUM
  /* If no keys pre-loaded then,
   * try to load public keys from storage(s) */
  if (sign->public_keys == NULL)
    {
      g_autoptr (GVariantBuilder) builder = NULL;
      g_autoptr (GVariant) options = NULL;

      builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
      options = g_variant_builder_end (builder);

      if (!ostree_sign_ed25519_load_pk (self, options, error))
        goto out;
    }

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

out:
  return ret;
}

const gchar * ostree_sign_ed25519_get_name (OstreeSign *self)
{
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);

  return OSTREE_SIGN_ED25519_NAME;
}

const gchar * ostree_sign_ed25519_metadata_key (OstreeSign *self)
{
  g_debug ("%s enter", __FUNCTION__);

  return OSTREE_SIGN_METADATA_ED25519_KEY;
}

const gchar * ostree_sign_ed25519_metadata_format (OstreeSign *self)
{
  g_debug ("%s enter", __FUNCTION__);

  return OSTREE_SIGN_METADATA_ED25519_TYPE;
}

gboolean ostree_sign_ed25519_set_sk (OstreeSign *self,
                                     GVariant *secret_key,
                                     GError **error)
{
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);

#ifdef HAVE_LIBSODIUM
  OstreeSignEd25519 *sign = ostree_sign_ed25519_get_instance_private(OSTREE_SIGN_ED25519(self));

  g_free (sign->secret_key);

  gsize n_elements = 0;
  sign->secret_key = (guchar *) g_variant_get_fixed_array (secret_key, &n_elements, sizeof(guchar));

  if (n_elements != crypto_sign_SECRETKEYBYTES)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Incorrect ed25519 secret key");
      goto err;
    }

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
      g_list_free_full (sign->public_keys, g_free);
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

  if (g_list_find (sign->public_keys, key) == NULL)
    {
      gpointer newkey = g_memdup (key, n_elements);
      sign->public_keys = g_list_prepend (sign->public_keys, newkey);
    }

  return TRUE;

err:
#endif /* HAVE_LIBSODIUM */
  return FALSE;
}


static gboolean
_load_pk_from_stream (OstreeSign *self, GDataInputStream *key_data_in, GError **error)
{
  g_return_val_if_fail (key_data_in, FALSE);
#ifdef HAVE_LIBSODIUM
  gboolean ret = FALSE;

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

static gboolean
_load_pk_from_file (OstreeSign *self,
                    const gchar *filename,
                    GError **error)
{
  g_debug ("%s enter", __FUNCTION__);
  g_debug ("Processing file '%s'", filename);

  g_autoptr (GFile) keyfile = NULL;
  g_autoptr (GFileInputStream) key_stream_in = NULL;
  g_autoptr (GDataInputStream) key_data_in = NULL;

  if (!g_file_test (filename, G_FILE_TEST_IS_REGULAR))
    {
      g_debug ("Can't open file '%s' with public keys", filename);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "File object '%s' is not a regular file", filename);
      goto err;
    }

  keyfile = g_file_new_for_path (filename);
  key_stream_in = g_file_read (keyfile, NULL, error);
  if (key_stream_in == NULL)
    goto err;
 
  key_data_in = g_data_input_stream_new (G_INPUT_STREAM(key_stream_in));
  g_assert (key_data_in != NULL);

  if (!_load_pk_from_stream (self, key_data_in, error))
    goto err;

  return TRUE;
err:
  return FALSE;
}

gboolean
ostree_sign_ed25519_load_pk (OstreeSign *self,
                             GVariant *options,
                             GError **error)
{
  g_debug ("%s enter", __FUNCTION__);

  gboolean ret = FALSE;

  /* Default paths there to find files with public keys */
  const gchar *default_dirs[] =
    {
      "/etc/ostree/trusted.ed25519.d",
      DATADIR "/ostree/trusted.ed25519.d"
    };
  const gchar *default_files[] =
    {
      "/etc/ostree/trusted.ed25519",
      DATADIR "/ostree/trusted.ed25519"
    };

  OstreeSignEd25519 *sign = ostree_sign_ed25519_get_instance_private(OSTREE_SIGN_ED25519(self));

  const gchar *filename = NULL;

  /* Clear already loaded keys */
  if (sign->public_keys != NULL)
    {
      g_list_free_full (sign->public_keys, g_free);
      sign->public_keys = NULL;
    }

  /* Read only file provided */
  if (g_variant_lookup (options, "filename", "&s", &filename))
      return _load_pk_from_file (self, filename, error);

  /* Scan all well-known files and directories */
  for (gint i=0; i < G_N_ELEMENTS(default_files); i++)
    if (!_load_pk_from_file (self, default_files[i], error))
      {
        g_debug ("Problem with loading ed25519 public keys from `%s`", default_files[i]);
        g_clear_error(error);
      }
    else
      ret = TRUE;

  /* Scan all well-known files and directories */
  for (gint i=0; i < G_N_ELEMENTS(default_dirs); i++)
    {
      g_autoptr (GDir) dir = g_dir_open (default_dirs[i], 0, error);
      if (dir == NULL)
        {
          g_clear_error (error);
          continue;
        }
      const gchar *entry = NULL;
      while ((entry = g_dir_read_name (dir)) != NULL)
        {
          filename = g_build_filename (default_dirs[i], entry, NULL);
          if (!_load_pk_from_file (self, filename, error))
            {
              g_debug ("Problem with loading ed25519 public keys from `%s`", filename);
              g_clear_error(error);
            }
          else
            ret = TRUE;
        }
    }

  return ret;
}

