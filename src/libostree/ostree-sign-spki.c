/* vim:set et sw=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e2s: */
/*
 * Copyright © 2019 Collabora Ltd.
 * Copyright © 2024 Red Hat, Inc.
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

#include "ostree-sign-spki.h"
#include "otcore.h"
#include <libglnx.h>
#include <ot-checksum-utils.h>
#include <string.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "OSTreeSign"

#define OSTREE_SIGN_SPKI_NAME "spki"

typedef enum
{
  SPKI_OK,
  SPKI_NOT_SUPPORTED,
  SPKI_FAILED_INITIALIZATION
} spki_state;

struct _OstreeSignSpki
{
  GObject parent;
  spki_state state;
  GBytes *secret_key;
  GList *public_keys;  /* GBytes */
  GList *revoked_keys; /* GBytes */
};

static void ostree_sign_spki_iface_init (OstreeSignInterface *self);

G_DEFINE_TYPE_WITH_CODE (OstreeSignSpki, _ostree_sign_spki, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_SIGN, ostree_sign_spki_iface_init));

static void
ostree_sign_spki_iface_init (OstreeSignInterface *self)
{

  self->data = ostree_sign_spki_data;
  self->data_verify = ostree_sign_spki_data_verify;
  self->get_name = ostree_sign_spki_get_name;
  self->metadata_key = ostree_sign_spki_metadata_key;
  self->metadata_format = ostree_sign_spki_metadata_format;
  self->clear_keys = ostree_sign_spki_clear_keys;
  self->set_sk = ostree_sign_spki_set_sk;
  self->set_pk = ostree_sign_spki_set_pk;
  self->add_pk = ostree_sign_spki_add_pk;
  self->load_pk = ostree_sign_spki_load_pk;
}

static void
_ostree_sign_spki_class_init (OstreeSignSpkiClass *self)
{
}

static void
_ostree_sign_spki_init (OstreeSignSpki *self)
{

  self->state = SPKI_OK;
  self->secret_key = NULL;
  self->public_keys = NULL;
  self->revoked_keys = NULL;

#if !defined(USE_OPENSSL)
  self->state = SPKI_NOT_SUPPORTED;
#else
  if (!otcore_spki_init ())
    self->state = SPKI_FAILED_INITIALIZATION;
#endif
}

static gboolean
_ostree_sign_spki_is_initialized (OstreeSignSpki *self, GError **error)
{
  switch (self->state)
    {
    case SPKI_OK:
      break;
    case SPKI_NOT_SUPPORTED:
      return glnx_throw (error, "spki: engine is not supported");
    case SPKI_FAILED_INITIALIZATION:
      return glnx_throw (error, "spki: crypto library isn't initialized properly");
    }

  return TRUE;
}

gboolean
ostree_sign_spki_data (OstreeSign *self, GBytes *data, GBytes **signature,
                       GCancellable *cancellable, GError **error)
{
#if defined(USE_OPENSSL)
  g_assert (OSTREE_IS_SIGN (self));
  OstreeSignSpki *sign = _ostree_sign_spki_get_instance_private (OSTREE_SIGN_SPKI (self));

  if (!_ostree_sign_spki_is_initialized (sign, error))
    return FALSE;

  if (sign->secret_key == NULL)
    return glnx_throw (error, "Not able to sign: secret key is not set");

  gsize secret_key_size;
  const guint8 *secret_key_buf = g_bytes_get_data (sign->secret_key, &secret_key_size);

  EVP_MD_CTX *ctx = EVP_MD_CTX_new ();
  if (!ctx)
    return glnx_throw (error, "openssl: failed to allocate context");

  const unsigned char *p = secret_key_buf;
  EVP_PKEY *pkey = d2i_AutoPrivateKey (NULL, &p, secret_key_size);
  if (!pkey)
    {
      EVP_MD_CTX_free (ctx);
      return glnx_throw (error, "openssl: Failed to initialize spki key");
    }

  unsigned long long sig_size = 0;
  g_autofree guchar *sig = NULL;

  size_t len;
  if (EVP_DigestSignInit (ctx, NULL, NULL, NULL, pkey)
      && EVP_DigestSign (ctx, NULL, &len, g_bytes_get_data (data, NULL), g_bytes_get_size (data)))
    {
      sig = g_malloc0 (len);
      if (EVP_DigestSign (ctx, sig, &len, g_bytes_get_data (data, NULL), g_bytes_get_size (data)))
        sig_size = len;
    }

  EVP_PKEY_free (pkey);
  EVP_MD_CTX_free (ctx);

  if (sig_size == 0)
    return glnx_throw (error, "Failed to sign");

  *signature = g_bytes_new_take (g_steal_pointer (&sig), sig_size);
  return TRUE;
#else
  return glnx_throw (error, "spki signature validation requested, but support not compiled in");
#endif
}

