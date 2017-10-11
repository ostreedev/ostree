/*
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
  OstreeRepo *parent_repo;
  GLnxTmpDir tmpdir; /* owned */
  GFile *working_dir; /* Points at tmpdir */
} Fixture;

static void
setup (Fixture       *fixture,
       gconstpointer  test_data)
{
  g_autoptr(GError) error = NULL;

  (void)glnx_mkdtemp ("test-repo-finder-mount-XXXXXX", 0700, &fixture->tmpdir, &error);
  g_assert_no_error (error);

  g_test_message ("Using temporary directory: %s", fixture->tmpdir.path);

  glnx_shutil_mkdir_p_at (fixture->tmpdir.fd, "repo", 0700, NULL, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    g_clear_error (&error);
  g_assert_no_error (error);

  fixture->working_dir = g_file_new_for_path (fixture->tmpdir.path);

  fixture->parent_repo = ot_test_setup_repo (NULL, &error);
  g_assert_no_error (error);
}

static void
teardown (Fixture       *fixture,
          gconstpointer  test_data)
{
  /* Recursively remove the temporary directory. */
  (void)glnx_tmpdir_delete (&fixture->tmpdir, NULL, NULL);

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
  const OstreeCollectionRef ref1 = { "org.example.Collection1", "exampleos/x86_64/standard" };
  const OstreeCollectionRef ref2 = { "org.example.Collection1", "exampleos/x86_64/buildmaster/standard" };
  const OstreeCollectionRef ref3 = { "org.example.Collection2", "exampleos/x86_64/standard" };
  const OstreeCollectionRef ref4 = { "org.example.Collection2", "exampleos/arm64/standard" };
  const OstreeCollectionRef * const refs[] = { &ref1, &ref2, &ref3, &ref4, NULL };

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

/* Create a .ostree/repos.d directory under the given @mount_root, or abort. */
static gboolean
assert_create_repos_dir (Fixture      *fixture,
                         const gchar  *mount_root_name,
                         int          *out_repos_dfd,
                         GMount      **out_mount)
{
  glnx_autofd int repos_dfd = -1;
  g_autoptr(GError) error = NULL;

  g_autofree gchar *path = g_build_filename (mount_root_name, ".ostree", "repos.d", NULL);
  glnx_shutil_mkdir_p_at_open (fixture->tmpdir.fd, path, 0700, &repos_dfd, NULL, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    g_clear_error (&error);
  g_assert_no_error (error);

  *out_repos_dfd = glnx_steal_fd (&repos_dfd);
  g_autoptr(GFile) mount_root = g_file_get_child (fixture->working_dir, mount_root_name);
  *out_mount = G_MOUNT (ostree_mock_mount_new (mount_root_name, mount_root));

  return TRUE;
}

/* Create a new repository in @repo_dir with its collection ID unset, and
 * containing the refs given in @... (which must be %NULL-terminated). Each
 * #OstreeCollectionRef in @... is followed by a gchar** return address for the
 * checksum committed for that ref. Return the new repository. */
static OstreeRepo *
assert_create_remote_va (Fixture *fixture,
                         GFile   *repo_dir,
                         va_list  args)
{
  g_autoptr(GError) error = NULL;

  g_autoptr(OstreeRepo) repo = ostree_repo_new (repo_dir);
  ostree_repo_create (repo, OSTREE_REPO_MODE_ARCHIVE, NULL, &error);
  g_assert_no_error (error);

  /* Set up the refs from @.... */
  for (const OstreeCollectionRef *ref = va_arg (args, const OstreeCollectionRef *);
       ref != NULL;
       ref = va_arg (args, const OstreeCollectionRef *))
    {
      g_autofree gchar *checksum = NULL;
      g_autoptr(OstreeMutableTree) mtree = NULL;
      g_autoptr(OstreeRepoFile) repo_file = NULL;
      gchar **out_checksum = va_arg (args, gchar **);

      mtree = ostree_mutable_tree_new ();
      ostree_repo_write_dfd_to_mtree (repo, AT_FDCWD, ".", mtree, NULL, NULL, &error);
      g_assert_no_error (error);
      ostree_repo_write_mtree (repo, mtree, (GFile **) &repo_file, NULL, &error);
      g_assert_no_error (error);

      ostree_repo_write_commit (repo, NULL  /* no parent */, ref->ref_name, ref->ref_name,
                                NULL  /* no metadata */, repo_file, &checksum,
                                NULL, &error);
      g_assert_no_error (error);

      if (ref->collection_id != NULL)
        ostree_repo_set_collection_ref_immediate (repo, ref, checksum, NULL, &error);
      else
        ostree_repo_set_ref_immediate (repo, NULL, ref->ref_name, checksum, NULL, &error);
      g_assert_no_error (error);

      if (out_checksum != NULL)
        *out_checksum = g_steal_pointer (&checksum);
    }

  /* Update the summary. */
  ostree_repo_regenerate_summary (repo, NULL  /* no metadata */, NULL, &error);
  g_assert_no_error (error);

  return g_steal_pointer (&repo);
}

static OstreeRepo *
assert_create_repo_dir (Fixture     *fixture,
                        int          repos_dfd,
                        GMount      *repos_mount,
                        const char  *repo_name,
                        gchar      **out_uri,
                        ...) G_GNUC_NULL_TERMINATED;

/* Create a @repo_name directory under the given @repos_dfd, or abort. Create a
 * new repository in it with the refs given in @..., as per
 * assert_create_remote_va(). Return the URI of the repository. */
static OstreeRepo *
assert_create_repo_dir (Fixture     *fixture,
                        int          repos_dfd,
                        GMount      *repos_mount,
                        const char  *repo_name,
                        gchar      **out_uri,
                        ...)
{
  glnx_autofd int ref_dfd = -1;
  g_autoptr(OstreeRepo) repo = NULL;
  g_autoptr(GError) error = NULL;
  va_list args;

  glnx_shutil_mkdir_p_at_open (repos_dfd, repo_name, 0700, &ref_dfd, NULL, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    g_clear_error (&error);
  g_assert_no_error (error);

  g_autoptr(GFile) mount_root = g_mount_get_root (repos_mount);
  g_autoptr(GFile) repos_dir = g_file_get_child (mount_root, ".ostree/repos.d");
  g_autoptr(GFile) repo_dir = g_file_get_child (repos_dir, repo_name);

  va_start (args, out_uri);
  repo = assert_create_remote_va (fixture, repo_dir, args);
  va_end (args);

  *out_uri = g_file_get_uri (repo_dir);

  return g_steal_pointer (&repo);
}

/* Create a @repo_name symlink under the given @repos_dfd, pointing to
 * @symlink_target_path, or abort. */
static void
assert_create_repo_symlink (int         repos_dfd,
                            const char *repo_name,
                            const char *symlink_target_path)
{
  if (TEMP_FAILURE_RETRY (symlinkat (symlink_target_path, repos_dfd, repo_name)) != 0)
    {
      g_autoptr(GError) error = NULL;
      glnx_throw_errno_prefix (&error, "symlinkat");
      g_assert_no_error (error);
    }
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

/* Test resolving the refs against a collection of mock volumes, some of which
 * are mounted, some of which are removable, some of which contain valid or
 * invalid repo information on the file system, etc. */
static void
test_repo_finder_mount_mixed_mounts (Fixture       *fixture,
                                     gconstpointer  test_data)
{
  g_autoptr(OstreeRepoFinderMount) finder = NULL;
  g_autoptr(GVolumeMonitor) monitor = NULL;
  g_autoptr(GMainContext) context = NULL;
  g_autoptr(GAsyncResult) result = NULL;
  g_autoptr(GPtrArray) results = NULL;  /* (element-type OstreeRepoFinderResult) */
  g_autoptr(GError) error = NULL;
  g_autoptr(GList) mounts = NULL;  /* (element-type OstreeMockMount)  */
  g_autoptr(GMount) non_removable_mount = NULL;
  g_autoptr(GMount) no_repos_mount = NULL;
  g_autoptr(GMount) repo1_mount = NULL;
  g_autoptr(GMount) repo2_mount = NULL;
  g_autoptr(GFile) non_removable_root = NULL;
  glnx_autofd int no_repos_repos = -1;
  glnx_autofd int repo1_repos = -1;
  glnx_autofd int repo2_repos = -1;
  g_autoptr(OstreeRepo) repo1_repo_a = NULL, repo1_repo_b = NULL;
  g_autoptr(OstreeRepo) repo2_repo_a = NULL;
  g_autofree gchar *repo1_repo_a_uri = NULL, *repo1_repo_b_uri = NULL;
  g_autofree gchar *repo2_repo_a_uri = NULL;
  g_autofree gchar *repo1_ref0_checksum = NULL, *repo1_ref1_checksum = NULL, *repo1_ref2_checksum = NULL;
  g_autofree gchar *repo2_ref0_checksum = NULL, *repo2_ref1_checksum = NULL, *repo2_ref2_checksum = NULL;
  g_autofree gchar *repo1_ref5_checksum = NULL, *repo2_ref3_checksum = NULL;
  gsize i;
  const OstreeCollectionRef ref0 = { "org.example.Collection1", "exampleos/x86_64/ref0" };
  const OstreeCollectionRef ref1 = { "org.example.Collection1", "exampleos/x86_64/ref1" };
  const OstreeCollectionRef ref2 = { "org.example.Collection1", "exampleos/x86_64/ref2" };
  const OstreeCollectionRef ref3 = { "org.example.Collection1", "exampleos/x86_64/ref3" };
  const OstreeCollectionRef ref4 = { "org.example.UnconfiguredCollection", "exampleos/x86_64/ref4" };
  const OstreeCollectionRef ref5 = { "org.example.Collection3", "exampleos/x86_64/ref0" };
  const OstreeCollectionRef * const refs[] = { &ref0, &ref1, &ref2, &ref3, &ref4, &ref5, NULL };

  context = g_main_context_new ();
  g_main_context_push_thread_default (context);

  /* Build the various mock drives/volumes/mounts, and some repositories with
   * refs within them. We use "/" under the assumption that it’s on a separate
   * file system from /tmp, so it’s an example of a symlink pointing outside
   * its mount point. */
  non_removable_root = g_file_get_child (fixture->working_dir, "non-removable-mount");
  non_removable_mount = G_MOUNT (ostree_mock_mount_new ("non-removable", non_removable_root));

  assert_create_repos_dir (fixture, "no-repos-mount", &no_repos_repos, &no_repos_mount);

  assert_create_repos_dir (fixture, "repo1-mount", &repo1_repos, &repo1_mount);
  repo1_repo_a = assert_create_repo_dir (fixture, repo1_repos, repo1_mount, "repo1-repo-a", &repo1_repo_a_uri,
                                         refs[0], &repo1_ref0_checksum,
                                         refs[2], &repo1_ref2_checksum,
                                         refs[5], &repo1_ref5_checksum,
                                         NULL);
  repo1_repo_b = assert_create_repo_dir (fixture, repo1_repos, repo1_mount, "repo1-repo-b", &repo1_repo_b_uri,
                                         refs[1], &repo1_ref1_checksum,
                                         NULL);
  assert_create_repo_symlink (repo1_repos, "repo1-repo-a-alias", "repo1-repo-a");

  assert_create_repos_dir (fixture, "repo2-mount", &repo2_repos, &repo2_mount);
  repo2_repo_a = assert_create_repo_dir (fixture, repo2_repos, repo2_mount, "repo2-repo-a", &repo2_repo_a_uri,
                                         refs[0], &repo2_ref0_checksum,
                                         refs[1], &repo2_ref1_checksum,
                                         refs[2], &repo2_ref2_checksum,
                                         refs[3], &repo2_ref3_checksum,
                                         NULL);
  assert_create_repo_symlink (repo2_repos, "repo2-repo-a-alias", "repo2-repo-a");
  assert_create_repo_symlink (repo2_repos, "dangling-symlink", "repo2-repo-b");
  assert_create_repo_symlink (repo2_repos, "root", "/");

  mounts = g_list_prepend (mounts, non_removable_mount);
  mounts = g_list_prepend (mounts, no_repos_mount);
  mounts = g_list_prepend (mounts, repo1_mount);
  mounts = g_list_prepend (mounts, repo2_mount);

  monitor = ostree_mock_volume_monitor_new (mounts, NULL);
  finder = ostree_repo_finder_mount_new (monitor);

  assert_create_remote_config (fixture->parent_repo, "remote1", "https://nope1", "org.example.Collection1");
  assert_create_remote_config (fixture->parent_repo, "remote2", "https://nope2", "org.example.Collection2");
  /* don’t configure org.example.UnconfiguredCollection */
  assert_create_remote_config (fixture->parent_repo, "remote3", "https://nope3", "org.example.Collection3");

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
  g_assert_cmpuint (results->len, ==, 4);

  /* Check that the results are correct: the invalid refs should have been
   * ignored, and the valid results canonicalised and deduplicated. */
  for (i = 0; i < results->len; i++)
    {
      g_autofree gchar *uri = NULL;
      const gchar *keyring;
      const OstreeRepoFinderResult *result = g_ptr_array_index (results, i);

      uri = g_key_file_get_string (result->remote->options, result->remote->group, "url", &error);
      g_assert_no_error (error);
      keyring = result->remote->keyring;

      if (g_strcmp0 (uri, repo1_repo_a_uri) == 0 &&
          g_strcmp0 (keyring, "remote1.trustedkeys.gpg") == 0)
        {
          g_assert_cmpuint (g_hash_table_size (result->ref_to_checksum), ==, 2);
          g_assert_cmpstr (g_hash_table_lookup (result->ref_to_checksum, refs[0]), ==, repo1_ref0_checksum);
          g_assert_cmpstr (g_hash_table_lookup (result->ref_to_checksum, refs[2]), ==, repo1_ref2_checksum);
        }
      else if (g_strcmp0 (uri, repo1_repo_a_uri) == 0 &&
          g_strcmp0 (keyring, "remote3.trustedkeys.gpg") == 0)
        {
          g_assert_cmpuint (g_hash_table_size (result->ref_to_checksum), ==, 1);
          g_assert_cmpstr (g_hash_table_lookup (result->ref_to_checksum, refs[5]), ==, repo1_ref5_checksum);
        }
      else if (g_strcmp0 (uri, repo1_repo_b_uri) == 0 &&
               g_strcmp0 (keyring, "remote1.trustedkeys.gpg") == 0)
        {
          g_assert_cmpuint (g_hash_table_size (result->ref_to_checksum), ==, 1);
          g_assert_cmpstr (g_hash_table_lookup (result->ref_to_checksum, refs[1]), ==, repo1_ref1_checksum);
        }
      else if (g_strcmp0 (uri, repo2_repo_a_uri) == 0 &&
               g_strcmp0 (keyring, "remote1.trustedkeys.gpg") == 0)
        {
          g_assert_cmpuint (g_hash_table_size (result->ref_to_checksum), ==, 4);
          g_assert_cmpstr (g_hash_table_lookup (result->ref_to_checksum, refs[0]), ==, repo2_ref0_checksum);
          g_assert_cmpstr (g_hash_table_lookup (result->ref_to_checksum, refs[1]), ==, repo2_ref1_checksum);
          g_assert_cmpstr (g_hash_table_lookup (result->ref_to_checksum, refs[2]), ==, repo2_ref2_checksum);
          g_assert_cmpstr (g_hash_table_lookup (result->ref_to_checksum, refs[3]), ==, repo2_ref3_checksum);
        }
      else
        {
          g_test_message ("Unknown result ‘%s’ with keyring ‘%s’.",
                          result->remote->name, result->remote->keyring);
          g_assert_not_reached ();
        }
    }

  g_main_context_pop_thread_default (context);
}

/* Test resolving the refs against a mock volume which contains two repositories
 * in the default repository paths ostree/repo and .ostree/repo, to check that
 * those paths are read */
static void
test_repo_finder_mount_well_known (Fixture       *fixture,
                                   gconstpointer  test_data)
{
  g_autoptr(OstreeRepoFinderMount) finder = NULL;
  g_autoptr(GVolumeMonitor) monitor = NULL;
  g_autoptr(GMainContext) context = NULL;
  g_autoptr(GAsyncResult) result = NULL;
  g_autoptr(GPtrArray) results = NULL;  /* (element-type OstreeRepoFinderResult) */
  g_autoptr(GError) error = NULL;
  g_autoptr(GList) mounts = NULL;  /* (element-type OstreeMockMount)  */
  g_autoptr(GMount) mount = NULL;
  glnx_autofd int repos = -1;
  g_autoptr(OstreeRepo) repo_a = NULL, repo_b = NULL;
  g_autofree gchar *repo_a_uri = NULL, *repo_b_uri = NULL;
  g_autofree gchar *ref_a_checksum = NULL, *ref_b_checksum = NULL;
  gsize i;
  const OstreeCollectionRef ref_a = { "org.example.Collection1", "refA" };
  const OstreeCollectionRef ref_b = { "org.example.Collection2", "refB" };
  const OstreeCollectionRef * const refs[] = { &ref_a, &ref_b, NULL };

  context = g_main_context_new ();
  g_main_context_push_thread_default (context);

  /* Build the various mock drives/volumes/mounts, and some repositories with
   * refs within them. We use "/" under the assumption that it’s on a separate
   * file system from /tmp, so it’s an example of a symlink pointing outside
   * its mount point. */
  assert_create_repos_dir (fixture, "mount", &repos, &mount);
  repo_a = assert_create_repo_dir (fixture, repos, mount, "../../ostree/repo", &repo_a_uri,
                                   &ref_a, &ref_a_checksum,
                                   NULL);
  repo_b = assert_create_repo_dir (fixture, repos, mount, "../../.ostree/repo", &repo_b_uri,
                                   &ref_b, &ref_b_checksum,
                                   NULL);
  assert_create_repo_symlink (repos, "repo-a-alias", "../../ostree/repo");

  mounts = g_list_prepend (mounts, mount);

  monitor = ostree_mock_volume_monitor_new (mounts, NULL);
  finder = ostree_repo_finder_mount_new (monitor);

  assert_create_remote_config (fixture->parent_repo, "remote1", "https://nope1", "org.example.Collection1");
  assert_create_remote_config (fixture->parent_repo, "remote2", "https://nope2", "org.example.Collection2");

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
  g_assert_cmpuint (results->len, ==, 2);

  /* Check that the results are correct: the valid results canonicalised and
   * deduplicated. */
  for (i = 0; i < results->len; i++)
    {
      g_autofree gchar *uri = NULL;
      const gchar *keyring;
      const OstreeRepoFinderResult *result = g_ptr_array_index (results, i);

      uri = g_key_file_get_string (result->remote->options, result->remote->group, "url", &error);
      g_assert_no_error (error);
      keyring = result->remote->keyring;

      if (g_strcmp0 (uri, repo_a_uri) == 0 &&
          g_strcmp0 (keyring, "remote1.trustedkeys.gpg") == 0)
        {
          g_assert_cmpuint (g_hash_table_size (result->ref_to_checksum), ==, 1);
          g_assert_cmpstr (g_hash_table_lookup (result->ref_to_checksum, &ref_a), ==, ref_a_checksum);
        }
      else if (g_strcmp0 (uri, repo_b_uri) == 0 &&
          g_strcmp0 (keyring, "remote2.trustedkeys.gpg") == 0)
        {
          g_assert_cmpuint (g_hash_table_size (result->ref_to_checksum), ==, 1);
          g_assert_cmpstr (g_hash_table_lookup (result->ref_to_checksum, &ref_b), ==, ref_b_checksum);
        }
      else
        {
          g_test_message ("Unknown result ‘%s’ with keyring ‘%s’.",
                          result->remote->name, result->remote->keyring);
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
  g_test_add ("/repo-finder-mount/well-known", Fixture, NULL, setup,
              test_repo_finder_mount_well_known, teardown);

  return g_test_run();
}
