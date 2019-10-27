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
 */

#include "config.h"

#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include "libglnx.h"
#include "otutil.h"

#include "ostree-autocleanups.h"
#include "ostree-core.h"
#include "ostree-sign.h"
#include "ostree-sign-dummy.h"
#ifdef HAVE_LIBSODIUM
#include "ostree-sign-ed25519.h"
#endif

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "OSTreeSign"

typedef struct
{
  gchar *name;
  GType type;
} _sign_type;

_sign_type sign_types[] =
{
#if defined(HAVE_LIBSODIUM)
    {"ed25519", 0},
#endif
    {"dummy", 0}
};

enum
{
#if defined(HAVE_LIBSODIUM)
    SIGN_ED25519,
#endif
    SIGN_DUMMY
};

G_DEFINE_INTERFACE (OstreeSign, ostree_sign, G_TYPE_OBJECT)

static void
ostree_sign_default_init (OstreeSignInterface *iface)
{
  g_debug ("OstreeSign initialization");
}

const gchar * ostree_sign_metadata_key (OstreeSign *self)
{
  g_debug ("%s enter", __FUNCTION__);

  g_return_val_if_fail (OSTREE_SIGN_GET_IFACE (self)->metadata_key != NULL, NULL);
  return OSTREE_SIGN_GET_IFACE (self)->metadata_key (self);
}

const gchar * ostree_sign_metadata_format (OstreeSign *self)
{
  g_debug ("%s enter", __FUNCTION__);

  g_return_val_if_fail (OSTREE_SIGN_GET_IFACE (self)->metadata_format != NULL, NULL);
  return OSTREE_SIGN_GET_IFACE (self)->metadata_format (self);
}

gboolean ostree_sign_clear_keys (OstreeSign *self,
                                 GError **error)
{
  g_debug ("%s enter", __FUNCTION__);

  if (OSTREE_SIGN_GET_IFACE (self)->clear_keys == NULL)
    return TRUE;

  return OSTREE_SIGN_GET_IFACE (self)->clear_keys (self, error);
}

gboolean ostree_sign_set_sk (OstreeSign *self,
                             GVariant *secret_key,
                             GError **error)
{
  g_debug ("%s enter", __FUNCTION__);

  if (OSTREE_SIGN_GET_IFACE (self)->set_sk == NULL)
    return TRUE;

  return OSTREE_SIGN_GET_IFACE (self)->set_sk (self, secret_key, error);
}

gboolean ostree_sign_set_pk (OstreeSign *self,
                             GVariant *public_key,
                             GError **error)
{
  g_debug ("%s enter", __FUNCTION__);

  if (OSTREE_SIGN_GET_IFACE (self)->set_pk == NULL)
    return TRUE;

  return OSTREE_SIGN_GET_IFACE (self)->set_pk (self, public_key, error);
}

gboolean ostree_sign_add_pk (OstreeSign *self,
                             GVariant *public_key,
                             GError **error)
{
  g_debug ("%s enter", __FUNCTION__);

  if (OSTREE_SIGN_GET_IFACE (self)->add_pk == NULL)
    return TRUE;

  return OSTREE_SIGN_GET_IFACE (self)->add_pk (self, public_key, error);
}

/* Load private keys for verification from anywhere.
 * No need to have the same function for secret keys -- the signing SW must do it in it's own way
 * */
gboolean
ostree_sign_load_pk (OstreeSign *self,
                     GVariant *options,
                     GError **error)
{
  g_debug ("%s enter", __FUNCTION__);

  if (OSTREE_SIGN_GET_IFACE (self)->load_pk == NULL)
    return TRUE;

  return OSTREE_SIGN_GET_IFACE (self)->load_pk (self, options, error);
}

gboolean ostree_sign_data (OstreeSign *self,
                           GBytes *data,
                           GBytes **signature,
                           GCancellable *cancellable,
                           GError **error)
{

  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);
  g_return_val_if_fail (OSTREE_SIGN_GET_IFACE (self)->data != NULL, FALSE);

  return OSTREE_SIGN_GET_IFACE (self)->data (self, data, signature, cancellable, error);
}

gboolean
ostree_sign_data_verify (OstreeSign *self,
                             GBytes     *data,
                             GVariant   *signatures,
                             GError     **error)
{
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);
  g_return_val_if_fail (OSTREE_SIGN_GET_IFACE (self)->data_verify != NULL, FALSE);

  return OSTREE_SIGN_GET_IFACE (self)->data_verify(self, data, signatures, error);
}

/*
 * Adopted version of _ostree_detached_metadata_append_gpg_sig ()
 */
static GVariant *
_sign_detached_metadata_append (OstreeSign *self,
                                GVariant   *existing_metadata,
                                GBytes     *signature_bytes)
{
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (signature_bytes != NULL, FALSE);

  GVariantDict metadata_dict;
  g_autoptr(GVariant) signature_data = NULL;
  g_autoptr(GVariantBuilder) signature_builder = NULL;

  g_variant_dict_init (&metadata_dict, existing_metadata);

  const gchar *signature_key = ostree_sign_metadata_key(self);
  GVariantType *signature_format = (GVariantType *) ostree_sign_metadata_format(self);

  signature_data = g_variant_dict_lookup_value (&metadata_dict,
                                                signature_key,
                                                (GVariantType*)signature_format);

  /* signature_data may be NULL */
  signature_builder = ot_util_variant_builder_from_variant (signature_data, signature_format);

  g_variant_builder_add (signature_builder, "@ay", ot_gvariant_new_ay_bytes (signature_bytes));

  g_variant_dict_insert_value (&metadata_dict,
                               signature_key,
                               g_variant_builder_end (signature_builder));

  return  g_variant_dict_end (&metadata_dict);
}

