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

#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"

#include <glib/gi18n.h>

static char *opt_osname;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-admin-config-diff.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "os", 0, 0, G_OPTION_ARG_STRING, &opt_osname, "Use a different operating system root than the current one", "OSNAME" },
  { NULL }
};

gboolean
ot_admin_builtin_diff (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeSysroot) sysroot = NULL;
  gboolean ret = FALSE;
  g_autoptr(OstreeDeployment) deployment = NULL;
  g_autoptr(GFile) deployment_dir = NULL;
  g_autoptr(GPtrArray) modified = NULL;
  g_autoptr(GPtrArray) removed = NULL;
  g_autoptr(GPtrArray) added = NULL;
  g_autoptr(GFile) orig_etc_path = NULL;
  g_autoptr(GFile) new_etc_path = NULL;

  context = g_option_context_new ("Diff current /etc configuration versus default");

  g_option_context_add_main_entries (context, options, NULL);

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER | OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED,
                                          &sysroot, cancellable, error))
    goto out;
  
  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  if (!ot_admin_require_booted_deployment_or_osname (sysroot, opt_osname,
                                                     cancellable, error))
    goto out;
  if (opt_osname != NULL)
    {
      deployment = ostree_sysroot_get_merge_deployment (sysroot, opt_osname);
      if (deployment == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "No deployment for OS '%s'", opt_osname);
          goto out;
        }
    }
  else
    deployment = g_object_ref (ostree_sysroot_get_booted_deployment (sysroot));

  deployment_dir = ostree_sysroot_get_deployment_directory (sysroot, deployment);

  orig_etc_path = g_file_resolve_relative_path (deployment_dir, "usr/etc");
  new_etc_path = g_file_resolve_relative_path (deployment_dir, "etc");
  
  modified = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_diff_item_unref);
  removed = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  added = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  if (!ostree_diff_dirs (OSTREE_DIFF_FLAGS_IGNORE_XATTRS,
                         orig_etc_path, new_etc_path, modified, removed, added,
                         cancellable, error))
    goto out;

  ostree_diff_print (orig_etc_path, new_etc_path, modified, removed, added);

  ret = TRUE;
 out:
  return ret;
}
