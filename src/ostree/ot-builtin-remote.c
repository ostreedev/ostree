/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

char **opt_set;
gboolean opt_no_gpg_verify;

static GOptionEntry options[] = {
  { "set", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_set, "Set config option KEY=VALUE for remote", "KEY=VALUE" },
  { "no-gpg-verify", 0, 0, G_OPTION_ARG_NONE, &opt_no_gpg_verify, "Disable GPG verification", NULL },
  { NULL }
};

static void
usage_error (GOptionContext *context, const char *message, GError **error)
{
  gchar *help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s\n", help);
  g_free (help);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       message);
}

static gboolean
parse_keyvalue (const char  *keyvalue,
                char       **out_key,
                char       **out_value,
                GError     **error)
{
  const char *eq = strchr (keyvalue, '=');
  if (!eq)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Missing '=' in KEY=VALUE for --set");
      return FALSE;
    }
  *out_key = g_strndup (keyvalue, eq - keyvalue);
  *out_value = g_strdup (eq + 1);
  return TRUE;
}

gboolean
ostree_builtin_remote (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gs_unref_object OstreeRepo *repo = NULL;
  gboolean ret = FALSE;
  const char *op;
  gs_free char *key = NULL;
  guint i;
  GKeyFile *config = NULL;
  const char *remote_name;

  context = g_option_context_new ("OPERATION NAME [args] - Control remote repository configuration");

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (argc < 3)
    {
      if (argc == 1)
        usage_error (context, "OPERATION must be specified", error);
      else
        usage_error (context, "NAME must be specified", error);

      goto out;
    }

  op = argv[1];
  remote_name = argv[2];
  key = g_strdup_printf ("remote \"%s\"", remote_name);

  config = ostree_repo_copy_config (repo);

  if (!strcmp (op, "add"))
    {
      const char *url;
      char **iter;
      gs_free char *target_name = NULL;
      gs_unref_object GFile *target_conf = NULL;
      gs_unref_variant_builder GVariantBuilder *optbuilder = NULL;

      if (argc < 4)
        {
          usage_error (context, "URL must be specified", error);
          goto out;
        }

      url = argv[3];

      optbuilder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

      if (argc > 4)
        {
          gs_unref_ptrarray GPtrArray *branchesp = g_ptr_array_new ();

          for (i = 4; i < argc; i++)
            g_ptr_array_add (branchesp, argv[i]);
          g_ptr_array_add (branchesp, NULL);

          g_variant_builder_add (optbuilder, "{s@v}",
                                 "branches",
                                 g_variant_new_variant (g_variant_new_strv ((const char*const*)branchesp->pdata, -1)));
        }

      for (iter = opt_set; iter && *iter; iter++)
        {
          const char *keyvalue = *iter;
          gs_free char *subkey = NULL;
          gs_free char *subvalue = NULL;

          if (!parse_keyvalue (keyvalue, &subkey, &subvalue, error))
            goto out;

          g_variant_builder_add (optbuilder, "{s@v}",
                                 subkey, g_variant_new_variant (g_variant_new_string (subvalue)));
        }

      if (opt_no_gpg_verify)
        g_variant_builder_add (optbuilder, "{s@v}",
                               "gpg-verify",
                               g_variant_new_variant (g_variant_new_boolean (FALSE)));

      if (!ostree_repo_remote_add (repo, remote_name, url,
                                   g_variant_builder_end (optbuilder),
                                   cancellable, error))
        goto out;
    }
  else if (!strcmp (op, "show-url"))
    {
      gs_free char *url = NULL;

      url = g_key_file_get_string (config, key, "url", error);
      if (url == NULL)
        goto out;

      g_print ("%s\n", url);
    }
  else if (!strcmp (op, "delete"))
    {
      if (!ostree_repo_remote_delete (repo, remote_name, cancellable, error))
        goto out;
    }
  else
    {
      usage_error (context, "Unknown operation", error);
      goto out;
    }

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  if (config)
    g_key_file_free (config);
  return ret;
}
