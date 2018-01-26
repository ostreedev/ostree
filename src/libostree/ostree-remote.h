/*
 * Copyright © 2011 Colin Walters <walters@verbum.org>
 * Copyright © 2015 Red Hat, Inc.
 * Copyright © 2017 Endless Mobile, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
 * Since: 2017.6
 */
#ifndef OSTREE_ENABLE_EXPERIMENTAL_API
/* This is in ostree-types.h otherwise */
typedef struct OstreeRemote OstreeRemote;
#endif

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
