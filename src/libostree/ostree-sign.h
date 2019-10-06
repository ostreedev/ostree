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

#pragma once

#include <glib.h>
#include <glib-object.h>

#include "ostree-ref.h"
#include "ostree-remote.h"
#include "ostree-types.h"

/* Special remote */
#define OSTREE_SIGN_ALL_REMOTES "__OSTREE_ALL_REMOTES__"


G_BEGIN_DECLS

#define OSTREE_TYPE_SIGN (ostree_sign_get_type ())

_OSTREE_PUBLIC
G_DECLARE_INTERFACE (OstreeSign, ostree_sign, OSTREE, SIGN, GObject)

struct _OstreeSignInterface
{
  GTypeInterface g_iface;
  const gchar *(* get_name) (OstreeSign *self);
  gboolean (* data)   (OstreeSign *self,
                       GBytes *data,
                       GBytes **signature,
                       GCancellable *cancellable,
                       GError **error);
  gboolean (* data_verify) (OstreeSign *self,
                            GBytes *data,
                            GVariant   *metadata,
                            GError **error);
  const gchar *(* metadata_key) (OstreeSign *self);
  const gchar *(* metadata_format) (OstreeSign *self);
  gboolean (* set_sk) (OstreeSign *self,
                       GVariant *secret_key,
                       GError **error);
  gboolean (* set_pk) (OstreeSign *self,
                       GVariant *public_key,
                       GError **error);
  gboolean (* add_pk) (OstreeSign *self,
                       GVariant *public_key,
                       GError **error);
  gboolean (* load_pk) (OstreeSign *self,
                        GVariant *options,
                        GError **error);
};

_OSTREE_PUBLIC
const gchar * ostree_sign_get_name (OstreeSign *self);

_OSTREE_PUBLIC
gboolean ostree_sign_data (OstreeSign *self,
                             GBytes *data,
                             GBytes **signature,
                             GCancellable *cancellable,
                             GError **error);

_OSTREE_PUBLIC
gboolean ostree_sign_data_verify (OstreeSign *self,
                                      GBytes     *data,
                                      GVariant   *signatures,
                                      GError     **error);

_OSTREE_PUBLIC
const gchar * ostree_sign_metadata_key (OstreeSign *self);

_OSTREE_PUBLIC
const gchar * ostree_sign_metadata_format (OstreeSign *self);

_OSTREE_PUBLIC
gboolean ostree_sign_commit (OstreeSign     *self,
                             OstreeRepo     *repo,
                             const gchar    *commit_checksum,
                             GCancellable   *cancellable,
                             GError         **error);

_OSTREE_PUBLIC
gboolean ostree_sign_commit_verify (OstreeSign *self,
                                    OstreeRepo     *repo,
                                    const gchar    *commit_checksum,
                                    GCancellable   *cancellable,
                                    GError         **error);

_OSTREE_PUBLIC
gboolean ostree_sign_set_sk (OstreeSign *self,
                             GVariant *secret_key,
                             GError **error);

_OSTREE_PUBLIC
gboolean ostree_sign_set_pk (OstreeSign *self,
                             GVariant *public_key,
                             GError **error);

_OSTREE_PUBLIC
gboolean ostree_sign_add_pk (OstreeSign *self,
                             GVariant *public_key,
                             GError **error);

_OSTREE_PUBLIC
gboolean ostree_sign_load_pk (OstreeSign *self,
                              GVariant *options,
                              GError **error);


/**
 * ostree_sign_list_names:
 *
 * Return the array with all available sign modules names.
 *
 * Returns: (transfer full): an array of strings, free when you used it
 */
_OSTREE_PUBLIC
GStrv ostree_sign_list_names(void);

/**
 * ostree_sign_get_by_name:
 *
 * Tries to find and return proper signing engine by it's name.
 *
 * Returns: (transfer full): a constant, free when you used it
 */
_OSTREE_PUBLIC
OstreeSign * ostree_sign_get_by_name (const gchar *name, GError **error);

G_END_DECLS

