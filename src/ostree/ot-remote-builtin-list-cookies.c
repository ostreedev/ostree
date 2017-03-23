/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
 * Copyright (C) 2016 Sjoerd Simons <sjoerd@luon.net>
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
#include "ostree-repo-private.h"
#include "ot-remote-cookie-util.h"

static GOptionEntry option_entries[] = {
  { NULL }
};

gboolean
ot_remote_builtin_list_cookies (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *remote_name;
  g_autofree char *jar_path = NULL;
  g_autofree char *cookie_file = NULL;

  context = g_option_context_new ("NAME - Show remote repository cookies");

  if (!ostree_option_context_parse (context, option_entries, &argc, &argv,
                                    OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    return FALSE;

  if (argc < 2)
    {
      ot_util_usage_error (context, "NAME must be specified", error);
      return FALSE;
    }

  remote_name = argv[1];

  cookie_file = g_strdup_printf ("%s.cookies.txt", remote_name);
  jar_path = g_build_filename (g_file_get_path (repo->repodir), cookie_file, NULL);

  if (!ot_list_cookies_at (AT_FDCWD, jar_path, error))
    return FALSE;

  return TRUE;
}
