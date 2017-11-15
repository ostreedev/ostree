/*
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
ot_admin_builtin_diff (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("");
  g_option_context_add_main_entries (context, options, NULL);

  g_autoptr(OstreeSysroot) sysroot = NULL;
  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER | OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED,
                                          invocation, &sysroot, cancellable, error))
    return FALSE;

  if (!ot_admin_require_booted_deployment_or_osname (sysroot, opt_osname,
                                                     cancellable, error))
    return FALSE;

  g_autoptr(OstreeDeployment) deployment = NULL;
  if (opt_osname != NULL)
    {
      deployment = ostree_sysroot_get_merge_deployment (sysroot, opt_osname);
      if (deployment == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "No deployment for OS '%s'", opt_osname);
          return FALSE;
        }
    }
  else
    deployment = g_object_ref (ostree_sysroot_get_booted_deployment (sysroot));

  g_autoptr(GFile) deployment_dir = ostree_sysroot_get_deployment_directory (sysroot, deployment);
  g_autoptr(GFile) orig_etc_path = g_file_resolve_relative_path (deployment_dir, "usr/etc");
  g_autoptr(GFile) new_etc_path = g_file_resolve_relative_path (deployment_dir, "etc");
  g_autoptr(GPtrArray) modified = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_diff_item_unref);
  g_autoptr(GPtrArray) removed = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  g_autoptr(GPtrArray) added = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  if (!ostree_diff_dirs (OSTREE_DIFF_FLAGS_IGNORE_XATTRS,
                         orig_etc_path, new_etc_path, modified, removed, added,
                         cancellable, error))
    return FALSE;

  ostree_diff_print (orig_etc_path, new_etc_path, modified, removed, added);

  return TRUE;
}
