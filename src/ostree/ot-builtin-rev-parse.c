/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-rev-parse.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ostree_builtin_rev_parse (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  gboolean ret = FALSE;
  const char *rev = "master";
  int i;
  g_autofree char *resolved_rev = NULL;

  context = g_option_context_new ("REV");

  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "REV must be specified", error);
      goto out;
    }
  for (i = 1; i < argc; i++)
    {
      rev = argv[i];
      g_free (resolved_rev);
      resolved_rev = NULL;
      if (!ostree_repo_resolve_rev (repo, rev, FALSE, &resolved_rev, error))
        goto out;
      g_print ("%s\n", resolved_rev);
    }
 
  ret = TRUE;
 out:
  return ret;
}
