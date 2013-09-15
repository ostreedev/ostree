/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-admin-functions.h"
#include "otutil.h"
#include "ostree.h"
#include "libgsystem.h"

OtOrderedHash *
ot_admin_parse_kernel_args (const char *options)
{
  OtOrderedHash *ret;
  char **args;
  char **iter;

  ret = ot_ordered_hash_new ();

  if (!options)
    return ret;
  
  args = g_strsplit (options, " ", -1);
  for (iter = args; *iter; iter++)
    {
      char *arg = *iter;
      char *val;
      
      val = ot_admin_util_split_keyeq (arg);

      g_ptr_array_add (ret->order, arg);
      g_hash_table_insert (ret->table, arg, val);
    }

  return ret;
}

char *
ot_admin_kernel_arg_string_serialize (OtOrderedHash *ohash)
{
  guint i;
  GString *buf = g_string_new ("");
  gboolean first = TRUE;

  for (i = 0; i < ohash->order->len; i++)
    {
      const char *key = ohash->order->pdata[i];
      const char *val = g_hash_table_lookup (ohash->table, key);

      g_assert (val != NULL);

      if (first)
        first = FALSE;
      else
        g_string_append_c (buf, ' ');

      if (*val)
        g_string_append_printf (buf, "%s=%s", key, val);
      else
        g_string_append (buf, key);
    }

  return g_string_free (buf, FALSE);
}

gboolean
ot_admin_check_os (GFile         *sysroot, 
                   const char    *osname,
                   GCancellable  *cancellable,
                   GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *osdir = NULL;

  osdir = ot_gfile_resolve_path_printf (sysroot, "ostree/deploy/%s/var", osname);
  if (!g_file_query_exists (osdir, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No such OS '%s', use os-init to create it", osname);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
parse_kernel_commandline (OtOrderedHash  **out_args,
                          GCancellable    *cancellable,
                          GError         **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *proc_cmdline = g_file_new_for_path ("/proc/cmdline");
  gs_free char *contents = NULL;
  gsize len;

  if (!g_file_load_contents (proc_cmdline, cancellable, &contents, &len, NULL,
                             error))
    goto out;

  ret = TRUE;
  *out_args = ot_admin_parse_kernel_args (contents);;
 out:
  return ret;
}

/**
 * ot_admin_find_booted_deployment:
 * @target_sysroot: Root directory
 * @deployments: (element-type OstreeDeployment): Loaded deployments
 * @out_deployment: (out): The currently booted deployment
 * @cancellable:
 * @error: 
 * 
 * If the system is currently booted into a deployment in
 * @deployments, set @out_deployment.  Note that if @target_sysroot is
 * not equal to "/", @out_deployment will always be set to %NULL.
 */
gboolean
ot_admin_find_booted_deployment (GFile               *target_sysroot,
                                 GPtrArray           *deployments,
                                 OstreeDeployment       **out_deployment,
                                 GCancellable        *cancellable,
                                 GError             **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *active_root = g_file_new_for_path ("/");
  gs_unref_object OstreeSysroot *active_deployment_root = ostree_sysroot_new_default ();
  gs_unref_object OstreeDeployment *ret_deployment = NULL;

  if (g_file_equal (active_root, target_sysroot))
    { 
      guint i;
      const char *bootlink_arg;
      __attribute__((cleanup(ot_ordered_hash_cleanup))) OtOrderedHash *kernel_args = NULL;
      guint32 root_device;
      guint64 root_inode;
      
      if (!ot_admin_util_get_devino (active_root, &root_device, &root_inode,
                                     cancellable, error))
        goto out;

      if (!parse_kernel_commandline (&kernel_args, cancellable, error))
        goto out;
      
      bootlink_arg = g_hash_table_lookup (kernel_args->table, "ostree");
      if (bootlink_arg)
        {
          for (i = 0; i < deployments->len; i++)
            {
              OstreeDeployment *deployment = deployments->pdata[i];
              gs_unref_object GFile *deployment_path = ostree_sysroot_get_deployment_directory (active_deployment_root, deployment);
              guint32 device;
              guint64 inode;

              if (!ot_admin_util_get_devino (deployment_path, &device, &inode,
                                             cancellable, error))
                goto out;

              if (device == root_device && inode == root_inode)
                {
                  ret_deployment = g_object_ref (deployment);
                  break;
                }
            }
          if (ret_deployment == NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unexpected state: ostree= kernel argument found, but / is not a deployment root");
              goto out;
            }
        }
      else
        {
          /* Not an ostree system */
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_deployment, &ret_deployment);
 out:
  return ret;
}

gboolean
ot_admin_require_deployment_or_osname (GFile               *sysroot,
                                       GPtrArray           *deployments,
                                       const char          *osname,
                                       OstreeDeployment       **out_deployment,
                                       GCancellable        *cancellable,
                                       GError             **error)
{
  gboolean ret = FALSE;
  gs_unref_object OstreeDeployment *ret_deployment = NULL;

  if (!ot_admin_find_booted_deployment (sysroot, deployments, &ret_deployment,
                                        cancellable, error))
    goto out;

  if (ret_deployment == NULL && osname == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Not currently booted into an OSTree system and no --os= argument given");
      goto out;
    }
  
  ret = TRUE;
  ot_transfer_out_value (out_deployment, &ret_deployment);
 out:
  return ret;
}

OstreeDeployment *
ot_admin_get_merge_deployment (GPtrArray         *deployments,
                               const char        *osname,
                               OstreeDeployment      *booted_deployment)
{
  g_return_val_if_fail (osname != NULL || booted_deployment != NULL, NULL);

  if (osname == NULL)
    osname = ostree_deployment_get_osname (booted_deployment);

  if (booted_deployment &&
      g_strcmp0 (ostree_deployment_get_osname (booted_deployment), osname) == 0)
    {
      return g_object_ref (booted_deployment);
    }
  else
    {
      guint i;
      for (i = 0; i < deployments->len; i++)
        {
          OstreeDeployment *deployment = deployments->pdata[i];

          if (strcmp (ostree_deployment_get_osname (deployment), osname) != 0)
            continue;
          
          return g_object_ref (deployment);
        }
    }
  return NULL;
}

GKeyFile *
ot_origin_new_from_refspec (const char *refspec)
{
  GKeyFile *ret = g_key_file_new ();
  g_key_file_set_string (ret, "origin", "refspec", refspec);
  return ret;
}
