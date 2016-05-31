/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include <string.h>
#include <glib-unix.h>

#include "ot-main.h"
#include "ot-admin-instutil-builtins.h"

#include "otutil.h"

static char *
ptrarray_path_join (GPtrArray  *path)
{
  GString *path_buf;

  path_buf = g_string_new ("");

  if (path->len == 0)
    g_string_append_c (path_buf, '/');
  else
    {
      guint i;
      for (i = 0; i < path->len; i++)
        {
          const char *elt = path->pdata[i];

          g_string_append_c (path_buf, '/');
          g_string_append (path_buf, elt);
        }
    }

  return g_string_free (path_buf, FALSE);
}

static gboolean
relabel_one_path (OstreeSePolicy *sepolicy,
                  GFile          *path,
                  GFileInfo      *info,
                  GPtrArray      *path_parts,
                  GCancellable   *cancellable,
                  GError        **error)
{
  gboolean ret = FALSE;
  g_autofree char *relpath = NULL;
  g_autofree char *new_label = NULL;

  relpath = ptrarray_path_join (path_parts);
  if (!ostree_sepolicy_restorecon (sepolicy, relpath,
                                   info, path,
                                   OSTREE_SEPOLICY_RESTORECON_FLAGS_ALLOW_NOLABEL |
                                   OSTREE_SEPOLICY_RESTORECON_FLAGS_KEEP_EXISTING,
                                   &new_label,
                                   cancellable, error))
    {
      g_prefix_error (error, "Setting context of %s: ", gs_file_get_path_cached (path));
      goto out;
    }

  if (new_label)
    g_print ("Set label of '%s' (as '%s') to '%s'\n",
             gs_file_get_path_cached (path),
             relpath,
             new_label);

  ret = TRUE;
 out:
  return ret;
}

static gboolean
relabel_recursively (OstreeSePolicy *sepolicy,
                     GFile          *dir,
                     GFileInfo      *dir_info,
                     GPtrArray      *path_parts,
                     GCancellable   *cancellable,
                     GError        **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFileEnumerator) direnum = NULL;

  if (!relabel_one_path (sepolicy, dir, dir_info, path_parts,
                         cancellable, error))
    goto out;

  direnum = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO,
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       cancellable, error);
  if (!direnum)
    goto out;
  
  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *child;
      GFileType ftype;

      if (!gs_file_enumerator_iterate (direnum, &file_info, &child,
                                       cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      g_ptr_array_add (path_parts, (char*)g_file_info_get_name (file_info));

      ftype = g_file_info_get_file_type (file_info);
      if (ftype == G_FILE_TYPE_DIRECTORY)
        {
          if (!relabel_recursively (sepolicy, child, file_info, path_parts,
                                    cancellable, error))
            goto out;
        }
      else
        {
          if (!relabel_one_path (sepolicy, child, file_info, path_parts,
                                 cancellable, error))
            goto out;
        }

      g_ptr_array_remove_index (path_parts, path_parts->len - 1);
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
selinux_relabel_dir (OstreeSePolicy                *sepolicy,
                     GFile                         *dir,
                     const char                    *prefix,
                     GCancellable                  *cancellable,
                     GError                       **error)
{
  gboolean ret = FALSE;
  g_autoptr(GPtrArray) path_parts = g_ptr_array_new ();
  g_autoptr(GFileInfo) root_info = NULL;

  root_info = g_file_query_info (dir, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!root_info)
    goto out;
  
  g_ptr_array_add (path_parts, (char*)prefix);
  if (!relabel_recursively (sepolicy, dir, root_info, path_parts,
                            cancellable, error))
    {
      g_prefix_error (error, "Relabeling /%s: ", prefix);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ot_admin_instutil_builtin_selinux_ensure_labeled (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  const char *policy_name;
  g_autoptr(GFile) subpath = NULL;
  const char *prefix = NULL;
  glnx_unref_object OstreeSePolicy *sepolicy = NULL;
  g_autoptr(GPtrArray) deployments = NULL;
  OstreeDeployment *first_deployment;
  GOptionContext *context = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;
  g_autoptr(GFile) deployment_path = NULL;

  context = g_option_context_new ("[SUBPATH PREFIX] - relabel all or part of a deployment");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER | OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED,
                                          &sysroot, cancellable, error))
    goto out;

  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  deployments = ostree_sysroot_get_deployments (sysroot);
  if (deployments->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unable to find a deployment in sysroot");
      goto out;
    }
  first_deployment = deployments->pdata[0];
  deployment_path = ostree_sysroot_get_deployment_directory (sysroot, first_deployment);

  if (argc >= 2)
    {
      subpath = g_file_new_for_path (argv[1]);
      prefix = argv[2];
    }
  else
    {
      subpath = g_object_ref (deployment_path);
      prefix = "";
    }

  sepolicy = ostree_sepolicy_new (deployment_path, cancellable, error);
  if (!sepolicy)
    goto out;
  
  policy_name = ostree_sepolicy_get_name (sepolicy);
  if (policy_name)
    {
      g_print ("Relabeling using policy '%s'\n", policy_name);
      if (!selinux_relabel_dir (sepolicy, subpath, prefix,
                                cancellable, error))
        goto out;
    }
  else
    g_print ("No SELinux policy found in deployment '%s'\n",
             gs_file_get_path_cached (deployment_path));

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
