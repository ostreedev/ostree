/* vim:set et sw=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e2s: */
/*
 * Copyright (C) 2026 Red Hat, Inc.
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

/**
 * SECTION:ostree-sign-pgp
 * @title: OpenPGP Signing (Sequoia)
 * @short_description: OpenPGP signature backend using Sequoia PGP
 *
 * An #OstreeSign implementation that provides OpenPGP signing and
 * verification using Sequoia PGP as the cryptographic backend.
 * This replaces the legacy GPGME-based GPG support with a FIPS-capable
 * implementation following the same "point solution" pattern used by
 * rpm-sequoia and podman-sequoia.
 */

#include "config.h"

#include "ostree-sign-pgp.h"
#include <libglnx.h>
#include <string.h>

/* C FFI declarations for the ostree-sequoia Rust library */
typedef struct OstreeSequoiaCtx OstreeSequoiaCtx;
extern OstreeSequoiaCtx *ostree_sequoia_ctx_new (void);
extern void ostree_sequoia_ctx_free (OstreeSequoiaCtx *ctx);
extern void ostree_sequoia_ctx_clear_keys (OstreeSequoiaCtx *ctx);
extern int ostree_sequoia_ctx_add_public_key (OstreeSequoiaCtx *ctx, const guint8 *key_data,
                                              gsize key_len, char **error_out);
extern int ostree_sequoia_ctx_set_secret_key (OstreeSequoiaCtx *ctx, const guint8 *key_data,
                                              gsize key_len, char **error_out);
extern int ostree_sequoia_ctx_load_public_keys_from_file (OstreeSequoiaCtx *ctx, const char *path,
                                                          char **error_out);
extern int ostree_sequoia_sign (const OstreeSequoiaCtx *ctx, const guint8 *data, gsize data_len,
                                guint8 **sig_out, gsize *sig_len_out, char **error_out);
extern int ostree_sequoia_verify (const OstreeSequoiaCtx *ctx, const guint8 *data, gsize data_len,
                                  const guint8 *signature, gsize sig_len, char **message_out,
                                  char **error_out);
extern void ostree_sequoia_free_bytes (guint8 *ptr, gsize len);
extern void ostree_sequoia_free_string (char *ptr);

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "OSTreeSign"

#define OSTREE_SIGN_PGP_NAME "pgp"

/* Metadata key stored in commit detached metadata, distinct from the
 * legacy "ostree.gpgsigs" used by the GPGME path.  */
#define OSTREE_SIGN_METADATA_PGP_KEY "ostree.sign.pgp"
#define OSTREE_SIGN_METADATA_PGP_TYPE "aay"

struct _OstreeSignPgp
{
  GObject parent;
  OstreeSequoiaCtx *sq_ctx;
};

static void ostree_sign_pgp_iface_init (OstreeSignInterface *self);

G_DEFINE_TYPE_WITH_CODE (OstreeSignPgp, _ostree_sign_pgp, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_SIGN, ostree_sign_pgp_iface_init));

static void
ostree_sign_pgp_iface_init (OstreeSignInterface *self)
{
  self->data = ostree_sign_pgp_data;
  self->data_verify = ostree_sign_pgp_data_verify;
  self->get_name = ostree_sign_pgp_get_name;
  self->metadata_key = ostree_sign_pgp_metadata_key;
  self->metadata_format = ostree_sign_pgp_metadata_format;
  self->clear_keys = ostree_sign_pgp_clear_keys;
  self->set_sk = ostree_sign_pgp_set_sk;
  self->set_pk = ostree_sign_pgp_set_pk;
  self->add_pk = ostree_sign_pgp_add_pk;
  self->load_pk = ostree_sign_pgp_load_pk;
}

static void
ostree_sign_pgp_finalize (GObject *object)
{
  OstreeSignPgp *self = OSTREE_SIGN_PGP (object);
  if (self->sq_ctx != NULL)
    ostree_sequoia_ctx_free (self->sq_ctx);
  G_OBJECT_CLASS (_ostree_sign_pgp_parent_class)->finalize (object);
}

static void
_ostree_sign_pgp_class_init (OstreeSignPgpClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = ostree_sign_pgp_finalize;
}

static void
_ostree_sign_pgp_init (OstreeSignPgp *self)
{
  self->sq_ctx = ostree_sequoia_ctx_new ();
}

/* Propagate a Rust error string into a GError and free it. */
static gboolean
_propagate_rust_error (GError **error, char *rust_error)
{
  if (rust_error != NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, rust_error);
      ostree_sequoia_free_string (rust_error);
    }
  else
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "pgp: unknown error");
    }
  return FALSE;
}

