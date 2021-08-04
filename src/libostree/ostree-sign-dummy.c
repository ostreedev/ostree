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

#include <libglnx.h>
#include "ostree-sign-dummy.h"
#include <string.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "OSTreeSign"

#define OSTREE_SIGN_DUMMY_NAME "dummy"

#define OSTREE_SIGN_METADATA_DUMMY_KEY "ostree.sign.dummy"
#define OSTREE_SIGN_METADATA_DUMMY_TYPE "aay"

struct _OstreeSignDummy
{
  GObject parent;
  gchar *sk_ascii;
  gchar *pk_ascii;
};

#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeSignDummy, g_object_unref)
#endif

static void
ostree_sign_dummy_iface_init (OstreeSignInterface *self);

G_DEFINE_TYPE_WITH_CODE (OstreeSignDummy, _ostree_sign_dummy, G_TYPE_OBJECT,
        G_IMPLEMENT_INTERFACE (OSTREE_TYPE_SIGN, ostree_sign_dummy_iface_init));

static gboolean
check_dummy_sign_enabled (GError **error)
{
  if (g_strcmp0 (g_getenv ("OSTREE_DUMMY_SIGN_ENABLED"), "1") != 0)
    return glnx_throw (error, "dummy signature type is only for ostree testing");
  return TRUE;
}

static void
ostree_sign_dummy_iface_init (OstreeSignInterface *self)
{

  self->get_name = ostree_sign_dummy_get_name;
  self->data = ostree_sign_dummy_data;
  self->data_verify = ostree_sign_dummy_data_verify;
  self->metadata_key = ostree_sign_dummy_metadata_key;
  self->metadata_format = ostree_sign_dummy_metadata_format;
  self->set_sk = ostree_sign_dummy_set_sk;
  self->set_pk = ostree_sign_dummy_set_pk;
  /* Implementation for dummy engine just load the single public key */
  self->add_pk = ostree_sign_dummy_set_pk;
}

static void
_ostree_sign_dummy_class_init (OstreeSignDummyClass *self)
{
}

static void
_ostree_sign_dummy_init (OstreeSignDummy *self)
{

  self->sk_ascii = NULL;
  self->pk_ascii = NULL;
}

gboolean ostree_sign_dummy_set_sk (OstreeSign *self, GVariant *key, GError **error)
{
  if (!check_dummy_sign_enabled (error))
    return FALSE;

  OstreeSignDummy *sign =  _ostree_sign_dummy_get_instance_private(OSTREE_SIGN_DUMMY(self));

  g_free(sign->sk_ascii);

  sign->sk_ascii = g_variant_dup_string (key, 0);

  return TRUE;
}

gboolean ostree_sign_dummy_set_pk (OstreeSign *self, GVariant *key, GError **error)
{
  OstreeSignDummy *sign =  _ostree_sign_dummy_get_instance_private(OSTREE_SIGN_DUMMY(self));

  g_free(sign->pk_ascii);

  sign->pk_ascii = g_variant_dup_string (key, 0);

  return TRUE;
}

gboolean ostree_sign_dummy_data (OstreeSign *self,
                                 GBytes *data,
                                 GBytes **signature,
                                 GCancellable *cancellable,
                                 GError **error)
{
  if (!check_dummy_sign_enabled (error))
    return FALSE;

  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);

  OstreeSignDummy *sign =  _ostree_sign_dummy_get_instance_private(OSTREE_SIGN_DUMMY(self));

  *signature = g_bytes_new (sign->sk_ascii, strlen(sign->sk_ascii));

  return TRUE;
}

const gchar * ostree_sign_dummy_get_name (OstreeSign *self)
{
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);

  return OSTREE_SIGN_DUMMY_NAME;
}

const gchar * ostree_sign_dummy_metadata_key (OstreeSign *self)
{

  return OSTREE_SIGN_METADATA_DUMMY_KEY;
}

const gchar * ostree_sign_dummy_metadata_format (OstreeSign *self)
{

  return OSTREE_SIGN_METADATA_DUMMY_TYPE;
}

gboolean ostree_sign_dummy_data_verify (OstreeSign *self,
                                            GBytes     *data,
                                            GVariant   *signatures,
                                            char       **out_success_message,
                                            GError     **error)
{
  if (!check_dummy_sign_enabled (error))
    return FALSE;

  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);
  g_return_val_if_fail (data != NULL, FALSE);

  OstreeSignDummy *sign =  _ostree_sign_dummy_get_instance_private(OSTREE_SIGN_DUMMY(self));

  if (signatures == NULL)
    return glnx_throw (error, "signature: dummy: commit have no signatures of my type");

  if (!g_variant_is_of_type (signatures, (GVariantType *) OSTREE_SIGN_METADATA_DUMMY_TYPE))
    return glnx_throw (error, "signature: dummy: wrong type passed for verification");

  gsize n = g_variant_n_children(signatures);
  for (gsize i = 0; i < n; i++)
    {
      g_autoptr (GVariant) child = g_variant_get_child_value (signatures, i);
      g_autoptr (GBytes) signature = g_variant_get_data_as_bytes(child);

      gsize sign_size = 0;
      g_bytes_get_data (signature, &sign_size);
      g_autofree gchar *sign_ascii = g_strndup(g_bytes_get_data (signature, NULL), sign_size);
      g_debug("Read signature %d: %s", (gint)i, sign_ascii);
      g_debug("Stored signature %d: %s", (gint)i, sign->pk_ascii);

      if (!g_strcmp0(sign_ascii, sign->pk_ascii))
        {
          if (out_success_message)
            *out_success_message = g_strdup ("dummy: Signature verified");
          return TRUE;
        }
    }

  if (n)
    return glnx_throw (error, "signature: dummy: incorrect signatures found: %" G_GSIZE_FORMAT, n);
  return glnx_throw (error, "signature: dummy: no signatures");
}
