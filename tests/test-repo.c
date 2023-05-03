/*
 * Copyright © 2017 Endless Mobile, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 *
 * Authors:
 *  - Philip Withnall <withnall@endlessm.com>
 */

#include "config.h"

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>
#include <libglnx.h>
#include <locale.h>

#include "ostree-autocleanups.h"
#include "ostree-types.h"

/* Test fixture. Creates a temporary directory. */
typedef struct
{
  GLnxTmpDir tmpdir; /* (owned) */
} Fixture;

static void
setup (Fixture *fixture, gconstpointer test_data)
{
  g_autoptr (GError) error = NULL;

  (void)glnx_mkdtemp ("test-repo-XXXXXX", 0700, &fixture->tmpdir, &error);
  g_assert_no_error (error);

  g_test_message ("Using temporary directory: %s", fixture->tmpdir.path);
}

/* Common setup for locking tests. Create an archive repo in the tmpdir and
 * set the locking timeout to 0 so lock failures don't block.
 */
static void
lock_setup (Fixture *fixture, gconstpointer test_data)
{
  setup (fixture, test_data);

  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeRepo) repo = ostree_repo_create_at (
      fixture->tmpdir.fd, ".", OSTREE_REPO_MODE_ARCHIVE, NULL, NULL, &error);
  g_assert_no_error (error);

  /* Set the lock timeout to 0 so failures don't block the test */
  g_autoptr (GKeyFile) config = ostree_repo_copy_config (repo);
  g_key_file_set_integer (config, "core", "lock-timeout-secs", 0);
  ostree_repo_write_config (repo, config, &error);
  g_assert_no_error (error);
}

static void
teardown (Fixture *fixture, gconstpointer test_data)
{
  /* Recursively remove the temporary directory. */
  (void)glnx_tmpdir_delete (&fixture->tmpdir, NULL, NULL);
}

/* Test that the hash values for two #OstreeRepo instances pointing at the same
 * repository are equal. We can’t test anything else, since hash collisions are
 * always a possibility. */
static void
test_repo_hash (Fixture *fixture, gconstpointer test_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeRepo) repo1 = ostree_repo_create_at (
      fixture->tmpdir.fd, ".", OSTREE_REPO_MODE_ARCHIVE, NULL, NULL, &error);
  g_assert_no_error (error);

  g_autoptr (OstreeRepo) repo2 = ostree_repo_open_at (fixture->tmpdir.fd, ".", NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpuint (ostree_repo_hash (repo1), ==, ostree_repo_hash (repo2));
}

/* Test that trying to hash a closed repo results in an assertion failure. */
static void
test_repo_hash_closed (Fixture *fixture, gconstpointer test_data)
{
  if (g_test_subprocess ())
    {
      g_autoptr (GFile) repo_path = g_file_new_for_path (fixture->tmpdir.path);
      g_autoptr (OstreeRepo) repo = ostree_repo_new (repo_path);

      ostree_repo_hash (repo);

      return;
    }

  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*ERROR*ostree_repo_hash: assertion failed:*");
}

/* Test that various repositories test equal (or not) with each other. */
static void
test_repo_equal (Fixture *fixture, gconstpointer test_data)
{
  g_autoptr (GError) error = NULL;

  /* Create a few separate repos and some #OstreeRepo objects for them. */
  glnx_ensure_dir (fixture->tmpdir.fd, "repo1", 0755, &error);
  g_assert_no_error (error);
  glnx_ensure_dir (fixture->tmpdir.fd, "repo2", 0755, &error);
  g_assert_no_error (error);

  g_autoptr (OstreeRepo) repo1 = ostree_repo_create_at (
      fixture->tmpdir.fd, "repo1", OSTREE_REPO_MODE_ARCHIVE, NULL, NULL, &error);
  g_assert_no_error (error);

  g_autoptr (OstreeRepo) repo1_alias
      = ostree_repo_open_at (fixture->tmpdir.fd, "repo1", NULL, &error);
  g_assert_no_error (error);

  g_autoptr (OstreeRepo) repo2 = ostree_repo_create_at (
      fixture->tmpdir.fd, "repo2", OSTREE_REPO_MODE_ARCHIVE, NULL, NULL, &error);
  g_assert_no_error (error);

  g_autoptr (GFile) closed_repo_path = g_file_new_for_path (fixture->tmpdir.path);
  g_autoptr (OstreeRepo) closed_repo = ostree_repo_new (closed_repo_path);

  /* Test various equalities. */
  g_assert_true (ostree_repo_equal (repo1, repo1));
  g_assert_true (ostree_repo_equal (repo1_alias, repo1_alias));
  g_assert_true (ostree_repo_equal (repo1, repo1_alias));
  g_assert_true (ostree_repo_equal (repo1_alias, repo1));
  g_assert_true (ostree_repo_equal (repo2, repo2));
  g_assert_false (ostree_repo_equal (repo1, repo2));
  g_assert_false (ostree_repo_equal (repo1_alias, repo2));
  g_assert_false (ostree_repo_equal (repo2, repo1));
  g_assert_false (ostree_repo_equal (repo2, repo1_alias));
  g_assert_false (ostree_repo_equal (repo1, closed_repo));
  g_assert_false (ostree_repo_equal (repo1_alias, closed_repo));
  g_assert_false (ostree_repo_equal (closed_repo, repo1));
  g_assert_false (ostree_repo_equal (closed_repo, repo1_alias));
  g_assert_false (ostree_repo_equal (repo2, closed_repo));
  g_assert_false (ostree_repo_equal (closed_repo, repo2));
  g_assert_false (ostree_repo_equal (closed_repo, closed_repo));
}

