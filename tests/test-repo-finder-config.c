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
#include <libglnx.h>
#include <locale.h>
#include <string.h>

#include "libostreetest.h"
#include "ostree-autocleanups.h"
#include "ostree-repo-finder.h"
#include "ostree-repo-finder-config.h"

/* Test fixture. Creates a temporary directory. */
typedef struct
{
  OstreeRepo *parent_repo;  /* owned */
  int working_dfd;  /* owned */
  GFile *working_dir;  /* owned */
} Fixture;

static void
setup (Fixture       *fixture,
       gconstpointer  test_data)
{
  g_autofree gchar *tmp_name = NULL;
  g_autoptr(GError) error = NULL;

  tmp_name = g_strdup ("test-repo-finder-config-XXXXXX");
  glnx_mkdtempat_open_in_system (tmp_name, 0700, &fixture->working_dfd, &error);
  g_assert_no_error (error);

  g_test_message ("Using temporary directory: %s", tmp_name);

  glnx_shutil_mkdir_p_at (fixture->working_dfd, "repo", 0700, NULL, &error);
  g_assert_no_error (error);

  g_autoptr(GFile) tmp_dir = g_file_new_for_path (g_get_tmp_dir ());
  fixture->working_dir = g_file_get_child (tmp_dir, tmp_name);

  fixture->parent_repo = ot_test_setup_repo (NULL, &error);
  g_assert_no_error (error);
}

static void
teardown (Fixture       *fixture,
          gconstpointer  test_data)
{
  g_autoptr(GError) error = NULL;

  /* Recursively remove the temporary directory. */
  glnx_shutil_rm_rf_at (fixture->working_dfd, ".", NULL, NULL);

  close (fixture->working_dfd);
  fixture->working_dfd = -1;

  /* The repo also needs its source files to be removed. This is the inverse
   * of setup_test_repository() in libtest.sh. */
  int parent_repo_dfd = ostree_repo_get_dfd (fixture->parent_repo);
  glnx_shutil_rm_rf_at (parent_repo_dfd, "../files", NULL, NULL);
  glnx_shutil_rm_rf_at (parent_repo_dfd, "../repo", NULL, NULL);

  g_clear_object (&fixture->working_dir);
  g_clear_object (&fixture->parent_repo);
}

/* Test the object constructor works at a basic level. */
static void
test_repo_finder_config_init (void)
{
  g_autoptr(OstreeRepoFinderConfig) finder = NULL;

  /* Default everything. */
  finder = ostree_repo_finder_config_new ();
}

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
  const OstreeCollectionRef ref1 = { "org.example.Os", "exampleos/x86_64/standard" };
  const OstreeCollectionRef ref2 = { "org.example.Os", "exampleos/x86_64/buildmaster/standard" };
  const OstreeCollectionRef * const refs[] = { &ref1, &ref2, NULL };

  context = g_main_context_new ();
  g_main_context_push_thread_default (context);

  finder = ostree_repo_finder_config_new ();

  ostree_repo_finder_resolve_async (OSTREE_REPO_FINDER (finder), refs,
                                    fixture->parent_repo, NULL, result_cb, &result);

  while (result == NULL)
    g_main_context_iteration (context, TRUE);

  results = ostree_repo_finder_resolve_finish (OSTREE_REPO_FINDER (finder),
                                               result, &error);
  g_assert_no_error (error);
  g_assert_nonnull (results);
  g_assert_cmpuint (results->len, ==, 0);

  g_main_context_pop_thread_default (context);
}

/* Add configuration for a remote named @remote_name, at @remote_uri, with a
 * remote collection ID of @collection_id, to the given @repo. */
static void
assert_create_remote_config (OstreeRepo  *repo,
                             const gchar *remote_name,
                             const gchar *remote_uri,
                             const gchar *collection_id)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) options = NULL;

  if (collection_id != NULL)
    options = g_variant_new_parsed ("@a{sv} { 'collection-id': <%s> }",
                                    collection_id);

  ostree_repo_remote_add (repo, remote_name, remote_uri, options, NULL, &error);
  g_assert_no_error (error);
}

static gchar *assert_create_remote (Fixture     *fixture,
                                    const gchar *collection_id,
                                    ...) G_GNUC_NULL_TERMINATED;

/* Create a new repository in a temporary directory with its collection ID set
 * to @collection_id, and containing the refs given in @... (which must be
 * %NULL-terminated). Return the `file://` URI of the new repository. */
