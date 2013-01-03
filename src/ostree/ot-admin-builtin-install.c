/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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

#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree-curl-fetcher.h"
#include "otutil.h"

#include <unistd.h>
#include <stdlib.h>
#include <glib/gi18n.h>

typedef struct {
  GMainLoop *loop;
  GFile *osconfig_path;
} OtAdminBuiltinInstall;

static GOptionEntry options[] = {
  { NULL }
};

static void
on_keyfile_retrieved (GObject       *obj,
                      GAsyncResult  *result,
                      gpointer       user_data)
{
  OtAdminBuiltinInstall *self = user_data;
  GError *error = NULL;
  
  self->osconfig_path = ostree_curl_fetcher_request_uri_finish ((OstreeCurlFetcher*)obj, result, &error);
  if (!self->osconfig_path)
    goto out;
  
 out:
  if (error)
    {
      g_printerr ("%s\n", error->message);
      exit (1);
    }
  g_main_loop_quit (self->loop);
}

gboolean
ot_admin_builtin_install (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error)
{
  OtAdminBuiltinInstall self_data;
  OtAdminBuiltinInstall *self = &self_data;
  GOptionContext *context;
  gboolean ret = FALSE;
  const char *keyfile_arg = NULL;
  const char *treename_arg = NULL;
  GFile *ostree_dir = admin_opts->ostree_dir;
  ot_lobj GFile *deploy_dir = NULL;
  ot_lobj GFile *osdir = NULL;
  ot_lobj GFile *dest_osconfig_path = NULL;
  gs_unref_ptrarray GPtrArray *subproc_args = NULL;
  ot_lfree char *osname = NULL;
  ot_lfree char *repoarg = NULL;
  ot_lfree char *ostree_dir_arg = NULL;
  ot_lfree char *tree_to_deploy = NULL;
  GKeyFile *keyfile = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  memset (self, 0, sizeof (*self));

  context = g_option_context_new ("KEYFILE [TREE] - Initialize, download, and deploy operating system");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "KEYFILE must be specified", error);
      goto out;
    }

  self->loop = g_main_loop_new (NULL, TRUE);

  keyfile_arg = argv[1];
  if (argc > 2)
    treename_arg = argv[2];

  keyfile = g_key_file_new ();

  if (g_str_has_prefix (keyfile_arg, "http://") || g_str_has_prefix (keyfile_arg, "https://"))
    {
      ot_lobj GFile *tmp = g_file_new_for_path (g_get_tmp_dir ());
      ot_lobj OstreeCurlFetcher *fetcher = ostree_curl_fetcher_new (tmp);

      g_print ("Fetching %s...\n", keyfile_arg);
      ostree_curl_fetcher_request_uri_async (fetcher, keyfile_arg, cancellable,
                                             on_keyfile_retrieved, self);

      g_main_loop_run (self->loop);
    }
  else
    {
      self->osconfig_path = g_file_new_for_path (keyfile_arg);
    }

  if (!g_key_file_load_from_file (keyfile, gs_file_get_path_cached (self->osconfig_path), 0,
                                  error))
    goto out;

  osname = g_key_file_get_string (keyfile, "os", "Name", error);

  ostree_dir_arg = g_strconcat ("--ostree-dir=",
                                gs_file_get_path_cached (ostree_dir),
                                NULL);

  if (!gs_subprocess_simple_run_sync (gs_file_get_path_cached (ostree_dir),
                                      GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
                                      cancellable, error,
                                      "ostree", "admin", ostree_dir_arg, "os-init", osname, NULL))
    goto out;

  if (treename_arg)
    {
      tree_to_deploy = g_strdup (treename_arg);
    }
  else
    {
      tree_to_deploy = g_key_file_get_string (keyfile, "os", "TreeDefault", error);
      if (!tree_to_deploy)
        goto out;
    }

  osdir = ot_gfile_get_child_build_path (ostree_dir, "deploy", osname, NULL);
  dest_osconfig_path = ot_gfile_get_child_strconcat (osdir, osname, ".cfg", NULL);

  if (!g_file_copy (self->osconfig_path, dest_osconfig_path, G_FILE_COPY_OVERWRITE | G_FILE_COPY_TARGET_DEFAULT_PERMS,
                    cancellable, NULL, NULL, error))
    goto out;

  if (!gs_file_unlink (self->osconfig_path, cancellable, error))
    goto out;

  repoarg = g_strconcat ("--repo=",
                         gs_file_get_path_cached (ostree_dir), "/repo",
                         NULL);

  {
    ot_lfree char *repourl = NULL;
      
    repourl = g_key_file_get_string (keyfile, "os", "Repo", error);
    if (!repourl)
      goto out;
    
    if (!gs_subprocess_simple_run_sync (gs_file_get_path_cached (ostree_dir),
                                        GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
                                        cancellable, error,
                                        "ostree", repoarg, "remote", "add",
                                        osname, repourl, tree_to_deploy, NULL))
      goto out;
  }

  if (!gs_subprocess_simple_run_sync (gs_file_get_path_cached (ostree_dir),
                                      GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
                                      cancellable, error,
                                        "ostree", "pull", repoarg, osname, NULL))
    goto out;

  if (!gs_subprocess_simple_run_sync (gs_file_get_path_cached (ostree_dir),
                                      GS_SUBPROCESS_STREAM_DISPOSITION_NULL,
                                      cancellable, error,
                                      "ostree", "admin", ostree_dir_arg, "deploy", osname,
                                      tree_to_deploy, NULL))
    goto out;

  ret = TRUE;
 out:
  if (self->loop)
    g_main_loop_unref (self->loop);
  g_clear_object (&self->osconfig_path);
  g_clear_pointer (&keyfile, (GDestroyNotify) g_key_file_unref);
  if (context)
    g_option_context_free (context);
  return ret;
}
