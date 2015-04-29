/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
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

#include <libglnx.h>

#include "otutil.h"
#include "ot-tool-util.h"

#include "ot-main.h"
#include "ot-remote-builtins.h"

static char **opt_set;
static gboolean opt_no_gpg_verify;
static gboolean opt_if_not_exists;

static GOptionEntry option_entries[] = {
  { "set", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_set, "Set config option KEY=VALUE for remote", "KEY=VALUE" },
  { "no-gpg-verify", 0, 0, G_OPTION_ARG_NONE, &opt_no_gpg_verify, "Disable GPG verification", NULL },
  { "if-not-exists", 0, 0, G_OPTION_ARG_NONE, &opt_if_not_exists, "Do nothing if the provided remote exists", NULL },
  { NULL }
};

gboolean
ot_remote_builtin_add (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  glnx_unref_object OstreeRepo *repo = NULL;
  const char *remote_name;
  const char *remote_url;
  char **iter;
  g_autofree char *target_name = NULL;
  glnx_unref_object GFile *target_conf = NULL;
  g_autoptr(GVariantBuilder) optbuilder = NULL;
  gboolean ret = FALSE;

  context = g_option_context_new ("NAME URL [BRANCH...] - Add a remote repository");

  if (!ostree_option_context_parse (context, option_entries, &argc, &argv,
                                    OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc < 3)
    {
      ot_util_usage_error (context, "NAME and URL must be specified", error);
      goto out;
    }

  remote_name = argv[1];
  remote_url  = argv[2];

  optbuilder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

  if (argc > 3)
    {
      g_autoptr(GPtrArray) branchesp = g_ptr_array_new ();
      int i;

      for (i = 3; i < argc; i++)
        g_ptr_array_add (branchesp, argv[i]);
      g_ptr_array_add (branchesp, NULL);

      g_variant_builder_add (optbuilder, "{s@v}",
                             "branches",
                             g_variant_new_variant (g_variant_new_strv ((const char*const*)branchesp->pdata, -1)));
    }

  for (iter = opt_set; iter && *iter; iter++)
    {
      const char *keyvalue = *iter;
      g_autofree char *subkey = NULL;
      g_autofree char *subvalue = NULL;

      if (!ot_parse_keyvalue (keyvalue, &subkey, &subvalue, error))
        goto out;

      g_variant_builder_add (optbuilder, "{s@v}",
                             subkey, g_variant_new_variant (g_variant_new_string (subvalue)));
    }

  if (opt_no_gpg_verify)
    g_variant_builder_add (optbuilder, "{s@v}",
                           "gpg-verify",
                           g_variant_new_variant (g_variant_new_boolean (FALSE)));

  if (!ostree_repo_remote_change (repo, NULL,
                                  opt_if_not_exists ? OSTREE_REPO_REMOTE_CHANGE_ADD_IF_NOT_EXISTS : 
                                  OSTREE_REPO_REMOTE_CHANGE_ADD,
                                  remote_name, remote_url,
                                  g_variant_builder_end (optbuilder),
                                  cancellable, error))
    goto out;

  ret = TRUE;
 out:
  g_option_context_free (context);

  return ret;
}