static gchar *
assert_create_remote (Fixture     *fixture,
                      const gchar *collection_id,
                      ...)
{
  va_list args;
  g_autoptr(GError) error = NULL;
  const gchar *repo_name = (collection_id != NULL) ? collection_id : "no-collection";

  glnx_shutil_mkdir_p_at (fixture->working_dfd, repo_name, 0700, NULL, &error);
  g_assert_no_error (error);

  g_autoptr(GFile) repo_path = g_file_get_child (fixture->working_dir, repo_name);
  g_autoptr(OstreeRepo) repo = ostree_repo_new (repo_path);
  ostree_repo_set_collection_id (repo, collection_id, &error);
  g_assert_no_error (error);
  ostree_repo_create (repo, OSTREE_REPO_MODE_ARCHIVE_Z2, NULL, &error);
  g_assert_no_error (error);

  /* Set up the refs from @.... */
  va_start (args, collection_id);

  for (const gchar *ref_name = va_arg (args, const gchar *);
       ref_name != NULL;
       ref_name = va_arg (args, const gchar *))
    {
      OstreeCollectionRef collection_ref = { (gchar *) collection_id, (gchar *) ref_name };
      g_autofree gchar *checksum = NULL;
      g_autoptr(OstreeMutableTree) mtree = NULL;
      g_autoptr(OstreeRepoFile) repo_file = NULL;

      mtree = ostree_mutable_tree_new ();
      ostree_repo_write_dfd_to_mtree (repo, AT_FDCWD, ".", mtree, NULL, NULL, &error);
      g_assert_no_error (error);
      ostree_repo_write_mtree (repo, mtree, (GFile **) &repo_file, NULL, &error);
      g_assert_no_error (error);

      ostree_repo_write_commit (repo, NULL  /* no parent */, ref_name, ref_name,
                                NULL  /* no metadata */, repo_file, &checksum,
                                NULL, &error);
      g_assert_no_error (error);

      if (collection_id != NULL)
        ostree_repo_set_collection_ref_immediate (repo, &collection_ref, checksum, NULL, &error);
      else
        ostree_repo_set_ref_immediate (repo, NULL, ref_name, checksum, NULL, &error);
      g_assert_no_error (error);
    }

  va_end (args);

  /* Update the summary. */
  ostree_repo_regenerate_summary (repo, NULL  /* no metadata */, NULL, &error);
  g_assert_no_error (error);

  return g_file_get_uri (repo_path);
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
  const OstreeCollectionRef ref0 = { "org.example.Collection0", "exampleos/x86_64/ref0" };
  const OstreeCollectionRef ref1 = { "org.example.Collection0", "exampleos/x86_64/ref1" };
  const OstreeCollectionRef ref2 = { "org.example.Collection1", "exampleos/x86_64/ref1" };
  const OstreeCollectionRef ref3 = { "org.example.Collection1", "exampleos/x86_64/ref2" };
  const OstreeCollectionRef ref4 = { "org.example.Collection2", "exampleos/x86_64/ref3" };
  const OstreeCollectionRef * const refs[] = { &ref0, &ref1, &ref2, &ref3, &ref4, NULL };

  context = g_main_context_new ();
  g_main_context_push_thread_default (context);

  /* Put together various ref configuration files. */
  g_autofree gchar *collection0_uri = assert_create_remote (fixture, "org.example.Collection0",
                                                            "exampleos/x86_64/ref0",
                                                            "exampleos/x86_64/ref1",
                                                            NULL);
  g_autofree gchar *collection1_uri = assert_create_remote (fixture, "org.example.Collection1",
                                                            "exampleos/x86_64/ref2",
                                                            NULL);
  g_autofree gchar *no_collection_uri = assert_create_remote (fixture, NULL,
                                                              "exampleos/x86_64/ref3",
                                                              NULL);

  assert_create_remote_config (fixture->parent_repo, "remote0", collection0_uri, "org.example.Collection0");
  assert_create_remote_config (fixture->parent_repo, "remote1", collection1_uri, "org.example.Collection1");
  assert_create_remote_config (fixture->parent_repo, "remote0-copy", collection0_uri, "org.example.Collection0");
  assert_create_remote_config (fixture->parent_repo, "remote1-bad-copy", collection1_uri, "org.example.NotCollection1");
  assert_create_remote_config (fixture->parent_repo, "remote2", no_collection_uri, NULL);

  finder = ostree_repo_finder_config_new ();

  /* Resolve the refs. */
  ostree_repo_finder_resolve_async (OSTREE_REPO_FINDER (finder), refs,
                                    fixture->parent_repo, NULL, result_cb, &result);

  while (result == NULL)
    g_main_context_iteration (context, TRUE);

  results = ostree_repo_finder_resolve_finish (OSTREE_REPO_FINDER (finder),
                                               result, &error);
  g_assert_no_error (error);
  g_assert_nonnull (results);
  g_assert_cmpuint (results->len, ==, 3);

  /* Check that the results are correct: the invalid refs should have been
   * ignored, and the valid results canonicalised and deduplicated. */
  for (i = 0; i < results->len; i++)
    {
      const OstreeRepoFinderResult *result = g_ptr_array_index (results, i);

      if (g_strcmp0 (ostree_remote_get_name (result->remote), "remote0") == 0 ||
          g_strcmp0 (ostree_remote_get_name (result->remote), "remote0-copy") == 0)
        {
          g_assert_cmpuint (g_hash_table_size (result->ref_to_checksum), ==, 2);
          g_assert_true (g_hash_table_contains (result->ref_to_checksum, &ref0));
          g_assert_true (g_hash_table_contains (result->ref_to_checksum, &ref1));
        }
      else if (g_strcmp0 (ostree_remote_get_name (result->remote), "remote1") == 0)
        {
          g_assert_cmpuint (g_hash_table_size (result->ref_to_checksum), ==, 1);
          g_assert_true (g_hash_table_contains (result->ref_to_checksum, &ref3));
        }
      else
        {
          g_assert_not_reached ();
        }
    }

  g_main_context_pop_thread_default (context);
}

int main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/repo-finder-config/init", test_repo_finder_config_init);
  g_test_add ("/repo-finder-config/no-configs", Fixture, NULL, setup,
              test_repo_finder_config_no_configs, teardown);
  g_test_add ("/repo-finder-config/mixed-configs", Fixture, NULL, setup,
              test_repo_finder_config_mixed_configs, teardown);

  return g_test_run();
}
