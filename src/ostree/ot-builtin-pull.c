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

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

static gboolean opt_disable_fsync;
static gboolean opt_mirror;
static gboolean opt_commit_only;
static gboolean opt_dry_run;
static gboolean opt_disable_static_deltas;
static gboolean opt_require_static_deltas;
static gboolean opt_untrusted;
static char* opt_subpath;
static char* opt_cache_dir;
static int opt_depth = 0;
 
static GOptionEntry options[] = {
   { "commit-metadata-only", 0, 0, G_OPTION_ARG_NONE, &opt_commit_only, "Fetch only the commit metadata", NULL },
   { "cache-dir", 0, 0, G_OPTION_ARG_STRING, &opt_cache_dir, "Use custom cache dir", NULL },
   { "disable-fsync", 0, 0, G_OPTION_ARG_NONE, &opt_disable_fsync, "Do not invoke fsync()", NULL },
   { "disable-static-deltas", 0, 0, G_OPTION_ARG_NONE, &opt_disable_static_deltas, "Do not use static deltas", NULL },
   { "require-static-deltas", 0, 0, G_OPTION_ARG_NONE, &opt_require_static_deltas, "Require static deltas", NULL },
   { "mirror", 0, 0, G_OPTION_ARG_NONE, &opt_mirror, "Write refs suitable for a mirror", NULL },
   { "subpath", 0, 0, G_OPTION_ARG_STRING, &opt_subpath, "Only pull the provided subpath", NULL },
   { "untrusted", 0, 0, G_OPTION_ARG_NONE, &opt_untrusted, "Do not trust (local) sources", NULL },
   { "dry-run", 0, 0, G_OPTION_ARG_NONE, &opt_dry_run, "Only print information on what will be downloaded (requires static deltas)", NULL },
   { "depth", 0, 0, G_OPTION_ARG_INT, &opt_depth, "Traverse DEPTH parents (-1=infinite) (default: 0)", "DEPTH" },
   { NULL }
 };

static void
gpg_verify_result_cb (OstreeRepo *repo,
                      const char *checksum,
                      OstreeGpgVerifyResult *result,
                      GSConsole *console)
{
  /* Temporarily place the GSConsole stream (which is just stdout)
   * back in normal mode before printing GPG verification results. */
  gs_console_end_status_line (console, NULL, NULL);

  g_print ("\n");
  ostree_print_gpg_verify_result (result);

  gs_console_begin_status_line (console, "", NULL, NULL);
}

static gboolean printed_console_progress;

static void
dry_run_console_progress_changed (OstreeAsyncProgress *progress,
                                  gpointer             user_data)
{
  guint fetched_delta_parts, total_delta_parts;
  guint64 total_delta_part_size, total_delta_part_usize;
  GString *buf;

  g_assert (!printed_console_progress);
  printed_console_progress = TRUE;

  fetched_delta_parts = ostree_async_progress_get_uint (progress, "fetched-delta-parts");
  total_delta_parts = ostree_async_progress_get_uint (progress, "total-delta-parts");
  total_delta_part_size = ostree_async_progress_get_uint64 (progress, "total-delta-part-size");
  total_delta_part_usize = ostree_async_progress_get_uint64 (progress, "total-delta-part-usize");

  buf = g_string_new ("");

  { g_autofree char *formatted_size =
      g_format_size (total_delta_part_size);
    g_autofree char *formatted_usize =
      g_format_size (total_delta_part_usize);

    g_string_append_printf (buf, "Delta update: %u/%u parts, %s to transfer, %s uncompressed",
                            fetched_delta_parts, total_delta_parts,
                            formatted_size, formatted_usize);
  }
  g_print ("%s\n", buf->str);
  g_string_free (buf, TRUE);
}

