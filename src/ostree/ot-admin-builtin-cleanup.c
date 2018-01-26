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
#include "ostree.h"

#include <glib/gi18n.h>

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-admin-cleanup.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ot_admin_builtin_cleanup (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeSysroot) sysroot = NULL;
  gboolean ret = FALSE;

  context = g_option_context_new ("");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER,
                                          invocation, &sysroot, cancellable, error))
    goto out;

  if (!ostree_sysroot_cleanup (sysroot, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
