/*
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
 *  - Philip Withnall <withnall@endlessm.com>
 */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>
#include <libglnx.h>

#include "ostree-types.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_MOCK_VOLUME_MONITOR (ostree_mock_volume_monitor_get_type ())

G_GNUC_INTERNAL
G_DECLARE_FINAL_TYPE (OstreeMockVolumeMonitor, ostree_mock_volume_monitor, OSTREE,
                      MOCK_VOLUME_MONITOR, GVolumeMonitor)

G_GNUC_INTERNAL
GVolumeMonitor *ostree_mock_volume_monitor_new (GList *mounts, GList *volumes);

#define OSTREE_TYPE_MOCK_VOLUME (ostree_mock_volume_get_type ())

G_GNUC_INTERNAL
G_DECLARE_FINAL_TYPE (OstreeMockVolume, ostree_mock_volume, OSTREE, MOCK_VOLUME, GObject)

G_GNUC_INTERNAL
OstreeMockVolume *ostree_mock_volume_new (const gchar *name, GDrive *drive, GMount *mount);

#define OSTREE_TYPE_MOCK_DRIVE (ostree_mock_drive_get_type ())

G_GNUC_INTERNAL
G_DECLARE_FINAL_TYPE (OstreeMockDrive, ostree_mock_drive, OSTREE, MOCK_DRIVE, GObject)

G_GNUC_INTERNAL
OstreeMockDrive *ostree_mock_drive_new (gboolean is_removable);

#define OSTREE_TYPE_MOCK_MOUNT (ostree_mock_mount_get_type ())

G_GNUC_INTERNAL
G_DECLARE_FINAL_TYPE (OstreeMockMount, ostree_mock_mount, OSTREE, MOCK_MOUNT, GObject)

G_GNUC_INTERNAL
OstreeMockMount *ostree_mock_mount_new (const gchar *name, GFile *root);

G_END_DECLS