static void
test_repo_get_min_free_space (Fixture *fixture, gconstpointer test_data)
{
  g_autoptr (GKeyFile) config = NULL;
  g_autoptr (GError) error = NULL;
  guint64 bytes = 0;
  typedef struct
  {
    const char *val;
    gboolean should_succeed;
  } min_free_space_value;

  g_autoptr (OstreeRepo) repo = ostree_repo_create_at (
      fixture->tmpdir.fd, ".", OSTREE_REPO_MODE_ARCHIVE, NULL, NULL, &error);
  g_assert_no_error (error);

  min_free_space_value values_to_test[]
      = { { "500MB", TRUE },
          { "0MB", TRUE },
          { "17179869185GB", FALSE }, /* Overflow parameter: bytes > G_MAXUINT64 */
          { NULL, FALSE } };

  config = ostree_repo_copy_config (repo);

  for (guint i = 0; values_to_test[i].val != NULL; i++)
    {
      g_key_file_remove_key (config, "core", "min-free-space-size", NULL);
      g_key_file_set_string (config, "core", "min-free-space-size", values_to_test[i].val);

      ostree_repo_write_config (repo, config, &error);
      g_assert_no_error (error);
      ostree_repo_reload_config (repo, NULL, &error);
      g_assert_no_error (error);

      ostree_repo_get_min_free_space_bytes (repo, &bytes, &error);
      if (values_to_test[i].should_succeed)
        g_assert_no_error (error);
      else
        continue;
    }
}

