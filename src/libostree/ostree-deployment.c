/*
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

/*
 * ostree_deployment_get_csum:
 * @self: Deployment
 *
 * Returns: (not nullable): The OSTree commit used for this deployment.
 */
const char *
ostree_deployment_get_csum (OstreeDeployment *self)
{
  return self->csum;
}

/*
 * ostree_deployment_get_bootcsum:
 * @self: Deployment
 *
 * Returns: (not nullable): The "boot checksum" for content installed in `/boot/ostree`.
 */
const char *
ostree_deployment_get_bootcsum (OstreeDeployment *self)
{
  return self->bootcsum;
}

/*
 * ostree_deployment_get_osname:
 * @self: Deployment
 *
 * Returns: (not nullable): The "stateroot" name, also known as an "osname"
 */
const char *
ostree_deployment_get_osname (OstreeDeployment *self)
{
  return self->osname;
}

/*
 * ostree_deployment_get_deployserial:
 * @self: Deployment
 *
 * Returns: An integer counter used to ensure multiple deployments of a commit are unique
 */
int
ostree_deployment_get_deployserial (OstreeDeployment *self)
{
  return self->deployserial;
}

/*
 * ostree_deployment_get_bootserial:
 * @self: Deployment
 *
 * Returns: An integer counter to index from shared kernels into deployments
 */
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

/**
 * ostree_deployment_get_index:
 * @self: Deployment
 *
 * Returns: The global index into the bootloader ordering
 */
int
ostree_deployment_get_index (OstreeDeployment *self)
{
  return self->index;
}

/**
 * ostree_deployment_set_index:
 * @self: Deployment
 * @index: Index into bootloader ordering
 *
 * Sets the global index into the bootloader ordering.
 */
void
ostree_deployment_set_index (OstreeDeployment *self, int index)
{
  self->index = index;
}

/**
 * ostree_deployment_set_bootserial:
 * @self: Deployment
 * @index: Don't use this
 *
 * Should never have been made public API; don't use this.
 */
void
ostree_deployment_set_bootserial (OstreeDeployment *self, int index)
{
  self->bootserial = index;
}

/**
 * ostree_deployment_set_bootconfig:
 * @self: Deployment
 * @bootconfig: (nullable): Bootloader configuration object
 *
 * Set or clear the bootloader configuration.
 */
void
ostree_deployment_set_bootconfig (OstreeDeployment *self, OstreeBootconfigParser *bootconfig)
{
  g_clear_object (&self->bootconfig);
  if (bootconfig)
    self->bootconfig = g_object_ref (bootconfig);
}

/**
 * ostree_deployment_set_origin:
 * @self: Deployment
 * @origin: (nullable): Set the origin for this deployment
 *
 * Replace the "origin", which is a description of the source
 * of the deployment and how to update to the next version.
 */
void
ostree_deployment_set_origin (OstreeDeployment *self, GKeyFile *origin)
{
  g_clear_pointer (&self->origin, g_key_file_unref);
  if (origin)
    self->origin = g_key_file_ref (origin);
}

/**
 * ostree_deployment_origin_remove_transient_state:
 * @origin: An origin
 *
 * The intention of an origin file is primarily describe the "inputs" that
 * resulted in a deployment, and it's commonly used to derive the new state. For
 * example, a key value (in pure libostree mode) is the "refspec". However,
 * libostree (or other applications) may want to store "transient" state that
 * should not be carried across upgrades.
 *
 * This function just removes all members of the `libostree-transient` group.
 * The name of that group is available to all libostree users; best practice
 * would be to prefix values underneath there with a short identifier for your
 * software.
 *
 * Additionally, this function will remove the `origin/unlocked` and
 * `origin/override-commit` members; these should be considered transient state
 * that should have been under an explicit group.
 *
 * Since: 2018.3
 */
void
ostree_deployment_origin_remove_transient_state (GKeyFile *origin)
{
  g_key_file_remove_group (origin, OSTREE_ORIGIN_TRANSIENT_GROUP, NULL);
  g_key_file_remove_key (origin, "origin", "override-commit", NULL);
  g_key_file_remove_key (origin, "origin", "unlocked", NULL);
}

