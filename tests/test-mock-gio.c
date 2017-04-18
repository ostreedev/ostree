/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
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

#include "config.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>

#include "test-mock-gio.h"

/* TODO: Section docs; class docs; blog about this */

/* Mock volume monitor class. This returns a static set of data to the caller,
 * which it was initialised with. */
struct _OstreeMockVolumeMonitor
{
  GVolumeMonitor parent_instance;

  GList *mounts;  /* (element-type OstreeMockMount) */
  GList *volumes;  /* (element-type OstreeMockVolume) */
};

G_DEFINE_TYPE (OstreeMockVolumeMonitor, ostree_mock_volume_monitor, G_TYPE_VOLUME_MONITOR)

static GList *
ostree_mock_volume_monitor_get_mounts (GVolumeMonitor *monitor)
{
  OstreeMockVolumeMonitor *self = OSTREE_MOCK_VOLUME_MONITOR (monitor);
  return g_list_copy_deep (self->mounts, (GCopyFunc) g_object_ref, NULL);
}

static GList *
ostree_mock_volume_monitor_get_volumes (GVolumeMonitor *monitor)
{
  OstreeMockVolumeMonitor *self = OSTREE_MOCK_VOLUME_MONITOR (monitor);
  return g_list_copy_deep (self->volumes, (GCopyFunc) g_object_ref, NULL);
}

static void
ostree_mock_volume_monitor_init (OstreeMockVolumeMonitor *self)
{
  /* Nothing to see here. */
}

static void
ostree_mock_volume_monitor_dispose (GObject *object)
{
  OstreeMockVolumeMonitor *self = OSTREE_MOCK_VOLUME_MONITOR (object);

  g_list_free_full (self->volumes, g_object_unref);
  self->volumes = NULL;

  g_list_free_full (self->mounts, g_object_unref);
  self->mounts = NULL;

  G_OBJECT_CLASS (ostree_mock_volume_monitor_parent_class)->dispose (object);
}

static void
ostree_mock_volume_monitor_class_init (OstreeMockVolumeMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GVolumeMonitorClass *monitor_class = G_VOLUME_MONITOR_CLASS (klass);

  object_class->dispose = ostree_mock_volume_monitor_dispose;

  monitor_class->get_mounts = ostree_mock_volume_monitor_get_mounts;
  monitor_class->get_volumes = ostree_mock_volume_monitor_get_volumes;
}

/* TODO: Docs */
GVolumeMonitor *
ostree_mock_volume_monitor_new (GList *mounts,
                                GList *volumes)
{
  g_autoptr(OstreeMockVolumeMonitor) monitor = NULL;

  monitor = g_object_new (OSTREE_TYPE_MOCK_VOLUME_MONITOR, NULL);
  monitor->mounts = g_list_copy_deep (mounts, (GCopyFunc) g_object_ref, NULL);
  monitor->volumes = g_list_copy_deep (volumes, (GCopyFunc) g_object_ref, NULL);

  return g_steal_pointer (&monitor);
}

/* Mock volume class. This returns a static set of data to the caller, which it
 * was initialised with. */
struct _OstreeMockVolume
{
  GObject parent_instance;

  gchar *name;
  GDrive *drive;  /* (owned) (nullable) */
  GMount *mount;  /* (owned) (nullable) */
};

static void ostree_mock_volume_iface_init (GVolumeIface *iface);

G_DEFINE_TYPE_WITH_CODE (OstreeMockVolume, ostree_mock_volume, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME, ostree_mock_volume_iface_init))

static gchar *
ostree_mock_volume_get_name (GVolume *volume)
{
  OstreeMockVolume *self = OSTREE_MOCK_VOLUME (volume);
  return g_strdup (self->name);
}

static GDrive *
ostree_mock_volume_get_drive (GVolume *volume)
{
  OstreeMockVolume *self = OSTREE_MOCK_VOLUME (volume);
  return (self->drive != NULL) ? g_object_ref (self->drive) : NULL;
}

static GMount *
ostree_mock_volume_get_mount (GVolume *volume)
{
  OstreeMockVolume *self = OSTREE_MOCK_VOLUME (volume);
  return (self->mount != NULL) ? g_object_ref (self->mount) : NULL;
}

static void
ostree_mock_volume_init (OstreeMockVolume *self)
{
  /* Nothing to see here. */
}

static void
ostree_mock_volume_dispose (GObject *object)
{
  OstreeMockVolume *self = OSTREE_MOCK_VOLUME (object);

  g_clear_pointer (&self->name, g_free);
  g_clear_object (&self->drive);
  g_clear_object (&self->mount);

  G_OBJECT_CLASS (ostree_mock_volume_parent_class)->dispose (object);
}

