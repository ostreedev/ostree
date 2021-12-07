/*
 * Copyright © 2011 Colin Walters <walters@verbum.org>
 * Copyright © 2015 Red Hat, Inc.
 * Copyright © 2017 Endless Mobile, Inc.
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
 *  - Colin Walters <walters@verbum.org>
 *  - Philip Withnall <withnall@endlessm.com>
 */

#pragma once

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>

#include "ostree-types.h"

G_BEGIN_DECLS

/**
 * OstreeRemote:
 *
 * This represents the configuration for a single remote repository. Currently,
 * remotes can only be passed around as (reference counted) opaque handles. In
 * future, more API may be added to create and interrogate them.
 *
 * Since: 2018.6
 */

_OSTREE_PUBLIC
GType ostree_remote_get_type (void) G_GNUC_CONST;
_OSTREE_PUBLIC
OstreeRemote *ostree_remote_ref (OstreeRemote *remote);
_OSTREE_PUBLIC
void ostree_remote_unref (OstreeRemote *remote);

_OSTREE_PUBLIC
const gchar *ostree_remote_get_name (OstreeRemote *remote);

_OSTREE_PUBLIC
gchar *ostree_remote_get_url (OstreeRemote *remote);

G_END_DECLS