gboolean
ostree_builtin_pull (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  glnx_unref_object OstreeRepo *repo = NULL;
  gboolean ret = FALSE;
  g_autofree char *remote = NULL;
  OstreeRepoPullFlags pullflags = 0;
  GSConsole *console = NULL;
  g_autoptr(GPtrArray) refs_to_fetch = NULL;
  g_autoptr(GPtrArray) override_commit_ids = NULL;
  glnx_unref_object OstreeAsyncProgress *progress = NULL;
  gulong signal_handler_id = 0;

  context = g_option_context_new ("REMOTE [BRANCH...] - Download data from remote repository");

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &repo, cancellable, error))
    goto out;

  if (!ostree_ensure_repo_writable (repo, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "REMOTE must be specified", error);
      goto out;
    }

  if (opt_disable_fsync)
    ostree_repo_set_disable_fsync (repo, TRUE);

  if (opt_cache_dir)
    {
      if (!ostree_repo_set_cache_dir (repo, AT_FDCWD, opt_cache_dir, cancellable, error))
        goto out;
    }

  if (opt_mirror)
    pullflags |= OSTREE_REPO_PULL_FLAGS_MIRROR;

  if (opt_commit_only)
    pullflags |= OSTREE_REPO_PULL_FLAGS_COMMIT_ONLY;

  if (opt_untrusted)
    pullflags |= OSTREE_REPO_PULL_FLAGS_UNTRUSTED;

  if (opt_dry_run && !opt_require_static_deltas)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "--dry-run requires --require-static-deltas");
      goto out;
    }

  if (strchr (argv[1], ':') == NULL)
    {
      remote = g_strdup (argv[1]);
      if (argc > 2)
        {
          int i;
          refs_to_fetch = g_ptr_array_new_with_free_func (g_free);

          for (i = 2; i < argc; i++)
            {
              const char *at = strrchr (argv[i], '@');

              if (at)
                {
                  guint j;
                  const char *override_commit_id = at + 1;

                  if (!ostree_validate_checksum_string (override_commit_id, error))
                    goto out;

                  if (!override_commit_ids)
                    override_commit_ids = g_ptr_array_new_with_free_func (g_free);

                  /* Backfill */
                  for (j = 2; j < i; i++)
                    g_ptr_array_add (override_commit_ids, g_strdup (""));

                  g_ptr_array_add (override_commit_ids, g_strdup (override_commit_id));
                  g_ptr_array_add (refs_to_fetch, g_strndup (argv[i], at - argv[i]));
                }
              else
                {
                  g_ptr_array_add (refs_to_fetch, g_strdup (argv[i]));
                }
            }

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

  if (!opt_dry_run)
    {
      console = gs_console_get ();
      if (console)
        {
          gs_console_begin_status_line (console, "", NULL, NULL);
          progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, console);
          signal_handler_id = g_signal_connect (repo, "gpg-verify-result",
                                                G_CALLBACK (gpg_verify_result_cb),
                                                console);
        }
    }
  else
    {
      progress = ostree_async_progress_new_and_connect (dry_run_console_progress_changed, console);
      signal_handler_id = g_signal_connect (repo, "gpg-verify-result",
                                            G_CALLBACK (gpg_verify_result_cb),
                                            console);
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
   
    g_variant_builder_add (&builder, "{s@v}", "disable-static-deltas",
                           g_variant_new_variant (g_variant_new_boolean (opt_disable_static_deltas)));

    g_variant_builder_add (&builder, "{s@v}", "require-static-deltas",
                           g_variant_new_variant (g_variant_new_boolean (opt_require_static_deltas)));

    g_variant_builder_add (&builder, "{s@v}", "dry-run",
                           g_variant_new_variant (g_variant_new_boolean (opt_dry_run)));

    if (override_commit_ids)
      g_variant_builder_add (&builder, "{s@v}", "override-commit-ids",
                             g_variant_new_variant (g_variant_new_strv ((const char*const*)override_commit_ids->pdata, override_commit_ids->len)));

    if (!ostree_repo_pull_with_options (repo, remote, g_variant_builder_end (&builder),
                                        progress, cancellable, error))
      goto out;
  }

  if (progress)
    ostree_async_progress_finish (progress);

  if (opt_dry_run)
    g_assert (printed_console_progress);

  ret = TRUE;
 out:
  if (signal_handler_id > 0)
    g_signal_handler_disconnect (repo, signal_handler_id);

  if (console)
    gs_console_end_status_line (console, NULL, NULL);
 
  if (context)
    g_option_context_free (context);
  return ret;
}