static void
test_write_regfile_api (Fixture *fixture, gconstpointer test_data)
{
  g_autoptr (GKeyFile) config = NULL;
  g_autoptr (GError) error = NULL;

  g_autoptr (OstreeRepo) repo = ostree_repo_create_at (
      fixture->tmpdir.fd, ".", OSTREE_REPO_MODE_ARCHIVE, NULL, NULL, &error);
  g_assert_no_error (error);

  g_auto (GVariantBuilder) xattrs_builder;
  g_variant_builder_init (&xattrs_builder, (GVariantType *)"a(ayay)");
  g_variant_builder_add (&xattrs_builder, "(^ay^ay)", "security.selinux",
                         "system_u:object_r:etc_t:s0");
  g_autoptr (GVariant) xattrs = g_variant_ref_sink (g_variant_builder_end (&xattrs_builder));

  // Current contents of /etc/networks in Fedora
  static const char contents[] = "default 0.0.0.0\nloopback 127.0.0.0\nlink-local 169.254.0.0\n";
  // First with no xattrs
  g_autofree char *checksum = ostree_repo_write_regfile_inline (
      repo, NULL, 0, 0, S_IFREG | 0644, NULL, (const guint8 *)contents, sizeof (contents) - 1, NULL,
      &error);
  g_assert_no_error (error);
  g_assert_cmpstr (checksum, ==,
                   "8aaa9dc13a0c5839fe4a277756798c609c53fac6fa2290314ecfef9041065873");
  g_clear_pointer (&checksum, g_free);

  // Invalid checksum
  checksum = ostree_repo_write_regfile_inline (
      repo, "3272139f889f6a7007b3d64adc74be9e2979bf6bbe663d1512e5bd43f4de24a1", 0, 0,
      S_IFREG | 0644, NULL, (const guint8 *)contents, sizeof (contents) - 1, NULL, &error);
  g_assert (checksum == NULL);
  g_assert (error != NULL);
  g_clear_error (&error);

  // Now with xattrs
  g_clear_pointer (&checksum, g_free);
  checksum = ostree_repo_write_regfile_inline (repo, NULL, 0, 0, S_IFREG | 0644, xattrs,
                                               (const guint8 *)contents, sizeof (contents) - 1,
                                               NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (checksum, ==,
                   "4f600d252338f93279c51c964915cb2c26f0d09082164c54890d1a3c78cdeb1e");
  g_clear_pointer (&checksum, g_free);

  // Test symlinks
  g_clear_pointer (&xattrs, g_variant_unref);
  g_variant_builder_init (&xattrs_builder, (GVariantType *)"a(ayay)");
  g_variant_builder_add (&xattrs_builder, "(^ay^ay)", "security.selinux",
                         "system_u:object_r:bin_t:s0");
  xattrs = g_variant_ref_sink (g_variant_builder_end (&xattrs_builder));

  g_clear_pointer (&checksum, g_free);
  checksum = ostree_repo_write_symlink (repo, NULL, 0, 0, xattrs, "bash", NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (checksum, ==,
                   "23a2e97d21d960ac7a4e39a8721b1baff7b213e00e5e5641334f50506012fcff");
}

/* Just a sanity check of the C autolocking API */
static void
test_repo_autolock (Fixture *fixture, gconstpointer test_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeRepo) repo = ostree_repo_create_at (
      fixture->tmpdir.fd, ".", OSTREE_REPO_MODE_ARCHIVE, NULL, NULL, &error);
  g_assert_no_error (error);

  {
    g_autoptr (OstreeRepoAutoLock) lock
        = ostree_repo_auto_lock_push (repo, OSTREE_REPO_LOCK_EXCLUSIVE, NULL, &error);
    g_assert_no_error (error);
  }

  g_autoptr (OstreeRepoAutoLock) lock1
      = ostree_repo_auto_lock_push (repo, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);

  g_autoptr (OstreeRepoAutoLock) lock2
      = ostree_repo_auto_lock_push (repo, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
}

/* Locking from single thread with a single OstreeRepo */
static void
test_repo_lock_single (Fixture *fixture, gconstpointer test_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeRepo) repo = ostree_repo_open_at (fixture->tmpdir.fd, ".", NULL, &error);
  g_assert_no_error (error);

  /* Single thread on a single repo can freely recurse in any state  */
  ostree_repo_lock_push (repo, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
  ostree_repo_lock_push (repo, OSTREE_REPO_LOCK_EXCLUSIVE, NULL, &error);
  g_assert_no_error (error);
  ostree_repo_lock_push (repo, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
  ostree_repo_lock_pop (repo, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
  ostree_repo_lock_pop (repo, OSTREE_REPO_LOCK_EXCLUSIVE, NULL, &error);
  g_assert_no_error (error);
  ostree_repo_lock_pop (repo, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
}

/* Unlocking without having ever locked */
static void
test_repo_lock_unlock_never_locked (Fixture *fixture, gconstpointer test_data)
{
  if (g_test_subprocess ())
    {
      g_autoptr (GError) error = NULL;
      g_autoptr (OstreeRepo) repo = ostree_repo_open_at (fixture->tmpdir.fd, ".", NULL, &error);
      g_assert_no_error (error);

      ostree_repo_lock_pop (repo, OSTREE_REPO_LOCK_SHARED, NULL, &error);

      return;
    }

  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*ERROR*Cannot pop repo never locked repo lock\n");
}

/* Unlocking after already unlocked */
static void
test_repo_lock_double_unlock (Fixture *fixture, gconstpointer test_data)
{
  if (g_test_subprocess ())
    {
      g_autoptr (GError) error = NULL;
      g_autoptr (OstreeRepo) repo = ostree_repo_open_at (fixture->tmpdir.fd, ".", NULL, &error);
      g_assert_no_error (error);

      ostree_repo_lock_push (repo, OSTREE_REPO_LOCK_SHARED, NULL, &error);
      g_assert_no_error (error);
      ostree_repo_lock_pop (repo, OSTREE_REPO_LOCK_SHARED, NULL, &error);
      g_assert_no_error (error);
      ostree_repo_lock_pop (repo, OSTREE_REPO_LOCK_SHARED, NULL, &error);

      return;
    }

  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*ERROR*Cannot pop already unlocked repo lock\n");
}

/* Unlocking the wrong type */
static void
test_repo_lock_unlock_wrong_type (Fixture *fixture, gconstpointer test_data)
{
  if (g_test_subprocess ())
    {
      g_autoptr (GError) error = NULL;
      g_autoptr (OstreeRepo) repo = ostree_repo_open_at (fixture->tmpdir.fd, ".", NULL, &error);
      g_assert_no_error (error);

      ostree_repo_lock_push (repo, OSTREE_REPO_LOCK_SHARED, NULL, &error);
      g_assert_no_error (error);
      ostree_repo_lock_pop (repo, OSTREE_REPO_LOCK_EXCLUSIVE, NULL, &error);

      return;
    }

  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr (
      "*ERROR*Repo exclusive lock pop requested, but none have been taken\n");
}

/* Locking with single thread and multiple OstreeRepos */
static void
test_repo_lock_multi_repo (Fixture *fixture, gconstpointer test_data)
{
  g_autoptr (GError) error = NULL;

  /* Open two OstreeRepo instances */
  g_autoptr (OstreeRepo) repo1 = ostree_repo_open_at (fixture->tmpdir.fd, ".", NULL, &error);
  g_assert_no_error (error);
  g_autoptr (OstreeRepo) repo2 = ostree_repo_open_at (fixture->tmpdir.fd, ".", NULL, &error);
  g_assert_no_error (error);

  /* Single thread with multiple OstreeRepo's conflict */
  ostree_repo_lock_push (repo1, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
  ostree_repo_lock_push (repo2, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
  ostree_repo_lock_push (repo1, OSTREE_REPO_LOCK_EXCLUSIVE, NULL, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
  g_clear_error (&error);
  ostree_repo_lock_pop (repo1, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
  ostree_repo_lock_pop (repo2, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);

  /* Recursive lock should stay exclusive once acquired */
  ostree_repo_lock_push (repo1, OSTREE_REPO_LOCK_EXCLUSIVE, NULL, &error);
  g_assert_no_error (error);
  ostree_repo_lock_push (repo1, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
  ostree_repo_lock_push (repo2, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
  g_clear_error (&error);
  ostree_repo_lock_push (repo2, OSTREE_REPO_LOCK_EXCLUSIVE, NULL, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
  g_clear_error (&error);
  ostree_repo_lock_pop (repo1, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
  ostree_repo_lock_pop (repo1, OSTREE_REPO_LOCK_EXCLUSIVE, NULL, &error);
  g_assert_no_error (error);
}

/* Locking from multiple threads with a single OstreeRepo */
typedef struct
{
  OstreeRepo *repo;
  guint step;
} LockThreadData;

static gpointer
lock_thread1 (gpointer thread_data)
{
  LockThreadData *data = thread_data;
  g_autoptr (GError) error = NULL;

  /* Step 0: Take an exclusive lock */
  g_assert_cmpuint (data->step, ==, 0);
  g_test_message ("Thread 1: Push exclusive lock");
  ostree_repo_lock_push (data->repo, OSTREE_REPO_LOCK_EXCLUSIVE, NULL, &error);
  g_assert_no_error (error);
  data->step++;

  /* Step 2: Take a shared lock */
  while (data->step != 2)
    g_thread_yield ();
  g_test_message ("Thread 1: Push shared lock");
  ostree_repo_lock_push (data->repo, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
  data->step++;

  /* Step 4: Pop both locks */
  while (data->step != 4)
    g_thread_yield ();
  g_test_message ("Thread 1: Pop shared lock");
  ostree_repo_lock_pop (data->repo, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
  g_test_message ("Thread 1: Pop exclusive lock");
  ostree_repo_lock_pop (data->repo, OSTREE_REPO_LOCK_EXCLUSIVE, NULL, &error);
  g_assert_no_error (error);
  data->step++;

  return NULL;
}

static gpointer
lock_thread2 (gpointer thread_data)
{
  LockThreadData *data = thread_data;
  g_autoptr (GError) error = NULL;

  /* Step 1: Wait for the other thread to acquire a lock and then take a
   * shared lock.
   */
  while (data->step != 1)
    g_thread_yield ();
  g_test_message ("Thread 2: Push shared lock");
  ostree_repo_lock_push (data->repo, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
  data->step++;

  /* Step 6: Pop lock */
  while (data->step != 6)
    g_thread_yield ();
  g_test_message ("Thread 2: Pop shared lock");
  ostree_repo_lock_pop (data->repo, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
  data->step++;

  return NULL;
}

static void
test_repo_lock_multi_thread (Fixture *fixture, gconstpointer test_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeRepo) repo1 = ostree_repo_open_at (fixture->tmpdir.fd, ".", NULL, &error);
  g_assert_no_error (error);
  g_autoptr (OstreeRepo) repo2 = ostree_repo_open_at (fixture->tmpdir.fd, ".", NULL, &error);
  g_assert_no_error (error);

  LockThreadData thread_data = { repo1, 0 };
  GThread *thread1 = g_thread_new ("lock-thread-1", lock_thread1, &thread_data);
  GThread *thread2 = g_thread_new ("lock-thread-2", lock_thread2, &thread_data);

  /* Step 3: Try to take a shared lock on repo2. This should fail since
   * thread1 still has an exclusive lock.
   */
  while (thread_data.step != 3)
    g_thread_yield ();
  g_test_message ("Repo 2: Push failing shared lock");
  ostree_repo_lock_push (repo2, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
  g_clear_error (&error);
  thread_data.step++;

  /* Step 5: Try to a lock on repo2. A shared lock should succeed since
   * thread1 has dropped its exclusive lock.
   */
  while (thread_data.step != 5)
    g_thread_yield ();
  g_test_message ("Repo 2: Push shared lock");
  ostree_repo_lock_push (repo2, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
  g_test_message ("Repo 2: Push failing exclusive lock");
  ostree_repo_lock_push (repo2, OSTREE_REPO_LOCK_EXCLUSIVE, NULL, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
  g_clear_error (&error);
  thread_data.step++;

  /* Step 7: Now both threads have dropped their locks and taking an exclusive
   * lock should succeed.
   */
  while (thread_data.step != 7)
    g_thread_yield ();
  g_test_message ("Repo 2: Push exclusive lock");
  ostree_repo_lock_push (repo2, OSTREE_REPO_LOCK_EXCLUSIVE, NULL, &error);
  g_assert_no_error (error);
  g_test_message ("Repo 2: Pop exclusive lock");
  ostree_repo_lock_pop (repo2, OSTREE_REPO_LOCK_EXCLUSIVE, NULL, &error);
  g_assert_no_error (error);
  g_test_message ("Repo 2: Pop shared lock");
  ostree_repo_lock_pop (repo2, OSTREE_REPO_LOCK_SHARED, NULL, &error);
  g_assert_no_error (error);
  thread_data.step++;

  g_thread_join (thread1);
  g_thread_join (thread2);
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  g_test_init (&argc, &argv, NULL);

  g_test_add ("/repo/hash", Fixture, NULL, setup, test_repo_hash, teardown);
  g_test_add ("/repo/hash/closed", Fixture, NULL, setup, test_repo_hash_closed, teardown);
  g_test_add ("/repo/equal", Fixture, NULL, setup, test_repo_equal, teardown);
  g_test_add ("/repo/get_min_free_space", Fixture, NULL, setup, test_repo_get_min_free_space,
              teardown);
  g_test_add ("/repo/write_regfile_api", Fixture, NULL, setup, test_write_regfile_api, teardown);
  g_test_add ("/repo/autolock", Fixture, NULL, setup, test_repo_autolock, teardown);
  g_test_add ("/repo/lock/single", Fixture, NULL, lock_setup, test_repo_lock_single, teardown);
  g_test_add ("/repo/lock/unlock-never-locked", Fixture, NULL, lock_setup,
              test_repo_lock_unlock_never_locked, teardown);
  g_test_add ("/repo/lock/double-unlock", Fixture, NULL, lock_setup, test_repo_lock_double_unlock,
              teardown);
  g_test_add ("/repo/lock/unlock-wrong-type", Fixture, NULL, lock_setup,
              test_repo_lock_unlock_wrong_type, teardown);
  g_test_add ("/repo/lock/multi-repo", Fixture, NULL, lock_setup, test_repo_lock_multi_repo,
              teardown);
  g_test_add ("/repo/lock/multi-thread", Fixture, NULL, lock_setup, test_repo_lock_multi_thread,
              teardown);

  return g_test_run ();
}
