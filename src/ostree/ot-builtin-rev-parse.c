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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ostree.h"
#include "ot-builtins.h"
#include "otutil.h"
#include <stdbool.h>

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-rev-parse.xml) when changing the option list.
 */

static gboolean opt_single;

static GOptionEntry options[] = { { "single", 'S', 0, G_OPTION_ARG_NONE, &opt_single,
                                    "If the repository has exactly one commit, then print it; any "
                                    "other case will result in an error",
                                    NULL },
                                  { NULL } };

gboolean
ostree_builtin_rev_parse (int argc, char **argv, OstreeCommandInvocation *invocation,
                          GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("REV");
  g_autoptr (OstreeRepo) repo = NULL;
  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable,
                                    error))
    return FALSE;

  if (opt_single)
    {
      if (argc >= 2)
        {
          ot_util_usage_error (context, "Cannot specify arguments with --single", error);
          return FALSE;
        }

      g_autoptr (GHashTable) objects = NULL;
      if (!ostree_repo_list_commit_objects_starting_with (repo, "", &objects, cancellable, error))
        return FALSE;

      GVariant *found = NULL;
      GLNX_HASH_TABLE_FOREACH (objects, GVariant *, key)
        {
          if (found)
            return glnx_throw (error, "Multiple commit objects found");
          found = key;
        }
      if (!found)
        return glnx_throw (error, "No commit objects found");
      const char *checksum;
      OstreeObjectType objtype;
      ostree_object_name_deserialize (found, &checksum, &objtype);
      g_assert (objtype == OSTREE_OBJECT_TYPE_COMMIT);

      g_print ("%s\n", checksum);

      return TRUE; /* Note early return */
    }

  if (argc < 2)
    {
      ot_util_usage_error (context, "REV must be specified", error);
      return FALSE;
    }

  for (gint i = 1; i < argc; i++)
    {
      const char *rev = argv[i];
      g_autofree char *resolved_rev = NULL;
      if (!ostree_repo_resolve_rev (repo, rev, FALSE, &resolved_rev, error))
        return FALSE;
      g_print ("%s\n", resolved_rev);
    }

  return TRUE;
}
