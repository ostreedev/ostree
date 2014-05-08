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

GS_DEFINE_CLEANUP_FUNCTION0(GKeyFile*, local_keyfile_unref, g_key_file_unref)
#define local_cleanup_keyfile __attribute__ ((cleanup(local_keyfile_unref)))

static gboolean
add_remote_to_keyfile (GKeyFile       *new_keyfile,
                       const char     *key,
                       const char     *url,
                       GPtrArray      *branches,
                       GError        **error)
{
  gboolean ret = FALSE;
  char **iter;

  g_key_file_set_string (new_keyfile, key, "url", url);

  for (iter = opt_set; iter && *iter; iter++)
    {
      const char *keyvalue = *iter;
      gs_free char *subkey = NULL;
      gs_free char *subvalue = NULL;

      if (!parse_keyvalue (keyvalue, &subkey, &subvalue, error))
        goto out;

      g_key_file_set_string (new_keyfile, key, subkey, subvalue);
    }

  if (branches->len > 0)
    g_key_file_set_string_list (new_keyfile, key, "branches",
                                    (const char *const *)branches->pdata,
                                branches->len);
  
  if (opt_no_gpg_verify)
    g_key_file_set_boolean (new_keyfile, key, "gpg-verify", FALSE);

  ret = TRUE;
 out:
  return ret;
}
                       

gboolean
ostree_builtin_remote (int argc, char **argv, OstreeRepo *repo, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  const char *op;
  gs_free char *key = NULL;
  guint i;
  GKeyFile *config = NULL;
  const char *remote_name;

  context = g_option_context_new ("OPERATION NAME [args] - Control remote repository configuration");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
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
      const char *url = argv[3];
      gs_unref_object GFile *etc_ostree_remotes_d = g_file_new_for_path (SYSCONFDIR "/ostree/remotes.d");
      gs_free char *target_name = NULL;
      gs_unref_object GFile *target_conf = NULL;
      local_cleanup_keyfile GKeyFile *new_keyfile = NULL;
      GKeyFile *target_keyfile = NULL;
      gs_unref_ptrarray GPtrArray *branches = NULL;

      if (strchr (remote_name, '/') != NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid character '/' in remote name: %s",
                       remote_name);
          goto out;
        }
      
      if (argc < 4)
        {
          usage_error (context, "URL must be specified", error);
          goto out;
        }

      branches = g_ptr_array_new ();
      for (i = 4; i < argc; i++)
        g_ptr_array_add (branches, argv[i]);

      if (ostree_repo_is_system (repo))
        {
          new_keyfile = g_key_file_new ();

          target_keyfile = new_keyfile;

          target_name = g_strconcat (remote_name, ".conf", NULL);
          target_conf = g_file_get_child (etc_ostree_remotes_d, target_name);
          
          if (g_file_query_exists (target_conf, NULL))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Remote configuration already exists: %s",
                           gs_file_get_path_cached (target_conf));
              goto out;
            }
        }
      else
        {
          target_keyfile = config;
        }

      if (!add_remote_to_keyfile (target_keyfile, key, url, branches, error))
        goto out;

      /* For the system repository, write to /etc/ostree/remotes.d */
      if (ostree_repo_is_system (repo))
        {
          gsize len;
          gs_free char *data = g_key_file_to_data (target_keyfile, &len, error);
          if (!g_file_replace_contents (target_conf, data, len,
                                        NULL, FALSE, 0, NULL,
                                        cancellable, error))
            goto out;
        }
      else
        {
          if (!ostree_repo_write_config (repo, config, error))
            goto out;
        }
    }
  else if (!strcmp (op, "show-url"))
    {
      gs_free char *url = NULL;

      url = g_key_file_get_string (config, key, "url", error);
      if (url == NULL)
        goto out;

      g_print ("%s\n", url);
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
