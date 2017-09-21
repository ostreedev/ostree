/*
 * Copyright © 2017 Endless Mobile, Inc.
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
 *  - Philip Withnall <withnall@endlessm.com>
 */

#pragma once

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <libglnx.h>

#include "ostree-types.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_MOCK_VOLUME_MONITOR (ostree_mock_volume_monitor_get_type ())

/* Manually expanded version of the following, omitting autoptr support (for GLib < 2.44):
G_GNUC_INTERNAL
G_DECLARE_FINAL_TYPE (OstreeMockVolumeMonitor, ostree_mock_volume_monitor, OSTREE, MOCK_VOLUME_MONITOR, GVolumeMonitor) */

G_GNUC_INTERNAL
GType ostree_mock_volume_monitor_get_type (void);

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
typedef struct _OstreeMockVolumeMonitor OstreeMockVolumeMonitor;
typedef struct { GVolumeMonitorClass parent_class; } OstreeMockVolumeMonitorClass;

static inline OstreeMockVolumeMonitor *OSTREE_MOCK_VOLUME_MONITOR (gpointer ptr) { return G_TYPE_CHECK_INSTANCE_CAST (ptr, ostree_mock_volume_monitor_get_type (), OstreeMockVolumeMonitor); }
static inline gboolean OSTREE_IS_MOCK_VOLUME_MONITOR (gpointer ptr) { return G_TYPE_CHECK_INSTANCE_TYPE (ptr, ostree_mock_volume_monitor_get_type ()); }
G_GNUC_END_IGNORE_DEPRECATIONS

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeMockVolumeMonitor, g_object_unref)

G_GNUC_INTERNAL
GVolumeMonitor *ostree_mock_volume_monitor_new (GList *mounts,
                                                GList *volumes);

#define OSTREE_TYPE_MOCK_VOLUME (ostree_mock_volume_get_type ())

/* Manually expanded version of the following, omitting autoptr support (for GLib < 2.44):
G_GNUC_INTERNAL
G_DECLARE_FINAL_TYPE (OstreeMockVolume, ostree_mock_volume, OSTREE, MOCK_VOLUME, GObject) */

G_GNUC_INTERNAL
GType ostree_mock_volume_get_type (void);

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
typedef struct _OstreeMockVolume OstreeMockVolume;
typedef struct { GObjectClass parent_class; } OstreeMockVolumeClass;

static inline OstreeMockVolume *OSTREE_MOCK_VOLUME (gpointer ptr) { return G_TYPE_CHECK_INSTANCE_CAST (ptr, ostree_mock_volume_get_type (), OstreeMockVolume); }
static inline gboolean OSTREE_IS_MOCK_VOLUME (gpointer ptr) { return G_TYPE_CHECK_INSTANCE_TYPE (ptr, ostree_mock_volume_get_type ()); }
G_GNUC_END_IGNORE_DEPRECATIONS

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeMockVolume, g_object_unref)

G_GNUC_INTERNAL
OstreeMockVolume *ostree_mock_volume_new (const gchar *name,
                                          GDrive      *drive,
                                          GMount      *mount);

#define OSTREE_TYPE_MOCK_DRIVE (ostree_mock_drive_get_type ())

/* Manually expanded version of the following, omitting autoptr support (for GLib < 2.44):
G_GNUC_INTERNAL
G_DECLARE_FINAL_TYPE (OstreeMockDrive, ostree_mock_drive, OSTREE, MOCK_DRIVE, GObject) */

G_GNUC_INTERNAL
GType ostree_mock_drive_get_type (void);

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
typedef struct _OstreeMockDrive OstreeMockDrive;
typedef struct { GObjectClass parent_class; } OstreeMockDriveClass;

static inline OstreeMockDrive *OSTREE_MOCK_DRIVE (gpointer ptr) { return G_TYPE_CHECK_INSTANCE_CAST (ptr, ostree_mock_drive_get_type (), OstreeMockDrive); }
static inline gboolean OSTREE_IS_MOCK_DRIVE (gpointer ptr) { return G_TYPE_CHECK_INSTANCE_TYPE (ptr, ostree_mock_drive_get_type ()); }
G_GNUC_END_IGNORE_DEPRECATIONS

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeMockDrive, g_object_unref)

G_GNUC_INTERNAL
OstreeMockDrive *ostree_mock_drive_new (gboolean is_removable);

#define OSTREE_TYPE_MOCK_MOUNT (ostree_mock_mount_get_type ())

/* Manually expanded version of the following, omitting autoptr support (for GLib < 2.44):
G_GNUC_INTERNAL
G_DECLARE_FINAL_TYPE (OstreeMockMount, ostree_mock_mount, OSTREE, MOCK_MOUNT, GObject) */

G_GNUC_INTERNAL
GType ostree_mock_mount_get_type (void);

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
typedef struct _OstreeMockMount OstreeMockMount;
typedef struct { GObjectClass parent_class; } OstreeMockMountClass;

static inline OstreeMockMount *OSTREE_MOCK_MOUNT (gpointer ptr) { return G_TYPE_CHECK_INSTANCE_CAST (ptr, ostree_mock_mount_get_type (), OstreeMockMount); }
static inline gboolean OSTREE_IS_MOCK_MOUNT (gpointer ptr) { return G_TYPE_CHECK_INSTANCE_TYPE (ptr, ostree_mock_mount_get_type ()); }
G_GNUC_END_IGNORE_DEPRECATIONS

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeMockMount, g_object_unref)

G_GNUC_INTERNAL
OstreeMockMount *ostree_mock_mount_new (const gchar *name,
                                        GFile       *root);

G_END_DECLS
