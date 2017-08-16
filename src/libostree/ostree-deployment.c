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

#include "otutil.h"
#include "ostree.h"
#include "ostree-deployment-private.h"

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

/*
 * ostree_deployment_get_osname:
 * @self: Deployemnt
 *
 * Returns: The "stateroot" name, also known as an "osname"
 */
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

void
_ostree_deployment_set_bootcsum (OstreeDeployment *self,
                                 const char *bootcsum)
{
  g_free (self->bootcsum);
  self->bootcsum = g_strdup (bootcsum);
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
  g_autoptr(OstreeBootconfigParser) new_bootconfig = NULL;
  OstreeDeployment *ret = ostree_deployment_new (self->index, self->osname, self->csum,
                                                 self->deployserial,
                                                 self->bootcsum, self->bootserial);

  new_bootconfig = ostree_bootconfig_parser_clone (self->bootconfig);
  ostree_deployment_set_bootconfig (ret, new_bootconfig);

  if (self->origin)
    {
      g_autoptr(GKeyFile) new_origin = NULL;
      g_autofree char *data = NULL;
      gsize len;
      gboolean success;

      data = g_key_file_to_data (self->origin, &len, NULL);
      g_assert (data);

      new_origin = g_key_file_new ();
      success = g_key_file_load_from_data (new_origin, data, len, 0, NULL);
      g_assert (success);

      ostree_deployment_set_origin (ret, new_origin);
    }
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

  if (a == b)
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
  self->unlocked = OSTREE_DEPLOYMENT_UNLOCKED_NONE;
  return self;
}

/**
 * ostree_deployment_get_origin_relpath:
 * @self: A deployment
 *
 * Note this function only returns a *relative* path - if you want to
 * access, it, you must either use fd-relative api such as openat(),
 * or concatenate it with the full ostree_sysroot_get_path().
 *
 * Returns: (transfer full): Path to deployment root directory, relative to sysroot
 */
char *
ostree_deployment_get_origin_relpath (OstreeDeployment *self)
{
  return g_strdup_printf ("ostree/deploy/%s/deploy/%s.%d.origin",
                          ostree_deployment_get_osname (self),
                          ostree_deployment_get_csum (self),
                          ostree_deployment_get_deployserial (self));
}

const char *
ostree_deployment_unlocked_state_to_string (OstreeDeploymentUnlockedState state)
{
  switch (state)
    {
    case OSTREE_DEPLOYMENT_UNLOCKED_NONE:
      return "none";
    case OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX:
      return "hotfix";
    case OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT:
      return "development";
    }
  g_assert_not_reached ();
}

OstreeDeploymentUnlockedState
ostree_deployment_get_unlocked (OstreeDeployment *self)
{
  return self->unlocked;
}
