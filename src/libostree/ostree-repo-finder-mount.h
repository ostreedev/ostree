/*
 * Copyright © 2017 Endless Mobile, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Authors:
 *  - Philip Withnall <withnall@endlessm.com>
 */

#pragma once

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>

#include "ostree-repo-finder.h"
#include "ostree-types.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_REPO_FINDER_MOUNT (ostree_repo_finder_mount_get_type ())

/* Manually expanded version of the following, omitting autoptr support (for GLib < 2.44):
_OSTREE_PUBLIC
G_DECLARE_FINAL_TYPE (OstreeRepoFinderMount, ostree_repo_finder_mount, OSTREE, REPO_FINDER_MOUNT, GObject) */

_OSTREE_PUBLIC
GType ostree_repo_finder_mount_get_type (void);

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
typedef struct _OstreeRepoFinderMount OstreeRepoFinderMount;
typedef struct { GObjectClass parent_class; } OstreeRepoFinderMountClass;

static inline OstreeRepoFinderMount *OSTREE_REPO_FINDER_MOUNT (gpointer ptr) { return G_TYPE_CHECK_INSTANCE_CAST (ptr, ostree_repo_finder_mount_get_type (), OstreeRepoFinderMount); }
static inline gboolean OSTREE_IS_REPO_FINDER_MOUNT (gpointer ptr) { return G_TYPE_CHECK_INSTANCE_TYPE (ptr, ostree_repo_finder_mount_get_type ()); }
G_GNUC_END_IGNORE_DEPRECATIONS

_OSTREE_PUBLIC
OstreeRepoFinderMount *ostree_repo_finder_mount_new (GVolumeMonitor *monitor);

G_END_DECLS
