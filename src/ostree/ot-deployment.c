/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "ot-deployment.h"
#include "libgsystem.h"

struct _OtDeployment
{
  GObject       parent_instance;

  int index;  /* Global offset */
  char *osname;  /* osname */
  char *csum;  /* OSTree checksum of tree */
  int deployserial;  /* How many times this particular csum appears in deployment list */
  char *bootcsum;  /* Checksum of kernel+initramfs */
  int bootserial; /* An integer assigned to this tree per its ${bootcsum} */
  OtConfigParser *bootconfig; /* Bootloader configuration */
  GKeyFile *origin; /* How to construct an upgraded version of this tree */
};

typedef GObjectClass OtDeploymentClass;

G_DEFINE_TYPE (OtDeployment, ot_deployment, G_TYPE_OBJECT)

const char *
ot_deployment_get_csum (OtDeployment *self)
{
  return self->csum;
}

const char *
ot_deployment_get_bootcsum (OtDeployment *self)
{
  return self->bootcsum;
}

const char *
ot_deployment_get_osname (OtDeployment *self)
{
  return self->osname;
}

int
ot_deployment_get_deployserial (OtDeployment *self)
{
  return self->deployserial;
}

int
ot_deployment_get_bootserial (OtDeployment *self)
{
  return self->bootserial;
}

OtConfigParser *
ot_deployment_get_bootconfig (OtDeployment *self)
{
  return self->bootconfig;
}

GKeyFile *
ot_deployment_get_origin (OtDeployment *self)
{
  return self->origin;
}

int
ot_deployment_get_index (OtDeployment *self)
{
  return self->index;
}

void
ot_deployment_set_index (OtDeployment *self, int index)
{
  self->index = index;
}

void
ot_deployment_set_bootserial (OtDeployment *self, int index)
{
  self->bootserial = index;
}

void
ot_deployment_set_bootconfig (OtDeployment *self, OtConfigParser *bootconfig)
{
  g_clear_object (&self->bootconfig);
  if (bootconfig)
    self->bootconfig = g_object_ref (bootconfig);
}

void
ot_deployment_set_origin (OtDeployment *self, GKeyFile *origin)
{
  g_clear_pointer (&self->origin, g_key_file_unref);
  if (origin)
    self->origin = g_key_file_ref (origin);
}

OtDeployment *
ot_deployment_clone (OtDeployment *self)
{
  OtDeployment *ret = ot_deployment_new (self->index, self->osname, self->csum,
                                         self->deployserial,
                                         self->bootcsum, self->bootserial);
  ot_deployment_set_bootconfig (ret, self->bootconfig);
  ot_deployment_set_origin (ret, self->origin);
  return ret;
}

guint
ot_deployment_hash (gconstpointer v)
{
  OtDeployment *d = (OtDeployment*)v;
  return g_str_hash (ot_deployment_get_osname (d)) +
    g_str_hash (ot_deployment_get_csum (d)) +
    ot_deployment_get_deployserial (d);
}

gboolean
ot_deployment_equal (gconstpointer ap, gconstpointer bp)
{
  OtDeployment *a = (OtDeployment*)ap;
  OtDeployment *b = (OtDeployment*)bp;
  
  if (a == NULL && b == NULL)
    return TRUE;
  else if (a != NULL && b != NULL)
    return g_str_equal (ot_deployment_get_osname (a),
                        ot_deployment_get_osname (b)) &&
      g_str_equal (ot_deployment_get_csum (a),
                   ot_deployment_get_csum (b)) &&
      ot_deployment_get_deployserial (a) == ot_deployment_get_deployserial (b);
  else 
    return FALSE;
}

static void
ot_deployment_finalize (GObject *object)
{
  OtDeployment *self = OT_DEPLOYMENT (object);

  g_free (self->osname);
  g_free (self->csum);
  g_free (self->bootcsum);
  g_clear_object (&self->bootconfig);
  g_clear_pointer (&self->origin, g_key_file_unref);

  G_OBJECT_CLASS (ot_deployment_parent_class)->finalize (object);
}

void
ot_deployment_init (OtDeployment *self)
{
}

void
ot_deployment_class_init (OtDeploymentClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = ot_deployment_finalize;
}

OtDeployment *
ot_deployment_new (int    index,
                   const char  *osname,
                   const char  *csum,
                   int    deployserial,
                   const char  *bootcsum,
                   int    bootserial)
{
  OtDeployment *self;
  
  /* index may be -1 */
  g_return_val_if_fail (osname != NULL, NULL);
  g_return_val_if_fail (csum != NULL, NULL);
  g_return_val_if_fail (deployserial >= 0, NULL);
  /* We can have "disconnected" deployments that don't have a
     bootcsum/serial */

  self = g_object_new (OT_TYPE_DEPLOYMENT, NULL);
  self->index = index;
  self->osname = g_strdup (osname);
  self->csum = g_strdup (csum);
  self->deployserial = deployserial;
  self->bootcsum = g_strdup (bootcsum);
  self->bootserial = bootserial;
  return self;
}