gboolean
ostree_sign_spki_data_verify (OstreeSign *self, GBytes *data, GVariant *signatures,
                              char **out_success_message, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (data == NULL)
    return glnx_throw (error, "spki: unable to verify NULL data");

  OstreeSignSpki *sign = _ostree_sign_spki_get_instance_private (OSTREE_SIGN_SPKI (self));

  if (!_ostree_sign_spki_is_initialized (sign, error))
    return FALSE;

  if (signatures == NULL)
    return glnx_throw (error, "spki: commit have no signatures of my type");

  if (!g_variant_is_of_type (signatures, (GVariantType *)OSTREE_SIGN_METADATA_SPKI_TYPE))
    return glnx_throw (error, "spki: wrong type passed for verification");

  /* If no keys pre-loaded then,
   * try to load public keys from storage(s) */
  if (sign->public_keys == NULL)
    {
      g_autoptr (GVariantBuilder) builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
      g_autoptr (GVariant) options = g_variant_builder_end (builder);

      if (!ostree_sign_spki_load_pk (self, options, error))
        return FALSE;
    }

  g_debug ("verify: data hash = 0x%x", g_bytes_hash (data));

  g_autoptr (GString) invalid_signatures = NULL;
  guint n_invalid_signatures = 0;

  for (gsize i = 0; i < g_variant_n_children (signatures); i++)
    {
      g_autoptr (GVariant) child = g_variant_get_child_value (signatures, i);
      g_autoptr (GBytes) signature = g_variant_get_data_as_bytes (child);

      g_debug ("Read signature %d: %s", (gint)i, g_variant_print (child, TRUE));

      for (GList *l = sign->public_keys; l != NULL; l = l->next)
        {
          GBytes *public_key = l->data;
          /* TODO: use non-list for tons of revoked keys? */
          if (g_list_find_custom (sign->revoked_keys, public_key, g_bytes_compare) != NULL)
            {
              g_autofree char *hex = g_malloc0 (g_bytes_get_size (public_key) * 2 + 1);
              ot_bin2hex (hex, g_bytes_get_data (public_key, NULL), g_bytes_get_size (public_key));
              g_debug ("Skip revoked key '%s'", hex);
              continue;
            }

          bool valid = false;
          if (!otcore_validate_spki_signature (data, public_key, signature, &valid, error))
            return FALSE;
          if (!valid)
            {
              /* Incorrect signature! */
              if (invalid_signatures == NULL)
                invalid_signatures = g_string_new ("");
              else
                g_string_append (invalid_signatures, "; ");
              n_invalid_signatures++;
              g_autofree char *hex = g_malloc0 (g_bytes_get_size (public_key) * 2 + 1);
              ot_bin2hex (hex, g_bytes_get_data (public_key, NULL), g_bytes_get_size (public_key));
              g_string_append_printf (invalid_signatures, "key '%s'", hex);
            }
          else
            {
              if (out_success_message)
                {
                  g_autofree char *hex = g_malloc0 (g_bytes_get_size (public_key) * 2 + 1);
                  ot_bin2hex (hex, g_bytes_get_data (public_key, NULL),
                              g_bytes_get_size (public_key));
                  *out_success_message = g_strdup_printf (
                      "spki: Signature verified successfully with key '%s'", hex);
                }
              return TRUE;
            }
        }
    }

  if (invalid_signatures)
    {
      g_assert_cmpuint (n_invalid_signatures, >, 0);
      /* The test suite has a key ring with 100 keys.  This seems insane, let's
       * cap a reasonable error message at 3.
       */
      if (n_invalid_signatures > 3)
        return glnx_throw (error, "spki: Signature couldn't be verified; tried %u keys",
                           n_invalid_signatures);
      return glnx_throw (error, "spki: Signature couldn't be verified with: %s",
                         invalid_signatures->str);
    }
  return glnx_throw (error, "spki: no signatures found");
}

