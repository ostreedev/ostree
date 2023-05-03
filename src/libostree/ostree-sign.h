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

#include <glib-object.h>
#include <glib.h>

#include "ostree-ref.h"
#include "ostree-remote.h"
#include "ostree-types.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_SIGN (ostree_sign_get_type ())

_OSTREE_PUBLIC
GType ostree_sign_get_type (void);

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
typedef struct _OstreeSign OstreeSign;
typedef struct _OstreeSignInterface OstreeSignInterface;

static inline OstreeSign *
OSTREE_SIGN (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_CAST (ptr, ostree_sign_get_type (), OstreeSign);
}
static inline gboolean
OSTREE_IS_SIGN (gpointer ptr)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (ptr, ostree_sign_get_type ());
}
static inline OstreeSignInterface *
OSTREE_SIGN_GET_IFACE (gpointer ptr)
{
  return G_TYPE_INSTANCE_GET_INTERFACE (ptr, ostree_sign_get_type (), OstreeSignInterface);
}
G_GNUC_END_IGNORE_DEPRECATIONS

/**
 * OSTREE_SIGN_NAME_ED25519:
 * The name of the default ed25519 signing type.
 *
 * Since: 2020.4
 */
#define OSTREE_SIGN_NAME_ED25519 "ed25519"

/* Have to use glib-2.44 for this
_OSTREE_PUBLIC
G_DECLARE_INTERFACE (OstreeSign, ostree_sign, OSTREE, SIGN, GObject)
*/

struct _OstreeSignInterface
{
  GTypeInterface g_iface;
  const gchar *(*get_name) (OstreeSign *self);
  gboolean (*data) (OstreeSign *self, GBytes *data, GBytes **signature, GCancellable *cancellable,
                    GError **error);
  gboolean (*data_verify) (OstreeSign *self, GBytes *data, GVariant *signatures,
                           char **out_success_message, GError **error);
  const gchar *(*metadata_key) (OstreeSign *self);
  const gchar *(*metadata_format) (OstreeSign *self);
  gboolean (*clear_keys) (OstreeSign *self, GError **error);
  gboolean (*set_sk) (OstreeSign *self, GVariant *secret_key, GError **error);
  gboolean (*set_pk) (OstreeSign *self, GVariant *public_key, GError **error);
  gboolean (*add_pk) (OstreeSign *self, GVariant *public_key, GError **error);
  gboolean (*load_pk) (OstreeSign *self, GVariant *options, GError **error);
};

_OSTREE_PUBLIC
const gchar *ostree_sign_get_name (OstreeSign *self);

_OSTREE_PUBLIC
gboolean ostree_sign_data (OstreeSign *self, GBytes *data, GBytes **signature,
                           GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sign_data_verify (OstreeSign *self, GBytes *data, GVariant *signatures,
                                  char **out_success_message, GError **error);

_OSTREE_PUBLIC
const gchar *ostree_sign_metadata_key (OstreeSign *self);

_OSTREE_PUBLIC
const gchar *ostree_sign_metadata_format (OstreeSign *self);

_OSTREE_PUBLIC
gboolean ostree_sign_commit (OstreeSign *self, OstreeRepo *repo, const gchar *commit_checksum,
                             GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sign_commit_verify (OstreeSign *self, OstreeRepo *repo,
                                    const gchar *commit_checksum, char **out_success_message,
                                    GCancellable *cancellable, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sign_clear_keys (OstreeSign *self, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sign_set_sk (OstreeSign *self, GVariant *secret_key, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sign_set_pk (OstreeSign *self, GVariant *public_key, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sign_add_pk (OstreeSign *self, GVariant *public_key, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sign_load_pk (OstreeSign *self, GVariant *options, GError **error);

_OSTREE_PUBLIC
GPtrArray *ostree_sign_get_all (void);

_OSTREE_PUBLIC
OstreeSign *ostree_sign_get_by_name (const gchar *name, GError **error);

_OSTREE_PUBLIC
gboolean ostree_sign_summary (OstreeSign *self, OstreeRepo *repo, GVariant *keys,
                              GCancellable *cancellable, GError **error);
G_END_DECLS
