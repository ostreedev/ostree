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
#include <stdlib.h>
#include "otutil.h"

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
  gs_unref_object OstreeBootconfigParser *new_bootconfig = NULL;
  GKeyFile *new_origin = NULL;
  OstreeDeployment *ret = ostree_deployment_new (self->index, self->osname, self->csum,
                                                 self->deployserial,
                                                 self->bootcsum, self->bootserial);

  new_bootconfig = ostree_bootconfig_parser_clone (self->bootconfig);
  ostree_deployment_set_bootconfig (ret, new_bootconfig);

  if (self->origin)
    {
      gs_free char *data = NULL;
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

static gboolean
get_custom_name_keyfile (GFile        *path_to_customs,
                         GKeyFile    **out_keyfile,
                         GCancellable *cancellable,
                         GError      **error)
{
  gboolean ret = FALSE;
  GKeyFile *ret_keyfile = NULL;

  if (!ot_keyfile_load_from_file_if_exists (g_file_get_path (path_to_customs), G_KEY_FILE_NONE, &ret_keyfile, error))
    goto out;

  /* keyfile might be null if it doesn't exist in file */
  if (!ret_keyfile)
    {
      if (!g_file_replace_contents (path_to_customs, "", 0, NULL, FALSE, 
                                 G_FILE_CREATE_REPLACE_DESTINATION, NULL, cancellable, error))
        goto out;

      ret_keyfile = g_key_file_new();

      if (!g_key_file_load_from_file (ret_keyfile, g_file_get_path (path_to_customs), G_KEY_FILE_NONE, error))
        goto out;
    }

  ret = TRUE;
  ot_transfer_out_value (out_keyfile, &ret_keyfile);
 out:
  if (ret_keyfile)
    g_key_file_free (ret_keyfile);
  return ret;
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

gboolean
ostree_deployment_get_name (char             *checksum,
                            GFile            *path_to_customs,
                            char            **out_name,
                            GError          **error)
{
  gboolean ret = FALSE;
  gs_unref_ptrarray GPtrArray *colors = g_ptr_array_new ();
  gs_unref_ptrarray GPtrArray *hats = g_ptr_array_new ();
  char *color = NULL;
  char *hat = NULL;
  gs_free char *name = NULL;
  gs_free char *dup = g_strdup (&(checksum[15]));
  gs_free char *csum1 = g_strndup(checksum, 15);
  gs_free char *csum2 = g_strndup (dup, 15);
  gs_free char *default_name = NULL;
  GKeyFile *keyfile = NULL;
  /* converts first 30 bytes of checksum into two decimal numbers to get an array index */
  unsigned long int color_number = strtol (csum1, NULL, 16);
  unsigned long int hat_number = strtol (csum2, NULL, 16);

  if (!ot_keyfile_load_from_file_if_exists (g_file_get_path (path_to_customs), G_KEY_FILE_NONE, &keyfile, error))
    goto out;

  ot_ptrarray_add_many (colors, "red", "orange", "yellow", "green", "blue", "purple",
                  "indigo", "pink", "teal", "magenta", "cyan", "black", 
                  "brown", "white", "tangerine", "beige", "gray", "maroon", 
                  "gold", "silver", "amber", "auburn", "azure", "celadon", 
                  "coral", "puce", "crimson", "vermillion", "scarlet", "peach", 
                  "salmon", "olive", "mint", "violet", "cerise", "ivory",
                  "jade", "navy", "orchid", "taupe", "chartreuse", "cerise", 
                  "copper", "fuchsia", "mauve", "periwinkle", "sepia", "khaki", 
                  "plum",  NULL);

  ot_ptrarray_add_many (hats, "fedora", "cap", "beanie", "beret", "bowler", 
                  "boater", "deerstalker", "fez", "helmet", "bonnet", "hood", 
                  "bandanna", "visor", "stetson", "tricorne", "chullo", "bicorne", 
                  "busby", "laplander", "sombrero", "chupalla", "turban", "trilby", NULL);

  color = g_ptr_array_index (colors, color_number % colors->len);
  hat = g_ptr_array_index (hats, hat_number % hats->len);  

  default_name = g_strdup_printf ("%s_%s", color, hat);

  if (!ot_keyfile_get_value_with_default (keyfile, "custom_names", checksum,
                                          default_name, &name, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_name, &name);
 out:
  if (keyfile)
    g_key_file_free (keyfile);
  return ret;
}

gboolean
ostree_deployment_set_custom_name (char          *checksum,
                                   char          *custom_name,    
                                   GFile         *path_to_customs,
                                   GCancellable  *cancellable,
                                   GError       **error)
{
  gboolean ret = FALSE;
  GKeyFile *keyfile = NULL;
  gs_free char *data = NULL;
  gsize len, keys_len;
  guint i;
  gs_free gchar **keys_array = NULL;
  gboolean unique_name = TRUE;
  gs_free char *key = NULL;
  gs_free char *val = NULL;

  if (!get_custom_name_keyfile (path_to_customs, &keyfile, cancellable, error))
    goto out;

  /* now assuming we have a legitimate key file  */
  data = g_key_file_to_data (keyfile, &len, error);

  /* uniqueness check */
  keys_array = g_key_file_get_keys (keyfile, "custom_names", &keys_len, error);
  for (i=0; i < keys_len; i++)
    {
      key = keys_array[i];
      val = g_key_file_get_value (keyfile, "custom_names", key, error);
      if (g_strcmp0 (val, custom_name) == 0)
        {
          unique_name = FALSE;
          break;
        }
    }

  if (unique_name)
    {
      g_key_file_set_string (keyfile, "custom_names", checksum, custom_name);
      data = g_key_file_to_data (keyfile, &len, error);
      
      if (!g_file_replace_contents (path_to_customs, data, len, NULL, FALSE, 
                                     G_FILE_CREATE_REPLACE_DESTINATION, NULL, cancellable, error))
        goto out;
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "%s already assigned as a custom name to %s, please rename the conflict or pick a unique name\n", custom_name, key);
      goto out;
    }

  ret = TRUE;
 out:
  if (keyfile)
    g_key_file_free (keyfile);
  return ret;
}

gboolean
ostree_deployment_rm_custom_name (char          *checksum,
                                  GFile         *path_to_customs,
                                  GCancellable  *cancellable,
                                  GError       **error)
{
  gboolean ret = FALSE;
  GKeyFile *keyfile = NULL;
  char *data = NULL;
  gsize len;

  if (!get_custom_name_keyfile (path_to_customs, &keyfile, cancellable, error))
    goto out;

  if (!g_key_file_remove_key (keyfile, "custom_names", checksum, error))
    goto out;

  data = g_key_file_to_data (keyfile, &len, error);
      
  if (!g_file_replace_contents (path_to_customs, data, len, NULL, FALSE, 
                                 G_FILE_CREATE_REPLACE_DESTINATION, NULL, cancellable, error))
    goto out;


  ret = TRUE;
 out:
  g_key_file_free (keyfile);
  return ret;
}