gboolean
ostree_sign_pgp_data (OstreeSign *self, GBytes *data, GBytes **signature, GCancellable *cancellable,
                      GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));
  OstreeSignPgp *sign = OSTREE_SIGN_PGP (self);

  gsize data_len;
  const guint8 *data_buf = g_bytes_get_data (data, &data_len);

  guint8 *sig_buf = NULL;
  gsize sig_len = 0;
  char *rust_error = NULL;

  if (ostree_sequoia_sign (sign->sq_ctx, data_buf, data_len, &sig_buf, &sig_len, &rust_error) != 0)
    return _propagate_rust_error (error, rust_error);

  /* Take ownership: the Rust side allocated with Vec, we wrap it in
   * GBytes and free via the Rust deallocator.  */
  *signature = g_bytes_new_with_free_func (
      sig_buf, sig_len, (GDestroyNotify)ostree_sequoia_free_bytes, GSIZE_TO_POINTER (sig_len));
  return TRUE;
}

gboolean
ostree_sign_pgp_data_verify (OstreeSign *self, GBytes *data, GVariant *signatures,
                             char **out_success_message, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (data == NULL)
    return glnx_throw (error, "pgp: unable to verify NULL data");

  OstreeSignPgp *sign = OSTREE_SIGN_PGP (self);

  if (signatures == NULL)
    return glnx_throw (error, "pgp: commit has no signatures of my type");

  if (!g_variant_is_of_type (signatures, (GVariantType *)OSTREE_SIGN_METADATA_PGP_TYPE))
    return glnx_throw (error, "pgp: wrong type passed for verification");

  /* If no keys pre-loaded, try to load from default locations */
  /* (unlike ed25519, OpenPGP keys come from keyring files) */

  gsize data_len;
  const guint8 *data_buf = g_bytes_get_data (data, &data_len);

  g_autoptr (GString) all_errors = NULL;
  guint n_sigs = 0;

  for (gsize i = 0; i < g_variant_n_children (signatures); i++)
    {
      g_autoptr (GVariant) child = g_variant_get_child_value (signatures, i);
      g_autoptr (GBytes) sig_bytes = g_variant_get_data_as_bytes (child);

      gsize sig_len;
      const guint8 *sig_buf = g_bytes_get_data (sig_bytes, &sig_len);

      char *message = NULL;
      char *rust_error = NULL;
      n_sigs++;

      if (ostree_sequoia_verify (sign->sq_ctx, data_buf, data_len, sig_buf, sig_len, &message,
                                 &rust_error)
          == 0)
        {
          if (out_success_message && message)
            *out_success_message = g_strdup (message);
          ostree_sequoia_free_string (message);
          return TRUE;
        }

      /* Collect error for diagnostics */
      if (all_errors == NULL)
        all_errors = g_string_new ("");
      else
        g_string_append (all_errors, "; ");
      if (rust_error)
        {
          g_string_append (all_errors, rust_error);
          ostree_sequoia_free_string (rust_error);
        }
      ostree_sequoia_free_string (message);
    }

  if (n_sigs == 0)
    return glnx_throw (error, "pgp: no signatures found");

  return glnx_throw (error, "pgp: Signature couldn't be verified: %s",
                     all_errors ? all_errors->str : "unknown error");
}

const gchar *
ostree_sign_pgp_get_name (OstreeSign *self)
{
  g_assert (OSTREE_IS_SIGN (self));
  return OSTREE_SIGN_PGP_NAME;
}

const gchar *
ostree_sign_pgp_metadata_key (OstreeSign *self)
{
  return OSTREE_SIGN_METADATA_PGP_KEY;
}

const gchar *
ostree_sign_pgp_metadata_format (OstreeSign *self)
{
  return OSTREE_SIGN_METADATA_PGP_TYPE;
}

gboolean
ostree_sign_pgp_clear_keys (OstreeSign *self, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));
  OstreeSignPgp *sign = OSTREE_SIGN_PGP (self);
  ostree_sequoia_ctx_clear_keys (sign->sq_ctx);
  return TRUE;
}

gboolean
ostree_sign_pgp_set_sk (OstreeSign *self, GVariant *secret_key, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));
  OstreeSignPgp *sign = OSTREE_SIGN_PGP (self);

  const guint8 *key_data = NULL;
  gsize key_len = 0;
  g_autofree guint8 *decoded = NULL;

  if (g_variant_is_of_type (secret_key, G_VARIANT_TYPE_STRING))
    {
      /* Assume ASCII-armored or base64 OpenPGP key */
      const gchar *key_str = g_variant_get_string (secret_key, NULL);
      key_data = (const guint8 *)key_str;
      key_len = strlen (key_str);
    }
  else if (g_variant_is_of_type (secret_key, G_VARIANT_TYPE_BYTESTRING))
    {
      key_data = g_variant_get_fixed_array (secret_key, &key_len, sizeof (guint8));
    }
  else
    {
      return glnx_throw (error, "pgp: unknown secret key variant type");
    }

  char *rust_error = NULL;
  if (ostree_sequoia_ctx_set_secret_key (sign->sq_ctx, key_data, key_len, &rust_error) != 0)
    return _propagate_rust_error (error, rust_error);

  return TRUE;
}