const gchar *
ostree_sign_spki_get_name (OstreeSign *self)
{
  g_assert (OSTREE_IS_SIGN (self));

  return OSTREE_SIGN_SPKI_NAME;
}

const gchar *
ostree_sign_spki_metadata_key (OstreeSign *self)
{

  return OSTREE_SIGN_METADATA_SPKI_KEY;
}

const gchar *
ostree_sign_spki_metadata_format (OstreeSign *self)
{

  return OSTREE_SIGN_METADATA_SPKI_TYPE;
}

gboolean
ostree_sign_spki_clear_keys (OstreeSign *self, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  OstreeSignSpki *sign = _ostree_sign_spki_get_instance_private (OSTREE_SIGN_SPKI (self));

  if (!_ostree_sign_spki_is_initialized (sign, error))
    return FALSE;

  /* Clear secret key */
  if (sign->secret_key != NULL)
    {
      gsize size;
      gpointer data = g_bytes_unref_to_data (sign->secret_key, &size);
      explicit_bzero (data, size);
      sign->secret_key = NULL;
    }

  /* Clear already loaded trusted keys */
  if (sign->public_keys != NULL)
    {
      g_list_free_full (sign->public_keys, (GDestroyNotify)g_bytes_unref);
      sign->public_keys = NULL;
    }

  /* Clear already loaded revoked keys */
  if (sign->revoked_keys != NULL)
    {
      g_list_free_full (sign->revoked_keys, (GDestroyNotify)g_bytes_unref);
      sign->revoked_keys = NULL;
    }

  return TRUE;
}

/* Support 2 representations:
 * base64 ascii -- secret key is passed as string
 * raw key -- key is passed as bytes array
 * */
gboolean
ostree_sign_spki_set_sk (OstreeSign *self, GVariant *secret_key, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (!ostree_sign_spki_clear_keys (self, error))
    return FALSE;

  OstreeSignSpki *sign = _ostree_sign_spki_get_instance_private (OSTREE_SIGN_SPKI (self));

  gsize n_elements = 0;

  g_autofree guchar *secret_key_buf = NULL;
  if (g_variant_is_of_type (secret_key, G_VARIANT_TYPE_STRING))
    {
      const gchar *sk_ascii = g_variant_get_string (secret_key, NULL);
      secret_key_buf = g_base64_decode (sk_ascii, &n_elements);
    }
  else if (g_variant_is_of_type (secret_key, G_VARIANT_TYPE_BYTESTRING))
    {
      const guchar *data = g_variant_get_fixed_array (secret_key, &n_elements, sizeof (guchar));
      secret_key_buf = g_memdup (data, n_elements);
    }
  else
    {
      return glnx_throw (error, "Unknown spki secret key type");
    }

  sign->secret_key = g_bytes_new_take (g_steal_pointer (&secret_key_buf), n_elements);

  return TRUE;
}

/* Support 2 representations:
 * base64 ascii -- public key is passed as string
 * raw key -- key is passed as bytes array
 * */
gboolean
ostree_sign_spki_set_pk (OstreeSign *self, GVariant *public_key, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (!ostree_sign_spki_clear_keys (self, error))
    return FALSE;

  return ostree_sign_spki_add_pk (self, public_key, error);
}

/* Support 2 representations:
 * base64 ascii -- public key is passed as string
 * raw key -- key is passed as bytes array
 * */