static void
ostree_mock_volume_class_init (OstreeMockVolumeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = ostree_mock_volume_dispose;
}

static void
ostree_mock_volume_iface_init (GVolumeIface *iface)
{
  iface->get_name = ostree_mock_volume_get_name;
  iface->get_drive = ostree_mock_volume_get_drive;
  iface->get_mount = ostree_mock_volume_get_mount;
}

/* TODO: Docs */
OstreeMockVolume *
ostree_mock_volume_new (const gchar *name,
                        GDrive      *drive,
                        GMount      *mount)
{
  g_autoptr(OstreeMockVolume) volume = NULL;

  volume = g_object_new (OSTREE_TYPE_MOCK_VOLUME, NULL);
  volume->name = g_strdup (name);
  volume->drive = (drive != NULL) ? g_object_ref (drive) : NULL;
  volume->mount = (mount != NULL) ? g_object_ref (mount) : NULL;

  return g_steal_pointer (&volume);
}

/* Mock drive class. This returns a static set of data to the caller, which it
 * was initialised with. */
struct _OstreeMockDrive
{
  GObject parent_instance;

  gboolean is_removable;
};

static void ostree_mock_drive_iface_init (GDriveIface *iface);

G_DEFINE_TYPE_WITH_CODE (OstreeMockDrive, ostree_mock_drive, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_DRIVE, ostree_mock_drive_iface_init))

#if GLIB_CHECK_VERSION(2, 50, 0)
static gboolean
ostree_mock_drive_is_removable (GDrive *drive)
{
  OstreeMockDrive *self = OSTREE_MOCK_DRIVE (drive);
  return self->is_removable;
}
#endif

static void
ostree_mock_drive_init (OstreeMockDrive *self)
{
  /* Nothing to see here. */
}

static void
ostree_mock_drive_class_init (OstreeMockDriveClass *klass)
{
  /* Nothing to see here. */
}

static void
ostree_mock_drive_iface_init (GDriveIface *iface)
{
#if GLIB_CHECK_VERSION(2, 50, 0)
  iface->is_removable = ostree_mock_drive_is_removable;
#endif
}

/* TODO: Docs */
OstreeMockDrive *
ostree_mock_drive_new (gboolean is_removable)
{
  g_autoptr(OstreeMockDrive) drive = NULL;

  drive = g_object_new (OSTREE_TYPE_MOCK_DRIVE, NULL);
  drive->is_removable = is_removable;

  return g_steal_pointer (&drive);
}

/* Mock mount class. This returns a static set of data to the caller, which it
 * was initialised with. */
struct _OstreeMockMount
{
  GObject parent_instance;

  gchar *name;  /* (owned) */
  GFile *root;  /* (owned) */
};

static void ostree_mock_mount_iface_init (GMountIface *iface);

G_DEFINE_TYPE_WITH_CODE (OstreeMockMount, ostree_mock_mount, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT, ostree_mock_mount_iface_init))

static gchar *
ostree_mock_mount_get_name (GMount *mount)
{
  OstreeMockMount *self = OSTREE_MOCK_MOUNT (mount);
  return g_strdup (self->name);
}

static GFile *
ostree_mock_mount_get_root (GMount *mount)
{
  OstreeMockMount *self = OSTREE_MOCK_MOUNT (mount);
  return g_object_ref (self->root);
}

static void
ostree_mock_mount_init (OstreeMockMount *self)
{
  /* Nothing to see here. */
}

static void
ostree_mock_mount_dispose (GObject *object)
{
  OstreeMockMount *self = OSTREE_MOCK_MOUNT (object);

  g_clear_pointer (&self->name, g_free);
  g_clear_object (&self->root);

  G_OBJECT_CLASS (ostree_mock_mount_parent_class)->dispose (object);
}

static void
ostree_mock_mount_class_init (OstreeMockMountClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = ostree_mock_mount_dispose;
}

static void
ostree_mock_mount_iface_init (GMountIface *iface)
{
  iface->get_name = ostree_mock_mount_get_name;
  iface->get_root = ostree_mock_mount_get_root;
}

/* TODO: Docs */
OstreeMockMount *
ostree_mock_mount_new (const gchar *name,
                       GFile       *root)
{
  g_autoptr(OstreeMockMount) mount = NULL;

  mount = g_object_new (OSTREE_TYPE_MOCK_MOUNT, NULL);
  mount->name = g_strdup (name);
  mount->root = g_object_ref (root);

  return g_steal_pointer (&mount);
}