gboolean
ostree_sign_commit_verify (OstreeSign     *self,
                           OstreeRepo     *repo,
                           const gchar    *commit_checksum,
                           GCancellable   *cancellable,
                           GError         **error)

{
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);

  g_autoptr(GVariant) commit_variant = NULL;
  /* Load the commit */
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 commit_checksum, &commit_variant,
                                 error))
    return glnx_prefix_error (error, "Failed to read commit");

  /* Load the metadata */
  g_autoptr(GVariant) metadata = NULL;
  if (!ostree_repo_read_commit_detached_metadata (repo,
                                                  commit_checksum,
                                                  &metadata,
                                                  cancellable,
                                                  error))
    return glnx_prefix_error (error, "Failed to read detached metadata");

  g_autoptr(GBytes) signed_data = g_variant_get_data_as_bytes (commit_variant);

  g_autoptr(GVariant) signatures = NULL;

  const gchar *signature_key = ostree_sign_metadata_key(self);
  GVariantType *signature_format = (GVariantType *) ostree_sign_metadata_format(self);

  if (metadata)
    signatures = g_variant_lookup_value (metadata,
                                         signature_key,
                                         signature_format);


  return ostree_sign_data_verify (self,
                                      signed_data,
                                      signatures,
                                      error);
}

const gchar * ostree_sign_get_name (OstreeSign *self)
{
    g_debug ("%s enter", __FUNCTION__);
    g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);
    g_return_val_if_fail (OSTREE_SIGN_GET_IFACE (self)->get_name != NULL, FALSE);

    return OSTREE_SIGN_GET_IFACE (self)->get_name (self);
}

OstreeSign * ostree_sign_get_by_name (const gchar *name, GError **error)
{
  g_debug ("%s enter", __FUNCTION__);

  OstreeSign *sign = NULL;

  /* Get types if not initialized yet */
#if defined(HAVE_LIBSODIUM)
  if (sign_types[SIGN_ED25519].type == 0)
    sign_types[SIGN_ED25519].type = OSTREE_TYPE_SIGN_ED25519;
#endif
  if (sign_types[SIGN_DUMMY].type == 0)
    sign_types[SIGN_DUMMY].type = OSTREE_TYPE_SIGN_DUMMY;

  for (gint i=0; i < G_N_ELEMENTS(sign_types); i++)
  {
    if (g_strcmp0 (name, sign_types[i].name) == 0)
      {
        g_debug ("Found '%s' signing module", sign_types[i].name);
        sign = g_object_new (sign_types[i].type, NULL);
        break;
      }
  }

  if (sign == NULL)
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Requested signature type is not implemented");

  return sign;
}


/**
 * ostree_sign_commit:
 * @self: Self
 * @commit_checksum: SHA256 of given commit to sign
 * @cancellable: A #GCancellable
 * @error: a #GError
 *
 * Add a GPG signature to a commit.
 */
gboolean
ostree_sign_commit (OstreeSign     *self,
                    OstreeRepo     *repo,
                    const gchar    *commit_checksum,
                    GCancellable   *cancellable,
                    GError         **error)
{
  g_debug ("%s enter", __FUNCTION__);

  g_autoptr(GBytes) commit_data = NULL;
  g_autoptr(GBytes) signature = NULL;
  g_autoptr(GVariant) commit_variant = NULL;
  g_autoptr(GVariant) old_metadata = NULL;
  g_autoptr(GVariant) new_metadata = NULL;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 commit_checksum, &commit_variant, error))
    return glnx_prefix_error (error, "Failed to read commit");

  if (!ostree_repo_read_commit_detached_metadata (repo,
                                                  commit_checksum,
                                                  &old_metadata,
                                                  cancellable,
                                                  error))
    return glnx_prefix_error (error, "Failed to read detached metadata");

  commit_data = g_variant_get_data_as_bytes (commit_variant);

  if (!ostree_sign_data (self, commit_data, &signature,
                         cancellable, error))
    return glnx_prefix_error (error, "Not able to sign the cobject");

  new_metadata =
    _sign_detached_metadata_append (self, old_metadata, signature);

  if (!ostree_repo_write_commit_detached_metadata (repo,
                                                   commit_checksum,
                                                   new_metadata,
                                                   cancellable,
                                                   error))
    return FALSE;

  return TRUE;
}

GStrv ostree_sign_list_names(void)
{
  g_debug ("%s enter", __FUNCTION__);

  GStrv names = g_new0 (char *, G_N_ELEMENTS(sign_types) + 1);
  gint i = 0;

  for (i=0; i < G_N_ELEMENTS(sign_types); i++)
  {
    names[i] = g_strdup(sign_types[i].name);
    g_debug ("Found '%s' signing module", names[i]);
  }

  return names;
}