gboolean
ostree_sign_spki_add_pk (OstreeSign *self, GVariant *public_key, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  OstreeSignSpki *sign = _ostree_sign_spki_get_instance_private (OSTREE_SIGN_SPKI (self));

  if (!_ostree_sign_spki_is_initialized (sign, error))
    return FALSE;

  g_autofree guint8 *key_owned = NULL;
  const guint8 *key = NULL;
  gsize n_elements = 0;

  if (g_variant_is_of_type (public_key, G_VARIANT_TYPE_STRING))
    {
      const gchar *pk_ascii = g_variant_get_string (public_key, NULL);
      key = key_owned = g_base64_decode (pk_ascii, &n_elements);
    }
  else if (g_variant_is_of_type (public_key, G_VARIANT_TYPE_BYTESTRING))
    {
      key = g_variant_get_fixed_array (public_key, &n_elements, sizeof (guchar));
    }
  else
    {
      return glnx_throw (error, "Unknown spki public key type");
    }

  g_autofree char *hex = g_malloc0 (n_elements * 2 + 1);
  ot_bin2hex (hex, key, n_elements);
  g_debug ("Read spki public key = %s", hex);

  g_autoptr (GBytes) key_bytes = g_bytes_new_static (key, n_elements);
  if (g_list_find_custom (sign->public_keys, key_bytes, g_bytes_compare) == NULL)
    {
      GBytes *new_key_bytes = g_bytes_new (key, n_elements);
      sign->public_keys = g_list_prepend (sign->public_keys, new_key_bytes);
    }

  return TRUE;
}

/* Add revoked public key */
static gboolean
_spki_add_revoked (OstreeSign *self, GVariant *revoked_key, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  OstreeSignSpki *sign = _ostree_sign_spki_get_instance_private (OSTREE_SIGN_SPKI (self));

  g_autofree guint8 *key_owned = NULL;
  const guint8 *key = NULL;
  gsize n_elements = 0;

  if (g_variant_is_of_type (revoked_key, G_VARIANT_TYPE_STRING))
    {
      const gchar *rk_ascii = g_variant_get_string (revoked_key, NULL);
      key = key_owned = g_base64_decode (rk_ascii, &n_elements);
    }
  else if (g_variant_is_of_type (revoked_key, G_VARIANT_TYPE_BYTESTRING))
    {
      key = g_variant_get_fixed_array (revoked_key, &n_elements, sizeof (guchar));
    }
  else
    {
      return glnx_throw (error, "Unknown spki revoked key type");
    }

  g_autofree char *hex = g_malloc0 (n_elements * 2 + 1);
  ot_bin2hex (hex, key, n_elements);
  g_debug ("Read spki revoked key = %s", hex);

  g_autoptr (GBytes) key_bytes = g_bytes_new_static (key, n_elements);
  if (g_list_find_custom (sign->revoked_keys, key, g_bytes_compare) == NULL)
    {
      GBytes *new_key_bytes = g_bytes_new (key, n_elements);
      sign->revoked_keys = g_list_prepend (sign->revoked_keys, new_key_bytes);
    }

  return TRUE;
}

static gboolean
_load_pk_from_stream (OstreeSign *self, GInputStream *key_stream_in, gboolean trusted,
                      GError **error)
{
  if (key_stream_in == NULL)
    return glnx_throw (error, "spki: unable to read from NULL key-data input stream");

  gboolean ret = FALSE;

  g_autoptr (OstreeBlobReader) blob_reader = ostree_sign_read_pk (self, key_stream_in);
  g_assert (blob_reader);

  /* Use simple file format with just a list of base64 public keys per line */
  while (TRUE)
    {
      gboolean added = FALSE;
      g_autoptr (GError) local_error = NULL;
      g_autoptr (GBytes) blob = ostree_blob_reader_read_blob (blob_reader, NULL, &local_error);

      if (local_error != NULL)
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }

      if (blob == NULL)
        return ret;

      /* Read the key itself */
      g_autoptr (GVariant) pk = g_variant_new_from_bytes (G_VARIANT_TYPE_BYTESTRING, blob, FALSE);

      if (trusted)
        added = ostree_sign_spki_add_pk (self, pk, error);
      else
        added = _spki_add_revoked (self, pk, error);

      g_autofree gchar *pk_printable = g_variant_print (pk, FALSE);
      g_debug ("%s %s key: %s", added ? "Added" : "Invalid", trusted ? "public" : "revoked",
               pk_printable);

      /* Mark what we load at least one key */
      if (added)
        ret = TRUE;
    }

  return ret;
}

