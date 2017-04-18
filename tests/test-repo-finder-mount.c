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
#include <locale.h>

#include "libostreetest.h"
#include "ostree-autocleanups.h"
#include "ostree-remote-private.h"
#include "ostree-repo-finder.h"
#include "ostree-repo-finder-mount.h"
#include "ostree-types.h"
#include "test-mock-gio.h"

/* Test fixture. Creates a temporary directory and repository. */
typedef struct
{
  GFile *tmp_dir;
  OstreeRepo *parent_repo;
} Fixture;

static void
setup (Fixture       *fixture,
       gconstpointer  test_data)
{
  g_autofree gchar *tmp_dir_path = NULL;
  g_autoptr(GError) error = NULL;

  tmp_dir_path = g_dir_make_tmp ("test-repo-finder-mount-XXXXXX", &error);
  g_assert_no_error (error);

  fixture->tmp_dir = g_file_new_for_path (tmp_dir_path);

  fixture->parent_repo = ot_test_setup_repo (NULL, &error);
  g_assert_no_error (error);
}

/* TODO: Use glnx_shutil_rm_rf_at(). */
/* Recursively delete @file and its children. @file may be a file or a directory. */
static gboolean
rm_rf (GFile         *file,
       GCancellable  *cancellable,
       GError       **error)
{
  g_autoptr(GFileEnumerator) enumerator = NULL;

  enumerator = g_file_enumerate_children (file, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, NULL);

  while (enumerator != NULL)
    {
      GFile *child;

      if (!g_file_enumerator_iterate (enumerator, NULL, &child, cancellable, error))
        return FALSE;
      if (child == NULL)
        break;
      if (!rm_rf (child, cancellable, error))
        return FALSE;
    }

  return g_file_delete (file, cancellable, error);
}

static void
teardown (Fixture       *fixture,
          gconstpointer  test_data)
{
  g_autoptr(GFile) repo_parent_path = NULL;

  /* Recursively remove the temporary directory and repo. */
  rm_rf (fixture->tmp_dir, NULL, NULL);
  g_clear_object (&fixture->tmp_dir);

  /* The repo also needs its source files to be removed. This is the inverse
   * of setup_test_repository() in libtest.sh. */
  repo_parent_path = g_file_get_parent (ostree_repo_get_path (fixture->parent_repo));
  g_autoptr(GFile) files_path = g_file_get_child (repo_parent_path, "files");
  g_autoptr(GFile) repo_path = g_file_get_child (repo_parent_path, "repo");

  rm_rf (files_path, NULL, NULL);
  rm_rf (repo_path, NULL, NULL);
  g_clear_object (&fixture->parent_repo);
}

/* Test the object constructor works at a basic level. */
static void
test_repo_finder_mount_init (void)
{
  g_autoptr(OstreeRepoFinderMount) finder = NULL;
  g_autoptr(GVolumeMonitor) monitor = NULL;

  /* Default #GVolumeMonitor. */
  finder = ostree_repo_finder_mount_new (NULL);
  g_clear_object (&finder);

  /* Explicit #GVolumeMonitor. */
  monitor = ostree_mock_volume_monitor_new (NULL, NULL);
  finder = ostree_repo_finder_mount_new (monitor);
  g_clear_object (&finder);
}

static void
result_cb (GObject      *source_object,
           GAsyncResult *result,
           gpointer      user_data)
{
  GAsyncResult **result_out = user_data;
  *result_out = g_object_ref (result);
}

