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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 *
 * Authors:
 *  - Denis Pynkin (d4s) <denis.pynkin@collabora.com>
 */

#pragma once

#include "ostree-sign.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_SIGN_ED25519 (_ostree_sign_ed25519_get_type ())

GType _ostree_sign_ed25519_get_type (void);

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
typedef struct _OstreeSignEd25519 OstreeSignEd25519;
typedef struct
{
  GObjectClass parent_class;
} OstreeSignEd25519Class;

static inline OstreeSignEd25519 *
OSTREE_SIGN_ED25519 (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_CAST (ptr, _ostree_sign_ed25519_get_type (), OstreeSignEd25519);
}
static inline gboolean
OSTREE_IS_SIGN_ED25519 (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (ptr, _ostree_sign_ed25519_get_type ());
}

G_GNUC_END_IGNORE_DEPRECATIONS

/* Have to use glib-2.44 for this
_OSTREE_PUBLIC
G_DECLARE_FINAL_TYPE (OstreeSignEd25519,
                      ostree_sign_ed25519,
                      OSTREE,
                      SIGN_ED25519,
                      GObject)
*/

gboolean ostree_sign_ed25519_data (OstreeSign *self, GBytes *data, GBytes **signature,
                                   GCancellable *cancellable, GError **error);

gboolean ostree_sign_ed25519_data_verify (OstreeSign *self, GBytes *data, GVariant *signatures,
                                          char **out_success_message, GError **error);

const gchar *ostree_sign_ed25519_get_name (OstreeSign *self);
const gchar *ostree_sign_ed25519_metadata_key (OstreeSign *self);
const gchar *ostree_sign_ed25519_metadata_format (OstreeSign *self);

gboolean ostree_sign_ed25519_clear_keys (OstreeSign *self, GError **error);

gboolean ostree_sign_ed25519_set_sk (OstreeSign *self, GVariant *secret_key, GError **error);

gboolean ostree_sign_ed25519_set_pk (OstreeSign *self, GVariant *public_key, GError **error);

gboolean ostree_sign_ed25519_add_pk (OstreeSign *self, GVariant *public_key, GError **error);

gboolean ostree_sign_ed25519_load_pk (OstreeSign *self, GVariant *options, GError **error);

G_END_DECLS
