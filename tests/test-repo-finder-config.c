/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright © 2017 Endless Mobile, Inc.
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
 * Authors:
 *  - Philip Withnall <withnall@endlessm.com>
 */

#include "config.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <libglnx/glnx-dirfd.h>
#include <libglnx/glnx-fdio.h>
#include <libglnx/glnx-local-alloc.h>
#include <libglnx/glnx-shutil.h>
#include <locale.h>
#include <string.h>

#include "ostree-autocleanups.h"
#include "ostree-repo-finder.h"
#include "ostree-repo-finder-config.h"

/* Test fixture. Creates a temporary directory. */
typedef struct
{
  int refs_dfd;  /* owned */
  gchar *refs_path;  /* owned */
} Fixture;

static void
setup (Fixture       *fixture,
       gconstpointer  test_data)
{
  g_autofree gchar *tmp_name = NULL;
  g_autoptr(GError) error = NULL;

  tmp_name = g_strdup ("test-repo-finder-config-XXXXXX");
  glnx_mkdtempat_open_in_system (tmp_name, 0700, &fixture->refs_dfd, &error);
  g_assert_no_error (error);
  fixture->refs_path = g_steal_pointer (&tmp_name);
}

static void
teardown (Fixture       *fixture,
          gconstpointer  test_data)
{
  /* Recursively remove the temporary directory. */
  glnx_shutil_rm_rf_at (fixture->refs_dfd, ".", NULL, NULL);

  close (fixture->refs_dfd);
  fixture->refs_dfd = -1;

  g_clear_pointer (&fixture->refs_path, g_free);
}

/* Test the object constructor works at a basic level. */
static void
test_repo_finder_config_init (void)
{
  g_autoptr(OstreeRepoFinderConfig) finder = NULL;

  /* Default everything. */
  finder = ostree_repo_finder_config_new ();
}

#if 0
TODO
static void
result_cb (GObject      *source_object,
           GAsyncResult *result,
           gpointer      user_data)
{
  GAsyncResult **result_out = user_data;
  *result_out = g_object_ref (result);
}

/* Test that no remotes are found if there are no config files in the refs
 * directory. */
static void
test_repo_finder_config_no_configs (Fixture       *fixture,
                                    gconstpointer  test_data)
{
  g_autoptr(OstreeRepoFinderConfig) finder = NULL;
  g_autoptr(GMainContext) context = NULL;
  g_autoptr(GAsyncResult) result = NULL;
  g_autoptr(GPtrArray) results = NULL;  /* (element-type OstreeRepoFinderResult) */
  g_autoptr(GError) error = NULL;
  const gchar * const refs[] =
    {
      "exampleos/x86_64/standard",
      "exampleos/x86_64/buildmaster/standard",
      NULL
    };

  context = g_main_context_new ();
  g_main_context_push_thread_default (context);

  finder = ostree_repo_finder_config_new (fixture->refs_dfd, fixture->refs_path);

  ostree_repo_finder_resolve_async (OSTREE_REPO_FINDER (finder), refs,
                                    NULL, result_cb, &result);

  while (result == NULL)
    g_main_context_iteration (context, TRUE);

  results = ostree_repo_finder_resolve_finish (OSTREE_REPO_FINDER (finder),
                                               result, &error);
  g_assert_no_error (error);
  g_assert_nonnull (results);
  g_assert_cmpuint (results->len, ==, 0);

  g_main_context_pop_thread_default (context);
}

/* Create a config file named $ref_name.conf in the given @refs_dfd,
 * containing @repo_uri as the configuration. If @repo_uri is %NULL, the config
 * file will contain invalid content (to trigger parser failure). */