/* Test that no remotes are found if the #GVolumeMonitor returns no mounts. */
static void
test_repo_finder_mount_no_mounts (Fixture       *fixture,
                                  gconstpointer  test_data)
{
  g_autoptr(OstreeRepoFinderMount) finder = NULL;
  g_autoptr(GVolumeMonitor) monitor = NULL;
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

  monitor = ostree_mock_volume_monitor_new (NULL, NULL);
  finder = ostree_repo_finder_mount_new (monitor);

  ostree_repo_finder_resolve_async (OSTREE_REPO_FINDER (finder), refs,
                                    fixture->parent_repo,
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

/* Create a .ostree/repos directory under the given @mount_root, or abort. */
static GFile *
assert_create_repos_dir (GFile *mount_root)
{
  g_autoptr(GFile) ostree = NULL, repos = NULL;
  g_autoptr(GError) error = NULL;

  ostree = g_file_get_child (mount_root, ".ostree");
  repos = g_file_get_child (ostree, "repos");
  g_file_make_directory_with_parents (repos, NULL, &error);
  g_assert_no_error (error);

  return g_steal_pointer (&repos);
}

/* Create a @ref_name directory under the given @repos_dir, or abort. */
static GFile *
assert_create_repo_dir (GFile       *repos_dir,
                        const gchar *ref_name)
{
  g_autoptr(GFile) repo_dir = NULL;
  g_autoptr(GError) error = NULL;

  repo_dir = g_file_get_child (repos_dir, ref_name);
  g_file_make_directory_with_parents (repo_dir, NULL, &error);
  g_assert_no_error (error);

  return g_steal_pointer (&repo_dir);
}

/* Create a @ref_name symlink under the given @repos_dir, pointing to
 * @symlink_target, or abort. */
static GFile *
assert_create_repo_symlink (GFile       *repos_dir,
                            const gchar *ref_name,
                            GFile       *symlink_target)
{
  g_autoptr(GFile) repo_dir = NULL;
  g_autoptr(GFile) repo_parent_dir = NULL;
  g_autofree gchar *symlink_target_path = NULL;
  g_autoptr(GError) error = NULL;

  /* The @repo_parent_dir is not necessarily @repos_dir, since @ref_name may
   * contain slashes. */
  repo_dir = g_file_get_child (repos_dir, ref_name);
  repo_parent_dir = g_file_get_parent (repo_dir);
  symlink_target_path = g_file_get_path (symlink_target);
  g_file_make_directory_with_parents (repo_parent_dir, NULL, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    g_clear_error (&error);
  g_assert_no_error (error);
  g_file_make_symbolic_link (repo_dir, symlink_target_path, NULL, &error);
  g_assert_no_error (error);

  return g_object_ref (symlink_target);
}

/* Test resolving the refs against a collection of mock volumes, some of which
 * are mounted, some of which are removable, some of which contain valid or
 * invalid repo information on the file system, etc. */
static void
test_repo_finder_mount_mixed_mounts (Fixture       *fixture,
                                     gconstpointer  test_data)
{
  /* TODO: Simplify this mess. */
  g_autoptr(OstreeRepoFinderMount) finder = NULL;
  g_autoptr(GVolumeMonitor) monitor = NULL;
  g_autoptr(GMainContext) context = NULL;
  g_autoptr(GAsyncResult) result = NULL;
  g_autoptr(GPtrArray) results = NULL;  /* (element-type OstreeRepoFinderResult) */
  g_autoptr(GError) error = NULL;
  g_autoptr(GList) mounts = NULL;  /* (element-type OstreeMockMount)  */
  g_autoptr(OstreeMockMount) non_removable_mount = NULL;
  g_autoptr(OstreeMockMount) no_repos_mount = NULL;
  g_autoptr(OstreeMockMount) repo1_mount = NULL;
  g_autoptr(OstreeMockMount) repo2_mount = NULL;
  g_autoptr(GFile) non_removable_root = NULL;
  g_autoptr(GFile) no_repos_root = NULL;
  g_autoptr(GFile) no_repos_repos = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) repo1_root = NULL;
  g_autoptr(GFile) repo2_root = NULL;
  g_autoptr(GFile) repo1_repos = NULL;
  g_autoptr(GFile) repo2_repos = NULL;
  g_autoptr(GFile) repo1_repo_a = NULL, repo1_repo_b = NULL, repo1_repo_c = NULL;
  g_autoptr(GFile) repo2_repo_a = NULL, repo2_repo_b = NULL;
  g_autoptr(GFile) repo2_repo_c = NULL, repo2_repo_d = NULL;
  g_autofree gchar *repo1_repo_a_uri = NULL, *repo1_repo_b_uri = NULL;
  g_autofree gchar *repo2_repo_a_uri = NULL;
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

  /* Build the various mock drives/volumes/mounts, and some repositories with
   * refs within them. We use @root under the assumption that it’s on a separate
   * file system from /tmp, so it’s an example of a symlink pointing outside
   * its mount point.
   * FIXME: Need to add the refs and checksums in or the test will fail. */
  root = g_file_new_for_path ("/");
  non_removable_root = g_file_get_child (fixture->tmp_dir, "non-removable");
  non_removable_mount = ostree_mock_mount_new ("non-removable", non_removable_root);

  no_repos_root = g_file_get_child (fixture->tmp_dir, "no-repos");
  no_repos_repos = assert_create_repos_dir (no_repos_root);
  no_repos_mount = ostree_mock_mount_new ("no-repos", no_repos_root);

  repo1_root = g_file_get_child (fixture->tmp_dir, "repo1");
  repo1_repos = assert_create_repos_dir (repo1_root);
  repo1_repo_a = assert_create_repo_dir (repo1_repos, refs[0]);
  repo1_repo_b = assert_create_repo_dir (repo1_repos, refs[1]);
  repo1_repo_c = assert_create_repo_symlink (repo1_repos, refs[2], repo1_repo_a);

  repo1_mount = ostree_mock_mount_new ("repo1", repo1_root);

  repo2_root = g_file_get_child (fixture->tmp_dir, "repo2");
  repo2_repos = assert_create_repos_dir (repo2_root);
  repo2_mount = ostree_mock_mount_new ("repo2", repo2_root);
  repo2_repo_a = assert_create_repo_dir (repo2_repos, refs[0]);
  repo2_repo_b = assert_create_repo_symlink (repo2_repos, refs[1], repo2_repo_a);
  repo2_repo_c = assert_create_repo_symlink (repo2_repos, refs[2], repo2_repo_b);
  repo2_repo_d = assert_create_repo_symlink (repo2_repos, refs[3], root);

  repo1_repo_a_uri = g_file_get_uri (repo1_repo_a);
  repo1_repo_b_uri = g_file_get_uri (repo1_repo_b);
  repo2_repo_a_uri = g_file_get_uri (repo2_repo_a);

  mounts = g_list_prepend (mounts, non_removable_mount);
  mounts = g_list_prepend (mounts, no_repos_mount);
  mounts = g_list_prepend (mounts, repo1_mount);
  mounts = g_list_prepend (mounts, repo2_mount);

  monitor = ostree_mock_volume_monitor_new (mounts, NULL);
  finder = ostree_repo_finder_mount_new (monitor);

  /* Resolve the refs. */
  ostree_repo_finder_resolve_async (OSTREE_REPO_FINDER (finder), refs,
                                    fixture->parent_repo,
                                    NULL, result_cb, &result);

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
      g_autofree gchar *uri = NULL;
      const OstreeRepoFinderResult *result = g_ptr_array_index (results, i);

      uri = g_key_file_get_string (result->remote->options, result->remote->group, "url", &error);
      g_assert_no_error (error);

      /* TODO: Check the checksums too */
      if (g_strcmp0 (result->remote->name, repo1_repo_a_uri) == 0)
        {
          g_assert_cmpstr (uri, ==, repo1_repo_a_uri);
          g_assert_cmpuint (g_hash_table_size (result->ref_to_checksum), ==, 2);
          g_assert_true (g_hash_table_contains (result->ref_to_checksum, refs[0]));
          g_assert_true (g_hash_table_contains (result->ref_to_checksum, refs[2]));
        }
      else if (g_strcmp0 (result->remote->name, repo1_repo_b_uri) == 0)
        {
          g_assert_cmpstr (uri, ==, repo1_repo_b_uri);
          g_assert_cmpuint (g_hash_table_size (result->ref_to_checksum), ==, 1);
          g_assert_true (g_hash_table_contains (result->ref_to_checksum, refs[1]));
        }
      else if (g_strcmp0 (result->remote->name, repo2_repo_a_uri) == 0)
        {
          g_assert_cmpstr (uri, ==, repo2_repo_a_uri);
          g_assert_cmpuint (g_hash_table_size (result->ref_to_checksum), ==, 3);
          g_assert_true (g_hash_table_contains (result->ref_to_checksum, refs[0]));
          g_assert_true (g_hash_table_contains (result->ref_to_checksum, refs[1]));
          g_assert_true (g_hash_table_contains (result->ref_to_checksum, refs[2]));
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

  g_test_add_func ("/repo-finder-mount/init", test_repo_finder_mount_init);
  g_test_add ("/repo-finder-mount/no-mounts", Fixture, NULL, setup,
              test_repo_finder_mount_no_mounts, teardown);
  g_test_add ("/repo-finder-mount/mixed-mounts", Fixture, NULL, setup,
              test_repo_finder_mount_mixed_mounts, teardown);

  return g_test_run();
}
