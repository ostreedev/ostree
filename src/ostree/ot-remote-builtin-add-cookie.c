/*
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

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-remote.xml) when changing the option list.
 */

static GOptionEntry option_entries[] = {
  { NULL }
};

gboolean
ot_remote_builtin_add_cookie (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("NAME DOMAIN PATH COOKIE_NAME VALUE");
  g_autoptr(OstreeRepo) repo = NULL;
  if (!ostree_option_context_parse (context, option_entries, &argc, &argv,
                                    invocation, &repo, cancellable, error))
    return FALSE;

  if (argc < 6)
    {
      ot_util_usage_error (context, "NAME, DOMAIN, PATH, COOKIE_NAME and VALUE must be specified", error);
      return FALSE;
    }

  const char *remote_name = argv[1];
  const char *domain = argv[2];
  const char *path = argv[3];
  const char *cookie_name = argv[4];
  const char *value = argv[5];
  g_autofree char *cookie_file = g_strdup_printf ("%s.cookies.txt", remote_name);
  if (!ot_add_cookie_at (ostree_repo_get_dfd (repo), cookie_file, domain, path, cookie_name, value, error))
    return FALSE;

  return TRUE;
}
