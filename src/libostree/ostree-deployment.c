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

#include "ostree-deployment.h"
#include "libgsystem.h"

struct _OstreeDeployment
{
  GObject       parent_instance;

  int index;  /* Global offset */
  char *osname;  /* osname */
  char *csum;  /* OSTree checksum of tree */
  int deployserial;  /* How many times this particular csum appears in deployment list */
  char *bootcsum;  /* Checksum of kernel+initramfs */
  int bootserial; /* An integer assigned to this tree per its ${bootcsum} */
  OstreeBootconfigParser *bootconfig; /* Bootloader configuration */
  GKeyFile *origin; /* How to construct an upgraded version of this tree */
};

typedef GObjectClass OstreeDeploymentClass;

G_DEFINE_TYPE (OstreeDeployment, ostree_deployment, G_TYPE_OBJECT)

const char *
ostree_deployment_get_csum (OstreeDeployment *self)
{
  return self->csum;
}

const char *
ostree_deployment_get_bootcsum (OstreeDeployment *self)
{
  return self->bootcsum;
}

const char *
ostree_deployment_get_osname (OstreeDeployment *self)
{
  return self->osname;
}

int
ostree_deployment_get_deployserial (OstreeDeployment *self)
{
  return self->deployserial;
}

int
ostree_deployment_get_bootserial (OstreeDeployment *self)
{
  return self->bootserial;
}

/**
 * ostree_deployment_get_bootconfig:
 * @self: Deployment
 *
 * Returns: (transfer none): Boot configuration
 */
OstreeBootconfigParser *
ostree_deployment_get_bootconfig (OstreeDeployment *self)
{
  return self->bootconfig;
}

/**
 * ostree_deployment_get_origin:
 * @self: Deployment
 *
 * Returns: (transfer none): Origin
 */
GKeyFile *
ostree_deployment_get_origin (OstreeDeployment *self)
{
  return self->origin;
}

int
ostree_deployment_get_index (OstreeDeployment *self)
{
  return self->index;
}

void
ostree_deployment_set_index (OstreeDeployment *self, int index)
{
  self->index = index;
}

void
ostree_deployment_set_bootserial (OstreeDeployment *self, int index)
{
  self->bootserial = index;
}

void
ostree_deployment_set_bootconfig (OstreeDeployment *self, OstreeBootconfigParser *bootconfig)
{
  g_clear_object (&self->bootconfig);
  if (bootconfig)
    self->bootconfig = g_object_ref (bootconfig);
}

void
ostree_deployment_set_origin (OstreeDeployment *self, GKeyFile *origin)
{
  g_clear_pointer (&self->origin, g_key_file_unref);
  if (origin)
    self->origin = g_key_file_ref (origin);
}

/**
 * ostree_deployment_clone:
 * @self: Deployment
 *
 * Returns: (transfer full): New deep copy of @self
 */
OstreeDeployment *
ostree_deployment_clone (OstreeDeployment *self)
{
  OstreeDeployment *ret = ostree_deployment_new (self->index, self->osname, self->csum,
                                         self->deployserial,
                                         self->bootcsum, self->bootserial);
  ostree_deployment_set_bootconfig (ret, self->bootconfig);
  ostree_deployment_set_origin (ret, self->origin);
  return ret;
}

guint
ostree_deployment_hash (gconstpointer v)
{
  OstreeDeployment *d = (OstreeDeployment*)v;
  return g_str_hash (ostree_deployment_get_osname (d)) +
    g_str_hash (ostree_deployment_get_csum (d)) +
    ostree_deployment_get_deployserial (d);
}

/**
 * ostree_deployment_equal:
 * @ap: (type OstreeDeployment): A deployment
 * @bp: (type OstreeDeployment): A deployment
 *
 * Returns: %TRUE if deployments have the same osname, csum, and deployserial
 */
gboolean
ostree_deployment_equal (gconstpointer ap, gconstpointer bp)
{
  OstreeDeployment *a = (OstreeDeployment*)ap;
  OstreeDeployment *b = (OstreeDeployment*)bp;
  
  if (a == NULL && b == NULL)
    return TRUE;
  else if (a != NULL && b != NULL)
    return g_str_equal (ostree_deployment_get_osname (a),
                        ostree_deployment_get_osname (b)) &&
      g_str_equal (ostree_deployment_get_csum (a),
                   ostree_deployment_get_csum (b)) &&
      ostree_deployment_get_deployserial (a) == ostree_deployment_get_deployserial (b);
  else 
    return FALSE;
}

static void
ostree_deployment_finalize (GObject *object)
{
  OstreeDeployment *self = OSTREE_DEPLOYMENT (object);

  g_free (self->osname);
  g_free (self->csum);
  g_free (self->bootcsum);
  g_clear_object (&self->bootconfig);
  g_clear_pointer (&self->origin, g_key_file_unref);

  G_OBJECT_CLASS (ostree_deployment_parent_class)->finalize (object);
}

void
ostree_deployment_init (OstreeDeployment *self)
{
}

void
ostree_deployment_class_init (OstreeDeploymentClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = ostree_deployment_finalize;
}

OstreeDeployment *
ostree_deployment_new (int    index,
                   const char  *osname,
                   const char  *csum,
                   int    deployserial,
                   const char  *bootcsum,
                   int    bootserial)
{
  OstreeDeployment *self;
  
  /* index may be -1 */
  g_return_val_if_fail (osname != NULL, NULL);
  g_return_val_if_fail (csum != NULL, NULL);
  g_return_val_if_fail (deployserial >= 0, NULL);
  /* We can have "disconnected" deployments that don't have a
     bootcsum/serial */

  self = g_object_new (OSTREE_TYPE_DEPLOYMENT, NULL);
  self->index = index;
  self->osname = g_strdup (osname);
  self->csum = g_strdup (csum);
  self->deployserial = deployserial;
  self->bootcsum = g_strdup (bootcsum);
  self->bootserial = bootserial;
  return self;
}