static void
assert_create_ref_config (int          refs_dfd,
                          const gchar *ref_name,
                          const gchar *remote_name,
                          const gchar *repo_uri)
{
  g_autofree gchar *ref_file_name = NULL;
  g_autofree gchar *ref_basename = NULL;
  g_autofree gchar *ref_dirname = NULL;
  glnx_fd_close int ref_dirname_fd = -1;
  g_autofree gchar *config_file_contents = NULL;
  g_autoptr(GError) error = NULL;

  /* The @ref_dirname is not necessarily @refs_dfd, since @ref_name
   * may contain slashes. */
  ref_file_name = g_strconcat (ref_name, ".conf", NULL);
  ref_basename = g_path_get_basename (ref_file_name);
  ref_dirname = g_path_get_dirname (ref_file_name);

  glnx_shutil_mkdir_p_at_open (refs_dfd, ref_dirname, 0755, &ref_dirname_fd, NULL, &error);
  g_assert_no_error (error);

  if (remote_name != NULL && repo_uri != NULL)
    config_file_contents = g_strdup_printf ("[remote \"%s\"]\nurl=%s\n",
                                            remote_name, repo_uri);
  else
    config_file_contents = g_strdup ("an invalid config file");

  glnx_file_replace_contents_at (ref_dirname_fd, ref_basename,
                                 (const guint8 *) config_file_contents,
                                 strlen (config_file_contents),
                                 0  /* no flags */,
                                 NULL, &error);
  g_assert_no_error (error);
}

/* Test resolving the refs against a collection of config files, which contain
 * valid, invalid or duplicate repo information. */
static void
test_repo_finder_config_mixed_configs (Fixture       *fixture,
                                       gconstpointer  test_data)
{
  g_autoptr(OstreeRepoFinderConfig) finder = NULL;
  g_autoptr(GMainContext) context = NULL;
  g_autoptr(GAsyncResult) result = NULL;
  g_autoptr(GPtrArray) results = NULL;  /* (element-type OstreeRepoFinderResult) */
  g_autoptr(GError) error = NULL;
  gsize i;
  const gchar * const refs[] =
    {
      "exampleos/x86_64/ref0",
      "exampleos/x86_64/ref1",
      "exampleos/x86_64/ref2",
      "exampleos/x86_64/ref3",
      NULL
    };

  context = g_main_context_new ();
  g_main_context_push_thread_default (context);

  /* Put together various ref configuration files. */
  assert_create_ref_config (fixture->refs_dfd, refs[0], "remote0", "http://ref0");
  assert_create_ref_config (fixture->refs_dfd, refs[1], "remote1", "http://ref1");
  assert_create_ref_config (fixture->refs_dfd, refs[2], "remote0", "http://ref0");
  assert_create_ref_config (fixture->refs_dfd, refs[3], NULL, NULL);

  finder = ostree_repo_finder_config_new (fixture->refs_dfd, fixture->refs_path);

  /* Resolve the refs. */
  ostree_repo_finder_resolve_async (OSTREE_REPO_FINDER (finder), refs,
                                    NULL, result_cb, &result);

  while (result == NULL)
    g_main_context_iteration (context, TRUE);

  results = ostree_repo_finder_resolve_finish (OSTREE_REPO_FINDER (finder),
                                               result, &error);
  g_assert_no_error (error);
  g_assert_nonnull (results);
  g_assert_cmpuint (results->len, ==, 2);

  /* Check that the results are correct: the invalid refs should have been
   * ignored, and the valid results canonicalised and deduplicated. */
  for (i = 0; i < results->len; i++)
    {
      g_autofree gchar *uri = NULL;
      const OstreeRepoFinderResult *result = g_ptr_array_index (results, i);

      uri = g_key_file_get_string (result->remote->options, result->remote->group, "url", &error);
      g_assert_no_error (error);

      if (g_strcmp0 (result->remote->name, "remote0") == 0)
        {
          g_assert_cmpstr (uri, ==, "http://ref0");
          g_assert_cmpuint (g_strv_length (result->refs), ==, 2);
          g_assert_true (g_strv_contains ((const gchar * const *) result->refs, refs[0]));
          g_assert_true (g_strv_contains ((const gchar * const *) result->refs, refs[2]));
        }
      else if (g_strcmp0 (result->remote->name, "remote1") == 0)
        {
          g_assert_cmpstr (uri, ==, "http://ref1");
          g_assert_cmpuint (g_strv_length (result->refs), ==, 1);
          g_assert_true (g_strv_contains ((const gchar * const *) result->refs, refs[1]));
        }
      else
        {
          g_assert_not_reached ();
        }
    }

  g_main_context_pop_thread_default (context);
}
#endif

int main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/repo-finder-config/init", test_repo_finder_config_init);
#if 0
  g_test_add ("/repo-finder-config/no-configs", Fixture, NULL, setup,
              test_repo_finder_config_no_configs, teardown);
  g_test_add ("/repo-finder-config/mixed-configs", Fixture, NULL, setup,
              test_repo_finder_config_mixed_configs, teardown);
#endif

  return g_test_run();
}
