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

#include "otutil.h"
#include "ob-builtins.h"

#include <glib/gi18n.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static char *repo_path;
static char *ref;
static char *name;
static char *generator;
static char *resultdir;
static gboolean raw;

static GOptionEntry options[] = {
  { "repo", 0, 0, G_OPTION_ARG_FILENAME, &repo_path, "Repository path", "repo" },
  { "rev", 'r', 0, G_OPTION_ARG_STRING, &ref, "Build using this tree", "rev" },
  { "name", 0, 0, G_OPTION_ARG_STRING, &name, "Name of the source", "source" },
  { "generator", 0, 0, G_OPTION_ARG_FILENAME, &generator, "Script to run on installed tree", "script" },
  { "raw", 0, 0, G_OPTION_ARG_NONE, &raw, "Do not instantiate a tree, use current", NULL },
  { "resultdir", 0, 0, G_OPTION_ARG_FILENAME, &resultdir, "Directory for output artifacts", "dir" },
  { NULL }
};

static char *
get_tmpdir (void) G_GNUC_UNUSED;

static char *
get_tmpdir (void)
{
  char *tmp_prefix = g_strdup (g_getenv ("XDG_RUNTIME_DIR"));
  char *ret;
  
  if (tmp_prefix)
    {
      ret = g_strdup_printf ("%s/osbuild", tmp_prefix);
    }
  else
    {
      tmp_prefix = g_strdup_printf ("/tmp/osbuild-%d", getuid ());
      if (!g_file_test (tmp_prefix, G_FILE_TEST_IS_DIR))
        {
          if (!mkdir (tmp_prefix, 0755))
            {
              g_printerr ("Failed to make logging directory %s\n", tmp_prefix);
              exit (1);
            }
        }
      ret = tmp_prefix;
      tmp_prefix = NULL;
    }
  g_free (tmp_prefix);
  return ret;
}

static gboolean
open_log (const char *name, 
          GOutputStream **out_log,
          GError **error) G_GNUC_UNUSED;

static gboolean
open_log (const char *name, 
          GOutputStream **out_log,
          GError **error)
{
  gboolean ret = FALSE;
  char *tmpdir = NULL;
  char *path = NULL;
  GFile *logf = NULL;
  GFileOutputStream *ret_log = NULL;

  path = g_strdup_printf ("%s/%s.log", tmpdir, name);
  logf = ot_gfile_new_for_path (path);

  ret_log = g_file_replace (logf, NULL, FALSE, 0, NULL, error);
  if (!ret_log)
    goto out;

  ret = TRUE;
  *out_log = (GOutputStream*)ret_log;
  ret_log = NULL;
 out:
  g_free (path);
  g_free (tmpdir);
  g_clear_object (&logf);
  g_clear_object (&ret_log);
  return ret;
}

gboolean
osbuild_builtin_buildone (int argc, char **argv, const char *prefix, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;

  context = g_option_context_new ("- Build current directory");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (!raw && !repo_path)
    {
      ot_util_usage_error (context, "A result directory must be specified with --resultdir", error);
      goto out;
    }

  if (!generator)
    generator = g_build_filename (LIBEXECDIR, "ostree", "generators", "default", NULL);

  

 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