void
_ostree_deployment_set_bootcsum (OstreeDeployment *self,
                                 const char *bootcsum)
{
  g_free (self->bootcsum);
  self->bootcsum = g_strdup (bootcsum);
}

void
_ostree_deployment_set_overlay_initrds (OstreeDeployment *self,
                                        char            **overlay_initrds)
{
  g_clear_pointer (&self->overlay_initrds, g_strfreev);
  g_clear_pointer (&self->overlay_initrds_id, g_free);

  if (!overlay_initrds || g_strv_length (overlay_initrds) == 0)
    return;

  /* Generate a unique ID representing this combination of overlay initrds. This is so that
   * ostree_sysroot_write_deployments_with_options() can easily compare initrds when
   * comparing deployments for whether a bootswap is necessary. We could be fancier here but
   * meh... this works. */
  g_autoptr(GString) id = g_string_new (NULL);
  for (char **it = overlay_initrds; it && *it; it++)
    g_string_append (id, *it);

  self->overlay_initrds = g_strdupv (overlay_initrds);
  self->overlay_initrds_id = g_string_free (g_steal_pointer (&id), FALSE);
}

char**
_ostree_deployment_get_overlay_initrds (OstreeDeployment *self)
{
  return self->overlay_initrds;
}

/**
 * ostree_deployment_clone:
 * @self: Deployment
 *
 * Returns: (not nullable) (transfer full): New deep copy of @self
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

  _ostree_deployment_set_overlay_initrds (ret, self->overlay_initrds);

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

/**
 * ostree_deployment_hash:
 * @v: (type OstreeDeployment): Deployment
 *
 * Returns: An integer suitable for use in a `GHashTable`
 */
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
  g_strfreev (self->overlay_initrds);
  g_free (self->overlay_initrds_id);

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

/**
 * ostree_deployment_new:
 * @index: Global index into the bootloader entries
 * @osname: "stateroot" for this deployment
 * @csum: OSTree commit that will be deployed
 * @deployserial: Unique counter
 * @bootcsum: (nullable): Kernel/initrd checksum
 * @bootserial: Unique index
 *
 * Returns: (transfer full) (not nullable): New deployment
 */
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
  g_assert (osname != NULL);
  g_assert (csum != NULL);
  g_assert (deployserial >= 0);
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
 * Returns: (not nullable) (transfer full): Path to deployment root directory, relative to sysroot
 */
char *
ostree_deployment_get_origin_relpath (OstreeDeployment *self)
{
  return g_strdup_printf ("ostree/deploy/%s/deploy/%s.%d.origin",
                          ostree_deployment_get_osname (self),
                          ostree_deployment_get_csum (self),
                          ostree_deployment_get_deployserial (self));
}

/**
 * ostree_deployment_unlocked_state_to_string:
 *
 * Returns: (not nullable): Description of state
 * Since: 2016.4
 */
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
    case OSTREE_DEPLOYMENT_UNLOCKED_TRANSIENT:
      return "transient";
    }
  g_assert_not_reached ();
}

/**
 * ostree_deployment_get_unlocked:
 *
 * Since: 2016.4
 */
OstreeDeploymentUnlockedState
ostree_deployment_get_unlocked (OstreeDeployment *self)
{
  return self->unlocked;
}

/**
 * ostree_deployment_is_pinned:
 * @self: Deployment
 *
 * See ostree_sysroot_deployment_set_pinned().
 *
 * Returns: `TRUE` if deployment will not be subject to GC
 * Since: 2018.3
 */
gboolean
ostree_deployment_is_pinned (OstreeDeployment *self)
{
  if (!self->origin)
    return FALSE;
  return g_key_file_get_boolean (self->origin, OSTREE_ORIGIN_TRANSIENT_GROUP, "pinned", NULL);
}

/**
 * ostree_deployment_is_staged:
 * @self: Deployment
 *
 * Returns: `TRUE` if deployment should be "finalized" at shutdown time
 * Since: 2018.3
 */
gboolean
ostree_deployment_is_staged (OstreeDeployment *self)
{
  return self->staged;
}
