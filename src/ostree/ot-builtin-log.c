/*
 * Copyright (C) 2013 Stef Walter <stefw@redhat.com>
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
 * Author: Stef Walter <stefw@redhat.com>
 */

#include "config.h"

#include "ostree.h"
#include "ot-builtins.h"
#include "ot-dump.h"
#include "ot-main.h"
#include "otutil.h"

static gboolean opt_raw;

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-log.xml) when changing the option list.
 */

static GOptionEntry options[]
    = { { "raw", 0, 0, G_OPTION_ARG_NONE, &opt_raw, "Show raw variant data" }, { NULL } };

static gboolean
log_commit (OstreeRepo *repo, const gchar *checksum, gboolean is_recurse, OstreeDumpFlags flags,
            GError **error)
{
  GError *local_error = NULL;

  g_autoptr (GVariant) variant = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum, &variant, &local_error))
    {
      if (is_recurse && g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_print ("<< History beyond this commit not fetched >>\n");
          g_clear_error (&local_error);
          return TRUE;
        }
      else
        {
          g_propagate_error (error, local_error);
          return FALSE;
        }
    }

  ot_dump_object (OSTREE_OBJECT_TYPE_COMMIT, checksum, variant, flags);

  /* Get the parent of this commit */
  g_autofree char *parent = ostree_commit_get_parent (variant);
  if (parent && !log_commit (repo, parent, TRUE, flags, error))
    return FALSE;

  return TRUE;
}

gboolean
ostree_builtin_log (int argc, char **argv, OstreeCommandInvocation *invocation,
                    GCancellable *cancellable, GError **error)
{

  g_autoptr (GOptionContext) context = g_option_context_new ("REV");

  g_autoptr (OstreeRepo) repo = NULL;
  if (!ostree_option_context_parse (context, options, &argc, &argv, invocation, &repo, cancellable,
                                    error))
    return FALSE;

  if (argc <= 1)
    {
      ot_util_usage_error (context, "A rev argument is required", error);
      return FALSE;
    }
  const char *rev = argv[1];
  OstreeDumpFlags flags = OSTREE_DUMP_NONE;
  if (opt_raw)
    flags |= OSTREE_DUMP_RAW;

  g_autofree char *checksum = NULL;
  if (!ostree_repo_resolve_rev (repo, rev, FALSE, &checksum, error))
    return FALSE;

  if (!log_commit (repo, checksum, FALSE, flags, error))
    return FALSE;

  return TRUE;
}
