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

#include "ostree-autocleanups.h"
#include "ostree-types.h"

/* Test fixture. Creates a temporary directory. */
typedef struct
{
  GLnxTmpDir tmpdir;  /* (owned) */
} Fixture;

static void
setup (Fixture       *fixture,
       gconstpointer  test_data)
{
  g_autoptr(GError) error = NULL;

  (void) glnx_mkdtemp ("test-repo-XXXXXX", 0700, &fixture->tmpdir, &error);
  g_assert_no_error (error);

  g_test_message ("Using temporary directory: %s", fixture->tmpdir.path);
}

static void
teardown (Fixture       *fixture,
          gconstpointer  test_data)
{
  /* Recursively remove the temporary directory. */
  (void) glnx_tmpdir_delete (&fixture->tmpdir, NULL, NULL);
}

/* Test that the hash values for two #OstreeRepo instances pointing at the same
 * repository are equal. We can’t test anything else, since hash collisions are
 * always a possibility. */
static void
test_repo_hash (Fixture       *fixture,
                gconstpointer  test_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(OstreeRepo) repo1 = ostree_repo_create_at (fixture->tmpdir.fd, ".",
                                                       OSTREE_REPO_MODE_ARCHIVE,
                                                       NULL,
                                                       NULL, &error);
  g_assert_no_error (error);

  g_autoptr(OstreeRepo) repo2 = ostree_repo_open_at (fixture->tmpdir.fd, ".",
                                                     NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpuint (ostree_repo_hash (repo1), ==, ostree_repo_hash (repo2));
}

/* Test that trying to hash a closed repo results in an assertion failure. */
static void
test_repo_hash_closed (Fixture       *fixture,
                       gconstpointer  test_data)
{
  if (g_test_subprocess ())
    {
      g_autoptr(GFile) repo_path = g_file_new_for_path (fixture->tmpdir.path);
      g_autoptr(OstreeRepo) repo = ostree_repo_new (repo_path);

      ostree_repo_hash (repo);

      return;
    }

  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*ERROR*ostree_repo_hash: assertion failed:*");
}

/* Test that various repositories test equal (or not) with each other. */
static void
test_repo_equal (Fixture       *fixture,
                 gconstpointer  test_data)
{
  g_autoptr(GError) error = NULL;

  /* Create a few separate repos and some #OstreeRepo objects for them. */
  glnx_ensure_dir (fixture->tmpdir.fd, "repo1", 0755, &error);
  g_assert_no_error (error);
  glnx_ensure_dir (fixture->tmpdir.fd, "repo2", 0755, &error);
  g_assert_no_error (error);

  g_autoptr(OstreeRepo) repo1 = ostree_repo_create_at (fixture->tmpdir.fd, "repo1",
                                                       OSTREE_REPO_MODE_ARCHIVE,
                                                       NULL,
                                                       NULL, &error);
  g_assert_no_error (error);

  g_autoptr(OstreeRepo) repo1_alias = ostree_repo_open_at (fixture->tmpdir.fd, "repo1",
                                                           NULL, &error);
  g_assert_no_error (error);

  g_autoptr(OstreeRepo) repo2 = ostree_repo_create_at (fixture->tmpdir.fd, "repo2",
                                                       OSTREE_REPO_MODE_ARCHIVE,
                                                       NULL,
                                                       NULL, &error);
  g_assert_no_error (error);

  g_autoptr(GFile) closed_repo_path = g_file_new_for_path (fixture->tmpdir.path);
  g_autoptr(OstreeRepo) closed_repo = ostree_repo_new (closed_repo_path);

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

int
main (int    argc,
      char **argv)
{
  setlocale (LC_ALL, "");
  g_test_init (&argc, &argv, NULL);

  g_test_add ("/repo/hash", Fixture, NULL, setup,
              test_repo_hash, teardown);
  g_test_add ("/repo/hash/closed", Fixture, NULL, setup,
              test_repo_hash_closed, teardown);
  g_test_add ("/repo/equal", Fixture, NULL, setup,
              test_repo_equal, teardown);

  return g_test_run ();
}
