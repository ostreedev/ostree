/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include "otutil.h"
#include "libgsystem.h"

#include "ostree-sysroot.h"

/**
 * SECTION:libostree-sysroot
 * @title: Root partition mount point
 * @short_description: Manage physical root filesystem
 *
 * A #OstreeSysroot object represents a physical root filesystem,
 * which in particular should contain a toplevel /ostree directory.
 * Inside this directory is an #OstreeRepo in /ostree/repo, plus a set
 * of deployments in /ostree/deploy.
 */

struct OstreeSysroot {
  GObject parent;

  GFile *path;
  int sysroot_fd;
};

typedef struct {
  GObjectClass parent_class;
} OstreeSysrootClass;

enum {
  PROP_0,

  PROP_PATH
};

G_DEFINE_TYPE (OstreeSysroot, ostree_sysroot, G_TYPE_OBJECT)

static void
ostree_sysroot_finalize (GObject *object)
{
  OstreeSysroot *self = OSTREE_SYSROOT (object);

  g_clear_object (&self->path);

  G_OBJECT_CLASS (ostree_sysroot_parent_class)->finalize (object);
}

static void
ostree_sysroot_set_property(GObject         *object,
                            guint            prop_id,
                            const GValue    *value,
                            GParamSpec      *pspec)
{
  OstreeSysroot *self = OSTREE_SYSROOT (object);

  switch (prop_id)
    {
    case PROP_PATH:
      /* Canonicalize */
      self->path = g_file_new_for_path (gs_file_get_path_cached (g_value_get_object (value)));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_sysroot_get_property(GObject         *object,
                            guint            prop_id,
                            GValue          *value,
                            GParamSpec      *pspec)
{
  OstreeSysroot *self = OSTREE_SYSROOT (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_object (value, self->path);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_sysroot_constructed (GObject *object)
{
  OstreeSysroot *self = OSTREE_SYSROOT (object);

  g_assert (self->path != NULL);

  G_OBJECT_CLASS (ostree_sysroot_parent_class)->constructed (object);
}

static void
ostree_sysroot_class_init (OstreeSysrootClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = ostree_sysroot_constructed;
  object_class->get_property = ostree_sysroot_get_property;
  object_class->set_property = ostree_sysroot_set_property;
  object_class->finalize = ostree_sysroot_finalize;

  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_object ("path",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
ostree_sysroot_init (OstreeSysroot *self)
{
  self->sysroot_fd = -1;
}

/**
 * ostree_sysroot_new:
 * @path: Path to a system root directory
 *
 * Returns: (transfer full): An accessor object for an system root located at @path
 */
OstreeSysroot*
ostree_sysroot_new (GFile *path)
{
  return g_object_new (OSTREE_TYPE_SYSROOT, "path", path, NULL);
}

/**
 * ostree_sysroot_new_default:
 *
 * Returns: (transfer full): An accessor for the current visible root / filesystem
 */
OstreeSysroot*
ostree_sysroot_new_default (void)
{
  gs_unref_object GFile *rootfs = g_file_new_for_path ("/");
  return ostree_sysroot_new (rootfs);
}

/**
 * ostree_sysroot_get_path:
 * @self:
 *
 * Returns: (transfer none): Path to rootfs
 */
GFile *
ostree_sysroot_get_path (OstreeSysroot  *self)
{
  return self->path;
}
