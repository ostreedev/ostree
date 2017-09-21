/*
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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

#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"
#include "otutil.h"

#include "../libostree/ostree-kernel-args.h"

#include <glib/gi18n.h>
#include <err.h>

static gboolean opt_hotfix;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-admin-unlock.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { "hotfix", 0, 0, G_OPTION_ARG_NONE, &opt_hotfix, "Retain changes across reboots", NULL },
  { NULL }
};

gboolean
ot_admin_builtin_unlock (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeSysroot) sysroot = NULL;
  OstreeDeployment *booted_deployment = NULL;
  OstreeDeploymentUnlockedState target_state;

  context = g_option_context_new ("Make the current deployment mutable (as a hotfix or development)");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER,
                                          &sysroot, cancellable, error))
    goto out;
  
  if (argc > 1)
    {
      ot_util_usage_error (context, "This command takes no extra arguments", error);
      goto out;
    }

  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
  if (!booted_deployment)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Not currently booted into an OSTree system");
      goto out;
    }

  target_state = opt_hotfix ? OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX : OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT;

  if (!ostree_sysroot_deployment_unlock (sysroot, booted_deployment,
                                         target_state, cancellable, error))
    goto out;
  
  switch (target_state)
    {
    case OSTREE_DEPLOYMENT_UNLOCKED_NONE:
      g_assert_not_reached ();
      break;
    case OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX:
      g_print ("Hotfix mode enabled.  A writable overlayfs is now mounted on /usr\n"
               "for this booted deployment.  A non-hotfixed clone has been created\n"
               "as the non-default rollback target.\n");
      break;
    case OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT:
      g_print ("Development mode enabled.  A writable overlayfs is now mounted on /usr.\n"
               "All changes there will be discarded on reboot.\n");
      break;
    }

  ret = TRUE;
 out:
  return ret;
}
