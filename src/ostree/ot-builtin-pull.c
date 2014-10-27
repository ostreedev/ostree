/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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
#include "ot-builtins-common.h"
#include "ostree.h"
#include "otutil.h"

static gboolean opt_disable_fsync;
static gboolean opt_mirror;
static char* opt_subpath;
static int opt_depth = 0;
 
 static GOptionEntry options[] = {
   { "disable-fsync", 0, 0, G_OPTION_ARG_NONE, &opt_disable_fsync, "Do not invoke fsync()", NULL },
   { "mirror", 0, 0, G_OPTION_ARG_NONE, &opt_mirror, "Write refs suitable for a mirror", NULL },
   { "subpath", 0, 0, G_OPTION_ARG_STRING, &opt_subpath, "Only pull the provided subpath", NULL },
   { "depth", 0, 0, G_OPTION_ARG_INT, &opt_depth, "Traverse DEPTH parents (-1=infinite) (default: 0)", "DEPTH" },
   { NULL }
 };

gboolean
ostree_builtin_pull (int argc, char **argv, OstreeRepo *repo, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  gs_free char *remote = NULL;
  OstreeRepoPullFlags pullflags = 0;
  GSConsole *console = NULL;
  gs_unref_ptrarray GPtrArray *refs_to_fetch = NULL;
  gs_unref_object OstreeAsyncProgress *progress = NULL;

  context = g_option_context_new ("REMOTE [BRANCH...] - Download data from remote repository");
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "REMOTE must be specified", error);
      goto out;
    }

  if (opt_disable_fsync)
    ostree_repo_set_disable_fsync (repo, TRUE);

  if (opt_mirror)
    pullflags |= OSTREE_REPO_PULL_FLAGS_MIRROR;

  if (strchr (argv[1], ':') == NULL)
    {
      remote = g_strdup (argv[1]);
      if (argc > 2)
        {
          int i;
          refs_to_fetch = g_ptr_array_new ();
          for (i = 2; i < argc; i++)
            g_ptr_array_add (refs_to_fetch, argv[i]);
          g_ptr_array_add (refs_to_fetch, NULL);
        }
    }
  else
    {
      char *ref_to_fetch;
      refs_to_fetch = g_ptr_array_new ();
      if (!ostree_parse_refspec (argv[1], &remote, &ref_to_fetch, error))
        goto out;
      /* Transfer ownership */
      g_ptr_array_add (refs_to_fetch, ref_to_fetch);
      g_ptr_array_add (refs_to_fetch, NULL);
    }

  console = gs_console_get ();
  if (console)
    {
      gs_console_begin_status_line (console, "", NULL, NULL);
      progress = ostree_async_progress_new_and_connect (ot_common_pull_progress, console);
    }

  {
    GVariantBuilder builder;
    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

    if (opt_subpath)
      g_variant_builder_add (&builder, "{s@v}", "subdir",
                             g_variant_new_variant (g_variant_new_string (opt_subpath)));
    g_variant_builder_add (&builder, "{s@v}", "flags",
                           g_variant_new_variant (g_variant_new_int32 (pullflags)));
    if (refs_to_fetch)
      g_variant_builder_add (&builder, "{s@v}", "refs",
                             g_variant_new_variant (g_variant_new_strv ((const char *const*) refs_to_fetch->pdata, -1)));
    g_variant_builder_add (&builder, "{s@v}", "depth",
                           g_variant_new_variant (g_variant_new_int32 (opt_depth)));
    
    if (!ostree_repo_pull_with_options (repo, remote, g_variant_builder_end (&builder),
                                        progress, cancellable, error))
      goto out;
  }

  if (progress)
    ostree_async_progress_finish (progress);

  ret = TRUE;
 out:
  if (console)
    gs_console_end_status_line (console, NULL, NULL);
 
  if (context)
    g_option_context_free (context);
  return ret;
}
