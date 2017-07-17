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
#include <sys/stat.h>

#include "ot-main.h"
#include "ot-remote-builtins.h"
#include "ostree-repo-private.h"
#include "ot-remote-cookie-util.h"

static GOptionEntry option_entries[] = {
  { NULL }
};

gboolean
ot_remote_builtin_delete_cookie (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(OstreeRepo) repo = NULL;
  g_autoptr(GOptionContext) context = g_option_context_new ("NAME DOMAIN PATH COOKIE_NAME- Remote one cookie from remote");

  if (!ostree_option_context_parse (context, option_entries, &argc, &argv,
                                    OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    return FALSE;

  if (argc < 5)
    {
      ot_util_usage_error (context, "NAME, DOMAIN, PATH and COOKIE_NAME must be specified", error);
      return FALSE;
    }

  const char *remote_name = argv[1];
  const char *domain = argv[2];
  const char *path = argv[3];
  const char *cookie_name = argv[4];
  g_autofree char *cookie_file = g_strdup_printf ("%s.cookies.txt", remote_name);
  if (!ot_delete_cookie_at (ostree_repo_get_dfd (repo), cookie_file, domain, path, cookie_name, error))
    return FALSE;

  return TRUE;
}
