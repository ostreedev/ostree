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
#include <linux/fs.h>
#include <locale.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>

#include "ostree-autocleanups.h"
#include "ostree-repo-private.h"
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

/* Test that writing duplicate content objects within a single transaction does
 * not spuriously trigger the min-free-space check.
 */
static guint64
expected_min_free_space_bytes (OstreeRepo *repo)
{
  struct statvfs stvfsbuf;
  if (TEMP_FAILURE_RETRY (fstatvfs (repo->repo_dir_fd, &stvfsbuf)) < 0)
    g_assert_not_reached ();

  /* Mirrors min_free_space_calculate_reserved_bytes(): the smaller of the two
   * configured limits is enforced. */
  guint64 total_bytes = (guint64)stvfsbuf.f_frsize * stvfsbuf.f_blocks;
  guint64 from_mb = repo->min_free_space_mb > 0 ? (guint64)repo->min_free_space_mb << 20 : 0;
  guint64 from_pct = repo->min_free_space_percent > 0
                         ? (guint64)((double)total_bytes * (repo->min_free_space_percent / 100.0))
                         : 0;

  if (from_mb > 0 && from_pct > 0)
    return MIN (from_mb, from_pct);
  else if (from_mb > 0)
    return from_mb;
  else
    return from_pct;
}

static void
reload_config_with (OstreeRepo *repo, GKeyFile *config, const char *size, const char *percent)
{
  g_autoptr (GError) error = NULL;
  g_key_file_remove_key (config, "core", "min-free-space-size", NULL);
  g_key_file_remove_key (config, "core", "min-free-space-percent", NULL);
  if (size != NULL)
    g_key_file_set_string (config, "core", "min-free-space-size", size);
  if (percent != NULL)
    g_key_file_set_string (config, "core", "min-free-space-percent", percent);
  g_assert_true (ostree_repo_write_config_and_reload (repo, config, &error));
  g_assert_no_error (error);
}