gboolean
ostree_sign_pgp_set_pk (OstreeSign *self, GVariant *public_key, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));

  if (!ostree_sign_pgp_clear_keys (self, error))
    return FALSE;

  return ostree_sign_pgp_add_pk (self, public_key, error);
}

gboolean
ostree_sign_pgp_add_pk (OstreeSign *self, GVariant *public_key, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));
  OstreeSignPgp *sign = OSTREE_SIGN_PGP (self);

  const guint8 *key_data = NULL;
  gsize key_len = 0;

  if (g_variant_is_of_type (public_key, G_VARIANT_TYPE_STRING))
    {
      const gchar *key_str = g_variant_get_string (public_key, NULL);
      key_data = (const guint8 *)key_str;
      key_len = strlen (key_str);
    }
  else if (g_variant_is_of_type (public_key, G_VARIANT_TYPE_BYTESTRING))
    {
      key_data = g_variant_get_fixed_array (public_key, &key_len, sizeof (guint8));
    }
  else
    {
      return glnx_throw (error, "pgp: unknown public key variant type");
    }

  char *rust_error = NULL;
  if (ostree_sequoia_ctx_add_public_key (sign->sq_ctx, key_data, key_len, &rust_error) != 0)
    return _propagate_rust_error (error, rust_error);

  return TRUE;
}

gboolean
ostree_sign_pgp_load_pk (OstreeSign *self, GVariant *options, GError **error)
{
  g_assert (OSTREE_IS_SIGN (self));
  OstreeSignPgp *sign = OSTREE_SIGN_PGP (self);

  const gchar *filename = NULL;

  /* Load from explicit file */
  if (g_variant_lookup (options, "filename", "&s", &filename))
    {
      char *rust_error = NULL;
      if (ostree_sequoia_ctx_load_public_keys_from_file (sign->sq_ctx, filename, &rust_error) != 0)
        return _propagate_rust_error (error, rust_error);
      return TRUE;
    }

  /* Load from well-known directories */
  const gchar *custom_dir = NULL;
  g_autoptr (GPtrArray) base_dirs = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) pgp_files = g_ptr_array_new_with_free_func (g_free);

  if (g_variant_lookup (options, "basedir", "&s", &custom_dir))
    {
      g_ptr_array_add (base_dirs, g_strdup (custom_dir));
    }
  else
    {
      g_ptr_array_add (base_dirs, g_strdup ("/etc/ostree"));
      g_ptr_array_add (base_dirs, g_strdup (DATADIR "/ostree"));
    }

  gboolean found_any = FALSE;

  for (guint i = 0; i < base_dirs->len; i++)
    {
      g_autofree gchar *base_name
          = g_build_filename ((gchar *)g_ptr_array_index (base_dirs, i), "trusted.pgp", NULL);

      g_debug ("Check pgp keys from file: %s", base_name);
      g_autofree gchar *base_dir = g_strconcat (base_name, ".d", NULL);
      g_ptr_array_add (pgp_files, g_steal_pointer (&base_name));

      g_autoptr (GDir) dir = g_dir_open (base_dir, 0, NULL);
      if (dir != NULL)
        {
          const gchar *entry = NULL;
          while ((entry = g_dir_read_name (dir)) != NULL)
            {
              gchar *filepath = g_build_filename (base_dir, entry, NULL);
              g_debug ("Check pgp keys from file: %s", filepath);
              g_ptr_array_add (pgp_files, filepath);
            }
        }
    }

  for (guint i = 0; i < pgp_files->len; i++)
    {
      const gchar *path = (gchar *)g_ptr_array_index (pgp_files, i);
      char *rust_error = NULL;

      if (ostree_sequoia_ctx_load_public_keys_from_file (sign->sq_ctx, path, &rust_error) != 0)
        {
          g_debug ("pgp: could not load keys from '%s': %s", path,
                   rust_error ? rust_error : "unknown");
          ostree_sequoia_free_string (rust_error);
        }
      else
        {
          found_any = TRUE;
        }
    }

  if (!found_any)
    return glnx_throw (error, "pgp: no keys loaded");

  return TRUE;
}
