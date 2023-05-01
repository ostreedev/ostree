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
#include <glib-object.h>
#include <glib.h>

#include "libglnx.h"
#include "ostree-remote.h"
#include "ostree-types.h"

G_BEGIN_DECLS

/* @refspec_name is set if this is a dynamic remote. It’s the name of the static
 * remote which this one inherits from, and is what should be used in refspecs
 * for pulls from this remote. If it’s %NULL, @name should be used instead. */
struct OstreeRemote
{
  int ref_count;      /* atomic */
  char *name;         /* (not nullable) */
  char *refspec_name; /* (nullable) */
  char *group;        /* group name in options (not nullable) */
  char *keyring;      /* keyring name ($refspec_name.trustedkeys.gpg) (not nullable) */
  GFile *file;        /* NULL if remote defined in repo/config */
  GKeyFile *options;
};

G_GNUC_INTERNAL
OstreeRemote *ostree_remote_new (const gchar *name);
G_GNUC_INTERNAL
OstreeRemote *ostree_remote_new_dynamic (const gchar *name, const gchar *refspec_name);

G_GNUC_INTERNAL
OstreeRemote *ostree_remote_new_from_keyfile (GKeyFile *keyfile, const gchar *group);

G_END_DECLS