static void
test_repo_get_min_free_space_limits (Fixture *fixture, gconstpointer test_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeRepo) repo = ostree_repo_create_at (
      fixture->tmpdir.fd, ".", OSTREE_REPO_MODE_ARCHIVE, NULL, NULL, &error);
  g_assert_no_error (error);
  g_autoptr (GKeyFile) config = ostree_repo_copy_config (repo);
  guint64 bytes = 0;

  /* Only min-free-space-size is set; the reserved bytes are exact. */
  reload_config_with (repo, config, "500MB", NULL);
  g_assert_cmpuint (repo->min_free_space_percent, ==, 0);
  g_assert_cmpuint (repo->min_free_space_mb, ==, 500);
  g_assert_true (ostree_repo_get_min_free_space_bytes (repo, &bytes, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (bytes, ==, (guint64)500 << 20);

  /* Only min-free-space-percent is set; match the percentage. */
  reload_config_with (repo, config, NULL, "97");
  g_assert_cmpuint (repo->min_free_space_percent, ==, 97);
  g_assert_cmpuint (repo->min_free_space_mb, ==, 0);
  g_assert_true (ostree_repo_get_min_free_space_bytes (repo, &bytes, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (bytes, ==, expected_min_free_space_bytes (repo));

  /* Both are set; the smaller of the two limits is enforced. */
  reload_config_with (repo, config, "500MB", "97");
  g_assert_cmpuint (repo->min_free_space_percent, ==, 97);
  g_assert_cmpuint (repo->min_free_space_mb, ==, 500);
  g_assert_true (ostree_repo_get_min_free_space_bytes (repo, &bytes, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (bytes, ==, expected_min_free_space_bytes (repo));

  /* Neither is set; the default of min(3%, 1GB) applies. */
  reload_config_with (repo, config, NULL, NULL);
  g_assert_cmpuint (repo->min_free_space_percent, ==, 3);
  g_assert_cmpuint (repo->min_free_space_mb, ==, 1024);
  g_assert_true (ostree_repo_get_min_free_space_bytes (repo, &bytes, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (bytes, ==, expected_min_free_space_bytes (repo));

  /* Reloading only a size after the default must clear the stale percent value,
   * and setting the size to zero disables the check entirely. */
  reload_config_with (repo, config, "1MB", NULL);
  g_assert_cmpuint (repo->min_free_space_percent, ==, 0);
  g_assert_cmpuint (repo->min_free_space_mb, ==, 1);
  reload_config_with (repo, config, "0MB", NULL);
  g_assert_cmpuint (repo->min_free_space_percent, ==, 0);
  g_assert_cmpuint (repo->min_free_space_mb, ==, 0);
}

static void
test_min_free_space_dup_content (Fixture *fixture, gconstpointer test_data)
{
  g_autoptr (GError) error = NULL;

  g_autoptr (OstreeRepo) repo = ostree_repo_create_at (
      fixture->tmpdir.fd, ".", OSTREE_REPO_MODE_BARE_USER_ONLY, NULL, NULL, &error);
  g_assert_no_error (error);

  const guint n_files = 64;
  const guint file_size = 4096;
  const guint dup_rounds = 10;

  ostree_repo_prepare_transaction (repo, NULL, NULL, &error);
  g_assert_no_error (error);

  /* Directly set the free-space budget via the private txn.max_blocks field. Budget for 2x the
   * blocks that one round of unique writes will reserve. With the bug, 11 rounds would be charged
   * (1 unique + 10 duplicate), easily exceeding 2x.  With the fix, only the unique round is
   * charged.
   */
  fsblkcnt_t blocks_per_file = (file_size / repo->txn.blocksize) + 1;
  repo->txn.max_blocks = n_files * blocks_per_file * 2;
  /* Enable space check by setting a nonzero min_free_space_percent. */
  repo->min_free_space_percent = 1;

  g_autofree guint8 *buf = g_malloc0 (file_size);

  /* Round 1: write unique objects.  Track checksums to verify each file
   * produces a distinct content object.
   */
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < n_files; i++)
    {
      memcpy (buf, &i, sizeof (i));
      g_autofree char *checksum = ostree_repo_write_regfile_inline (
          repo, NULL, 0, 0, S_IFREG | 0644, NULL, buf, file_size, NULL, &error);
      g_assert_no_error (error);
      g_assert_nonnull (checksum);
      g_assert_false (g_hash_table_contains (seen, checksum));
      g_hash_table_add (seen, g_steal_pointer (&checksum));
    }
  g_assert_cmpuint (g_hash_table_size (seen), ==, n_files);

  /* Duplicate rounds: rewrite the exact same objects many times. */
  for (guint round = 0; round < dup_rounds; round++)
    {
      for (guint i = 0; i < n_files; i++)
        {
          memcpy (buf, &i, sizeof (i));
          g_autofree char *checksum = ostree_repo_write_regfile_inline (
              repo, NULL, 0, 0, S_IFREG | 0644, NULL, buf, file_size, NULL, &error);
          g_assert_no_error (error);
          g_assert_nonnull (checksum);
        }
    }

  ostree_repo_commit_transaction (repo, NULL, NULL, &error);
  g_assert_no_error (error);
}

static GVariant *
test_xattr_cb (OstreeRepo *repo, const char *path, GFileInfo *file_info, gpointer data)
{
  return g_variant_ref ((GVariant *)data);
}

/* Test that writing content objects that get reflinked does not spuriously trigger the
 * min-free-space check. This simulates the bootc install-to-filesystem flow where a merged commit
 * re-writes content with SELinux labels applied.
 */
static void
test_min_free_space_reflinked_content (Fixture *fixture, gconstpointer test_data)
{
  g_autoptr (GError) error = NULL;

  /* Check if the filesystem supports reflinks; skip if not. */
  {
    g_auto (GLnxTmpfile) tmpf1 = {
      0,
    };
    g_auto (GLnxTmpfile) tmpf2 = {
      0,
    };
    if (!glnx_open_tmpfile_linkable_at (fixture->tmpdir.fd, ".", O_RDWR | O_CLOEXEC, &tmpf1,
                                        &error))
      g_assert_no_error (error);
    (void)glnx_loop_write (tmpf1.fd, "x", 1);
    if (!glnx_open_tmpfile_linkable_at (fixture->tmpdir.fd, ".", O_RDWR | O_CLOEXEC, &tmpf2,
                                        &error))
      g_assert_no_error (error);
    if (ioctl (tmpf2.fd, FICLONE, tmpf1.fd) < 0)
      {
        g_test_skip ("filesystem does not support reflinks");
        return;
      }
  }

  /* Use BARE_USER_ONLY so checkout produces hardlinks to repo objects,
   * which is what triggers FICLONE in write_content_object. */
  g_assert_cmpint (mkdirat (fixture->tmpdir.fd, "repo", 0755), ==, 0);
  glnx_autofd int repo_dfd = -1;
  if (!glnx_opendirat (fixture->tmpdir.fd, "repo", TRUE, &repo_dfd, &error))
    g_assert_no_error (error);
  g_autoptr (OstreeRepo) repo
      = ostree_repo_create_at (repo_dfd, ".", OSTREE_REPO_MODE_BARE_USER_ONLY, NULL, NULL, &error);
  g_assert_no_error (error);

  const guint n_files = 64;
  const guint file_size = 4096;

  /* Round 1: create files on disk and commit them into the repo. */
  g_assert_cmpint (mkdirat (fixture->tmpdir.fd, "content", 0755), ==, 0);
  g_autofree guint8 *buf = g_malloc0 (file_size);
  for (guint i = 0; i < n_files; i++)
    {
      memcpy (buf, &i, sizeof (i));
      g_autofree char *name = g_strdup_printf ("content/file%u", i);
      glnx_file_replace_contents_at (fixture->tmpdir.fd, name, buf, file_size, 0, NULL, &error);
      g_assert_no_error (error);
    }

  ostree_repo_prepare_transaction (repo, NULL, NULL, &error);
  g_assert_no_error (error);
  g_autoptr (OstreeMutableTree) mtree = ostree_mutable_tree_new ();
  ostree_repo_write_dfd_to_mtree (repo, fixture->tmpdir.fd, "content", mtree, NULL, NULL, &error);
  g_assert_no_error (error);
  g_autoptr (GFile) root = NULL;
  ostree_repo_write_mtree (repo, mtree, &root, NULL, &error);
  g_assert_no_error (error);
  g_autofree char *commit = NULL;
  ostree_repo_write_commit (repo, NULL, NULL, NULL, NULL, OSTREE_REPO_FILE (root), &commit, NULL,
                            &error);
  g_assert_no_error (error);
  ostree_repo_transaction_set_ref (repo, NULL, "testref", commit);
  ostree_repo_commit_transaction (repo, NULL, NULL, &error);
  g_assert_no_error (error);

  /* Checkout the commit. */
  OstreeRepoCheckoutAtOptions co_opts = {
    0,
  };
  co_opts.mode = OSTREE_REPO_CHECKOUT_MODE_USER;
  ostree_repo_checkout_at (repo, &co_opts, fixture->tmpdir.fd, "checkout", commit, NULL, &error);
  g_assert_no_error (error);

  /* Round 2: re-import the checkout with a modifier that adds an xattr, simulating SELinux
   * relabeling. The content is identical so FICLONE will share data blocks, but the xattr change
   * produces new checksums. */
  ostree_repo_prepare_transaction (repo, NULL, NULL, &error);
  g_assert_no_error (error);

  /* Set max_blocks budget to half what one round of writes would charge. Without the reflink
   * credit-back, this would exhaust the budget halfway through. */
  fsblkcnt_t blocks_per_file = (file_size / repo->txn.blocksize) + 1;
  repo->txn.max_blocks = n_files * blocks_per_file / 2;
  repo->min_free_space_percent = 1;

  g_autoptr (OstreeRepoCommitModifier) modifier
      = ostree_repo_commit_modifier_new (0, NULL, NULL, NULL);
  GVariantBuilder xattr_builder;
  g_variant_builder_init (&xattr_builder, G_VARIANT_TYPE ("a(ayay)"));
  /* Add a fake security.selinux xattr */
  const char *label = "system_u:object_r:usr_t:s0";
  g_variant_builder_add (
      &xattr_builder, "(@ay@ay)", g_variant_new_bytestring ("security.selinux"),
      g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, label, strlen (label) + 1, 1));
  g_autoptr (GVariant) xattrs = g_variant_ref_sink (g_variant_builder_end (&xattr_builder));
  ostree_repo_commit_modifier_set_xattr_callback (modifier, test_xattr_cb, NULL, xattrs);

  g_autoptr (OstreeMutableTree) mtree2 = ostree_mutable_tree_new ();
  ostree_repo_write_dfd_to_mtree (repo, fixture->tmpdir.fd, "checkout", mtree2, modifier, NULL,
                                  &error);
  g_assert_no_error (error);

  ostree_repo_commit_transaction (repo, NULL, NULL, &error);
  g_assert_no_error (error);
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
  g_test_add ("/repo/get_min_free_space_limits", Fixture, NULL, setup,
              test_repo_get_min_free_space_limits, teardown);
  g_test_add ("/repo/write_regfile_api", Fixture, NULL, setup, test_write_regfile_api, teardown);
  g_test_add ("/repo/min_free_space_dup_content", Fixture, NULL, setup,
              test_min_free_space_dup_content, teardown);
  g_test_add ("/repo/min_free_space_reflinked_content", Fixture, NULL, setup,
              test_min_free_space_reflinked_content, teardown);
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
