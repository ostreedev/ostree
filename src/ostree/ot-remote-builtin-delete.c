/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
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

#include "otutil.h"

#include "ot-main.h"
#include "ot-remote-builtins.h"

static gboolean opt_if_exists = FALSE;
static char *opt_sysroot;
static char *opt_repo;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-remote.xml) when changing the option list.
 */

static GOptionEntry option_entries[] = {
  { "if-exists", 0, 0, G_OPTION_ARG_NONE, &opt_if_exists, "Do nothing if the provided remote does not exist", NULL },
  { "repo", 0, 0, G_OPTION_ARG_FILENAME, &opt_repo, "Path to OSTree repository (defaults to /sysroot/ostree/repo)", "PATH" },
  { "sysroot", 0, 0, G_OPTION_ARG_FILENAME, &opt_sysroot, "Use sysroot at PATH (overrides --repo)", "PATH" },
  { NULL }
};

gboolean
ot_remote_builtin_delete (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{

  g_autoptr(GOptionContext) context = g_option_context_new ("NAME");

  if (!ostree_option_context_parse (context, option_entries, &argc, &argv,
                                    invocation, NULL, cancellable, error))
    return FALSE;

  g_autoptr(OstreeSysroot) sysroot = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  if (!ostree_parse_sysroot_or_repo_option (context, opt_sysroot, opt_repo,
                                            &sysroot, &repo,
                                            cancellable, error))
    return FALSE;

  if (argc < 2)
    {
      ot_util_usage_error (context, "NAME must be specified", error);
      return FALSE;
    }

  const char *remote_name = argv[1];

  if (!ostree_repo_remote_change (repo, NULL,
                                  opt_if_exists ? OSTREE_REPO_REMOTE_CHANGE_DELETE_IF_EXISTS :
                                  OSTREE_REPO_REMOTE_CHANGE_DELETE,
                                  remote_name, NULL, NULL,
                                  cancellable, error))
    return FALSE;

  return TRUE;
}
