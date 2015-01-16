/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
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

#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"
#include "ot-tool-util.h"
#include "otutil.h"
#include "libgsystem.h"

#include <unistd.h>
#include <stdlib.h>
#include <glib/gi18n.h>

static int opt_index = -1;
static char **opt_set;

static GOptionEntry options[] = {
  { "set", 's', 0, G_OPTION_ARG_STRING_ARRAY, &opt_set, "Set config option KEY=VALUE for remote", "KEY=VALUE" },
  { "index", 0, 0, G_OPTION_ARG_INT, &opt_index, "Operate on the deployment INDEX, starting from zero", "INDEX" },
  { NULL }
};

gboolean
ot_admin_builtin_set_origin (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  const char *remotename = NULL;
  const char *url = NULL;
  const char *branch = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  gs_unref_object OstreeSysroot *sysroot = NULL;
  OstreeDeployment *target_deployment = NULL;

  context = g_option_context_new ("REMOTENAME URL [BRANCH]");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv, &sysroot, cancellable, error))
    goto out;

  if (argc < 3)
    {
      ot_util_usage_error (context, "REMOTENAME and URL must be specified", error);
      goto out;
    }

  remotename = argv[1];
  url = argv[2];
  if (argc > 3)
    branch = argv[3];

  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  if (opt_index == -1)
    {
      target_deployment = ostree_sysroot_get_booted_deployment (sysroot);
      if (target_deployment == NULL)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Not currently booted into an OSTree system");
          goto out;
        }
    }
  else
    {
      target_deployment = ot_admin_get_indexed_deployment (sysroot, opt_index, error);
      if (!target_deployment)
        goto out;
    }

  { char **iter;
    gs_unref_variant_builder GVariantBuilder *optbuilder =
      g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

    for (iter = opt_set; iter && *iter; iter++)
      {
        const char *keyvalue = *iter;
        gs_free char *subkey = NULL;
        gs_free char *subvalue = NULL;

        if (!ot_parse_keyvalue (keyvalue, &subkey, &subvalue, error))
          goto out;

        g_variant_builder_add (optbuilder, "{s@v}",
                               subkey, g_variant_new_variant (g_variant_new_string (subvalue)));
      }
    
    if (!ostree_repo_remote_change (repo, NULL,
                                    OSTREE_REPO_REMOTE_CHANGE_ADD_IF_NOT_EXISTS, 
                                    remotename, url,
                                    g_variant_builder_end (optbuilder),
                                    cancellable, error))
      goto out;
  }
  
  { GKeyFile *old_origin = ostree_deployment_get_origin (target_deployment);
    gs_free char *origin_refspec = g_key_file_get_string (old_origin, "origin", "refspec", NULL);
    gs_free char *new_refspec = NULL;
    gs_free char *origin_remote = NULL;
    gs_free char *origin_ref = NULL;
  
    if (!ostree_parse_refspec (origin_refspec, &origin_remote, &origin_ref, error))
      goto out;

    { gs_free char *new_refspec = g_strconcat (remotename, ":", branch ? branch : origin_ref, NULL);
      gs_unref_keyfile GKeyFile *new_origin = NULL;
      gs_unref_object GFile *origin_path = NULL;
      
      new_origin = ostree_sysroot_origin_new_from_refspec (sysroot, new_refspec);

      if (!ostree_sysroot_write_origin_file (sysroot, target_deployment, new_origin,
                                             cancellable, error))
        goto out;
    }
  }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
