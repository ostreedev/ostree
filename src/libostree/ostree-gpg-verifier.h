/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 * Copyright (C) 2013 Sjoerd Simons <sjoerd.simons@collabora.co.uk>
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
 * Author: Sjoerd Simons <sjoerd.simons@collabora.co.uk>
 */

//#pragma once

#include "ostree-gpg-verify-result.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_GPG_VERIFIER _ostree_gpg_verifier_get_type()
#define OSTREE_GPG_VERIFIER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSTREE_TYPE_GPG_VERIFIER, OstreeGpgVerifier))
#define OSTREE_IS_GPG_VERIFIER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSTREE_TYPE_GPG_VERIFIER))

typedef struct OstreeGpgVerifier OstreeGpgVerifier;

/* If this type becomes public in future, move this autoptr cleanup
 * definition to the ostree-autocleanups.h header file. Right now it
 * relies on glnx's fallback definition of the macro. */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeGpgVerifier, g_object_unref)

GType _ostree_gpg_verifier_get_type (void);

OstreeGpgVerifier *_ostree_gpg_verifier_new (void);

OstreeGpgVerifyResult *_ostree_gpg_verifier_check_signature (OstreeGpgVerifier *self,
                                                             GBytes            *signed_data,
                                                             GBytes            *signatures,
                                                             GCancellable      *cancellable,
                                                             GError           **error);

gboolean      _ostree_gpg_verifier_add_keyring_dir (OstreeGpgVerifier   *self,
                                                    GFile               *path,
                                                    GCancellable        *cancellable,
                                                    GError             **error);

gboolean      _ostree_gpg_verifier_add_keyring_dir_at (OstreeGpgVerifier   *self,
                                                       int                  dfd,
                                                       const char          *path,
                                                       GCancellable        *cancellable,
                                                       GError             **error);

gboolean      _ostree_gpg_verifier_add_global_keyring_dir (OstreeGpgVerifier  *self,
                                                           GCancellable       *cancellable,
                                                           GError            **error);

void _ostree_gpg_verifier_add_keyring_data (OstreeGpgVerifier *self,
                                            GBytes            *data);
void _ostree_gpg_verifier_add_keyring_file (OstreeGpgVerifier *self,
                                            GFile             *path);

void _ostree_gpg_verifier_add_key_ascii_file (OstreeGpgVerifier *self,
                                              const char        *path);

G_END_DECLS
