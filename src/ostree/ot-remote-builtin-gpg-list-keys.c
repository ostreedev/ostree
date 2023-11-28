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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "otutil.h"

#include "ot-dump.h"
#include "ot-remote-builtins.h"

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-remote.xml) when changing the option list.
 */

static GOptionEntry option_entries[] = { { NULL } };

gboolean
ot_remote_builtin_list_gpg_keys (int argc, char **argv, OstreeCommandInvocation *invocation,
                                 GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("NAME");
  g_autoptr (OstreeRepo) repo = NULL;
  if (!ostree_option_context_parse (context, option_entries, &argc, &argv, invocation, &repo,
                                    cancellable, error))
    return FALSE;

  const char *remote_name = (argc > 1) ? argv[1] : NULL;

  g_autoptr (GPtrArray) keys = NULL;
  if (!ostree_repo_remote_get_gpg_keys (repo, remote_name, NULL, &keys, cancellable, error))
    return FALSE;

  for (guint i = 0; i < keys->len; i++)
    {
      if (!ot_dump_gpg_key (keys->pdata[i], error))
        return FALSE;
    }

  return TRUE;
}