static gboolean
_load_pk_from_file (OstreeSign *self, const gchar *filename, gboolean trusted, GError **error)
{
  g_debug ("Processing file '%s'", filename);

  if (!g_file_test (filename, G_FILE_TEST_IS_REGULAR))
    {
      g_debug ("Can't open file '%s' with public keys", filename);
      return glnx_throw (error, "File object '%s' is not a regular file", filename);
    }

  g_autoptr (GFile) keyfile = keyfile = g_file_new_for_path (filename);
  g_autoptr (GFileInputStream) key_stream_in = g_file_read (keyfile, NULL, error);
  if (key_stream_in == NULL)
    return FALSE;

  if (!_load_pk_from_stream (self, G_INPUT_STREAM (key_stream_in), trusted, error))
    {
      if (error == NULL || *error == NULL)
        return glnx_throw (error, "signature: spki: no valid keys in file '%s'", filename);
      else
        return FALSE;
    }

  return TRUE;
}

static gboolean
_spki_load_pk (OstreeSign *self, GVariant *options, gboolean trusted, GError **error)
{

  gboolean ret = FALSE;
  const gchar *custom_dir = NULL;

  g_autoptr (GPtrArray) base_dirs = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) spki_files = g_ptr_array_new_with_free_func (g_free);

  if (g_variant_lookup (options, "basedir", "&s", &custom_dir))
    {
      /* Add custom directory */
      g_ptr_array_add (base_dirs, g_strdup (custom_dir));
    }
  else
    {
      /* Default paths where to find files with public keys */
      g_ptr_array_add (base_dirs, g_strdup ("/etc/ostree"));
      g_ptr_array_add (base_dirs, g_strdup (DATADIR "/ostree"));
    }

  /* Scan all well-known directories and construct the list with file names to scan keys */
  for (gint i = 0; i < base_dirs->len; i++)
    {
      g_autofree gchar *base_name
          = g_build_filename ((gchar *)g_ptr_array_index (base_dirs, i),
                              trusted ? "trusted.spki" : "revoked.spki", NULL);

      g_debug ("Check spki keys from file: %s", base_name);
      g_autofree gchar *base_dir = g_strconcat (base_name, ".d", NULL);
      g_ptr_array_add (spki_files, g_steal_pointer (&base_name));

      g_autoptr (GDir) dir = g_dir_open (base_dir, 0, error);
      if (dir == NULL)
        {
          g_clear_error (error);
          continue;
        }
      const gchar *entry = NULL;
      while ((entry = g_dir_read_name (dir)) != NULL)
        {
          gchar *filename = g_build_filename (base_dir, entry, NULL);
          g_debug ("Check spki keys from file: %s", filename);
          g_ptr_array_add (spki_files, filename);
        }
    }

  /* Scan all well-known files */
  for (gint i = 0; i < spki_files->len; i++)
    {
      if (!_load_pk_from_file (self, (gchar *)g_ptr_array_index (spki_files, i), trusted, error))
        {
          g_debug ("Problem with loading spki %s keys from `%s`", trusted ? "public" : "revoked",
                   (gchar *)g_ptr_array_index (spki_files, i));
          g_clear_error (error);
        }
      else
        ret = TRUE;
    }

  if (!ret && (error == NULL || *error == NULL))
    return glnx_throw (error, "signature: spki: no keys loaded");

  return ret;
}

/*
 * options argument should be a{sv}:
 * - filename -- single file to use to load keys from;
 * - basedir -- directory containing subdirectories
 *   'trusted.spki.d' and 'revoked.spki.d' with appropriate
 *   public keys. Used for testing and re-definition of system-wide
 *   directories if defaults are not suitable for any reason.
 */
gboolean
ostree_sign_spki_load_pk (OstreeSign *self, GVariant *options, GError **error)
{

  const gchar *filename = NULL;

  OstreeSignSpki *sign = _ostree_sign_spki_get_instance_private (OSTREE_SIGN_SPKI (self));
  if (!_ostree_sign_spki_is_initialized (sign, error))
    return FALSE;

  /* Read keys only from single file provided */
  if (g_variant_lookup (options, "filename", "&s", &filename))
    return _load_pk_from_file (self, filename, TRUE, error);

  /* Load public keys from well-known directories and files */
  if (!_spki_load_pk (self, options, TRUE, error))
    return FALSE;

  /* Load untrusted keys from well-known directories and files
   * Ignore the failure from this function -- it is expected to have
   * empty list of revoked keys.
   * */
  if (!_spki_load_pk (self, options, FALSE, error))
    g_clear_error (error);

  return TRUE;
}
