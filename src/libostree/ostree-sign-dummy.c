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

#include "ostree-sign-dummy.h"
#include <string.h>

#define OSTREE_SIGN_DUMMY_NAME "dummy"

#define OSTREE_SIGN_METADATA_DUMMY_KEY "ostree.sign.dummy"
#define OSTREE_SIGN_METADATA_DUMMY_TYPE "aay"

#define OSTREE_SIGN_DUMMY_SIGNATURE "dummysign"

struct _OstreeSignDummy
{
  GObject parent;
  gchar *signature_ascii;
};

static void
ostree_sign_dummy_iface_init (OstreeSignInterface *self);

G_DEFINE_TYPE_WITH_CODE (OstreeSignDummy, ostree_sign_dummy, G_TYPE_OBJECT,
        G_IMPLEMENT_INTERFACE (OSTREE_TYPE_SIGN, ostree_sign_dummy_iface_init));

static void
ostree_sign_dummy_iface_init (OstreeSignInterface *self)
{
  g_debug ("%s enter", __FUNCTION__);

  self->data = ostree_sign_dummy_data;
  self->get_name = ostree_sign_dummy_get_name;
  self->metadata_key = ostree_sign_dummy_metadata_key;
  self->metadata_format = ostree_sign_dummy_metadata_format;
  self->metadata_verify = ostree_sign_dummy_metadata_verify;
  self->set_sk = ostree_sign_dummy_set_signature;
  self->set_pk = ostree_sign_dummy_set_signature;
}

static void
ostree_sign_dummy_class_init (OstreeSignDummyClass *self)
{
  g_debug ("%s enter", __FUNCTION__);
  GObjectClass *object_class = G_OBJECT_CLASS(self);
}

static void
ostree_sign_dummy_init (OstreeSignDummy *self)
{
  g_debug ("%s enter", __FUNCTION__);

  self->signature_ascii = g_strdup(OSTREE_SIGN_DUMMY_SIGNATURE);
}

gboolean ostree_sign_dummy_set_signature (OstreeSign *self, GVariant *key, GError **error)
{
  g_debug ("%s enter", __FUNCTION__);

  OstreeSignDummy *sign =  ostree_sign_dummy_get_instance_private(OSTREE_SIGN_DUMMY(self));

  if (sign->signature_ascii != NULL)
    g_free(sign->signature_ascii);

  sign->signature_ascii = g_variant_dup_string (key, 0);

  return TRUE;
}

gboolean ostree_sign_dummy_data (OstreeSign *self,
                                 GBytes *data,
                                 GBytes **signature,
                                 GCancellable *cancellable,
                                 GError **error)
{

  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);

  OstreeSignDummy *sign =  ostree_sign_dummy_get_instance_private(OSTREE_SIGN_DUMMY(self));

  *signature = g_bytes_new (sign->signature_ascii, strlen(sign->signature_ascii));

  return TRUE;
}

gchar * ostree_sign_dummy_get_name (OstreeSign *self)
{
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);

  g_autofree gchar *name = g_strdup(OSTREE_SIGN_DUMMY_NAME);

  return g_steal_pointer (&name);
}

gchar * ostree_sign_dummy_metadata_key (OstreeSign *self)
{
  g_debug ("%s enter", __FUNCTION__);

  g_autofree gchar *key = g_strdup(OSTREE_SIGN_METADATA_DUMMY_KEY);
  return g_steal_pointer (&key);
}

gchar * ostree_sign_dummy_metadata_format (OstreeSign *self)
{
  g_debug ("%s enter", __FUNCTION__);

  g_autofree gchar *type = g_strdup(OSTREE_SIGN_METADATA_DUMMY_TYPE);
  return g_steal_pointer (&type);
}

gboolean ostree_sign_dummy_metadata_verify (OstreeSign *self,
                                            GBytes     *data,
                                            GVariant   *signatures,
                                            GError     **error)
{
  g_debug ("%s enter", __FUNCTION__);
  g_return_val_if_fail (OSTREE_IS_SIGN (self), FALSE);
  g_return_val_if_fail (data != NULL, FALSE);

  OstreeSignDummy *sign =  ostree_sign_dummy_get_instance_private(OSTREE_SIGN_DUMMY(self));

  gboolean ret = FALSE;

  if (signatures == NULL)
    {
      g_set_error_literal (error,
                           G_IO_ERROR, G_IO_ERROR_FAILED,
                           "signature: dummy: commit have no signatures of my type");
      goto err;
    }


  if (!g_variant_is_of_type (signatures, (GVariantType *) OSTREE_SIGN_METADATA_DUMMY_TYPE))
    {
      g_set_error_literal (error,
                           G_IO_ERROR, G_IO_ERROR_FAILED,
                           "signature: dummy: wrong type passed for verification");
      goto err;
    }

  for (gsize i = 0; i < g_variant_n_children(signatures); i++)
    {
      g_autoptr (GVariant) child = g_variant_get_child_value (signatures, i);
      g_autoptr (GBytes) signature = g_variant_get_data_as_bytes(child);

      gsize sign_size = 0;
      g_bytes_get_data (signature, &sign_size);
      g_autofree gchar *sign_ascii = g_strndup(g_bytes_get_data (signature, NULL), sign_size);
      g_debug("Read signature %d: %s", (gint)i, sign_ascii);

      if (!g_strcmp0(sign_ascii, sign->signature_ascii))
          ret = TRUE;
    }

err:
  return ret;
}
