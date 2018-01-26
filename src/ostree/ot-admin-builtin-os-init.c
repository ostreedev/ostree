/*
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "otutil.h"

#include <glib/gi18n.h>

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-admin-os-init.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ot_admin_builtin_os_init (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeSysroot) sysroot = NULL;
  gboolean ret = FALSE;
  const char *osname = NULL;

  context = g_option_context_new ("OSNAME");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER | OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED,
                                          invocation, &sysroot, cancellable, error))
    goto out;

  if (!ostree_sysroot_ensure_initialized (sysroot, cancellable, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "OSNAME must be specified", error);
      goto out;
    }

  osname = argv[1];

  if (!ostree_sysroot_init_osname (sysroot, osname, cancellable, error))
    goto out;

  g_print ("ostree/deploy/%s initialized as OSTree root\n", osname);

  ret = TRUE;
 out:
  return ret;
}
