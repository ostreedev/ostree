/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
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
 */

#include "config.h"
#include "libglnx.h"
#include <glib.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>

#include <ostree.h>
#include <archive.h>
#include <archive_entry.h>

typedef struct {
  OstreeRepo *repo;
  int fd;
  int fd_empty;
  char *tmpd;
} TestData;

static void
test_data_init (TestData *td)
{
  GError *error = NULL;
  struct archive *a = archive_write_new ();
  struct archive_entry *ae;

  td->tmpd = g_mkdtemp (g_strdup ("/var/tmp/test-libarchive-import-XXXXXX"));
  g_assert_cmpint (0, ==, chdir (td->tmpd));

  td->fd = openat (AT_FDCWD, "foo.tar.gz", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
  (void) unlink ("foo.tar.gz");

  g_assert_no_error (error);
  g_assert (td->fd >= 0);
  
  g_assert_cmpint (0, ==, archive_write_set_format_pax (a));
  g_assert_cmpint (0, ==, archive_write_add_filter_gzip (a));
  g_assert_cmpint (0, ==, archive_write_open_fd (a, td->fd));

  ae = archive_entry_new ();
  archive_entry_set_pathname (ae, "/");
  archive_entry_set_mode (ae, S_IFDIR | 0755);
  g_assert_cmpint (0, ==, archive_write_header (a, ae));
  archive_entry_free (ae);

  ae = archive_entry_new ();
  archive_entry_set_pathname (ae, "/file");
  archive_entry_set_mode (ae, S_IFREG | 0777);
  archive_entry_set_size (ae, 4);
  g_assert_cmpint (0, ==, archive_write_header (a, ae));
  g_assert_cmpint (4, ==, archive_write_data (a, "foo\n", 4));
  archive_entry_free (ae);

  ae = archive_entry_new ();
  archive_entry_set_pathname (ae, "/devnull");
  archive_entry_set_mode (ae, S_IFCHR | 0777);
  archive_entry_set_devmajor (ae, 1);
  archive_entry_set_devminor (ae, 3);
  g_assert_cmpint (0, ==, archive_write_header (a, ae));
  archive_entry_free (ae);

  ae = archive_entry_new ();
  archive_entry_set_pathname (ae, "/anotherfile");
  archive_entry_set_mode (ae, S_IFREG | 0777);
  archive_entry_set_size (ae, 4);
  g_assert_cmpint (0, ==, archive_write_header (a, ae));
  g_assert_cmpint (4, ==, archive_write_data (a, "bar\n", 4));
  archive_entry_free (ae);

  g_assert_cmpint (ARCHIVE_OK, ==, archive_write_close (a));
  g_assert_cmpint (ARCHIVE_OK, ==, archive_write_free (a));

  td->fd_empty = openat (AT_FDCWD, "empty.tar.gz", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
  g_assert (td->fd_empty >= 0);
  (void) unlink ("empty.tar.gz");

  a = archive_write_new ();
  g_assert (a);

  g_assert_cmpint (0, ==, archive_write_set_format_pax (a));
  g_assert_cmpint (0, ==, archive_write_add_filter_gzip (a));
  g_assert_cmpint (0, ==, archive_write_open_fd (a, td->fd_empty));
  g_assert_cmpint (ARCHIVE_OK, ==, archive_write_close (a));
  g_assert_cmpint (ARCHIVE_OK, ==, archive_write_free (a));

  { g_autoptr(GFile) repopath = g_file_new_for_path ("repo");
    td->repo = ostree_repo_new (repopath);

    g_assert_cmpint (0, ==, mkdir ("repo", 0755));

    ostree_repo_create (td->repo, OSTREE_REPO_MODE_BARE_USER, NULL, &error);
    g_assert_no_error (error);
  }
}

static gboolean
spawn_cmdline (const char *cmd, GError **error)
{
  int estatus;
  if (!g_spawn_command_line_sync (cmd, NULL, NULL, &estatus, error))
    return FALSE;
  if (!g_spawn_check_exit_status (estatus, error))
    return FALSE;
  return TRUE;
}

static void
test_libarchive_noautocreate_empty (gconstpointer data)
{
  TestData *td = (void*)data;
  GError *error = NULL;
  struct archive *a = archive_read_new ();
  OstreeRepoImportArchiveOptions opts = { 0, };
  glnx_unref_object OstreeMutableTree *mtree = ostree_mutable_tree_new ();

  g_assert_cmpint (0, ==, lseek (td->fd_empty, 0, SEEK_SET));
  g_assert_cmpint (0, ==, archive_read_support_format_all (a));
  g_assert_cmpint (0, ==, archive_read_support_filter_all (a));
  g_assert_cmpint (0, ==, archive_read_open_fd (a, td->fd_empty, 8192));

  (void)ostree_repo_import_archive_to_mtree (td->repo, &opts, a, mtree, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert (ostree_mutable_tree_get_metadata_checksum (mtree) == NULL);
}

static void
test_libarchive_autocreate_empty (gconstpointer data)
{
  TestData *td = (void*)data;
  GError *error = NULL;
  struct archive *a = archive_read_new ();
  OstreeRepoImportArchiveOptions opts = { 0, };
  glnx_unref_object OstreeMutableTree *mtree = ostree_mutable_tree_new ();

  opts.autocreate_parents = 1;

  g_assert_cmpint (0, ==, lseek (td->fd_empty, 0, SEEK_SET));
  g_assert_cmpint (0, ==, archive_read_support_format_all (a));
  g_assert_cmpint (0, ==, archive_read_support_filter_all (a));
  g_assert_cmpint (0, ==, archive_read_open_fd (a, td->fd_empty, 8192));

  (void)ostree_repo_import_archive_to_mtree (td->repo, &opts, a, mtree, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert (ostree_mutable_tree_get_metadata_checksum (mtree) != NULL);
}

static void
test_libarchive_error_device_file (gconstpointer data)
{
  TestData *td = (void*)data;
  GError *error = NULL;
  struct archive *a = archive_read_new ();
  OstreeRepoImportArchiveOptions opts = { 0, };
  glnx_unref_object OstreeMutableTree *mtree = ostree_mutable_tree_new ();

  g_assert_cmpint (0, ==, lseek (td->fd, 0, SEEK_SET));
  g_assert_cmpint (0, ==, archive_read_support_format_all (a));
  g_assert_cmpint (0, ==, archive_read_support_filter_all (a));
  g_assert_cmpint (0, ==, archive_read_open_fd (a, td->fd, 8192));

  (void)ostree_repo_import_archive_to_mtree (td->repo, &opts, a, mtree, NULL, NULL, &error);
  g_assert (error != NULL);
}

static void
test_libarchive_ignore_device_file (gconstpointer data)
{
  TestData *td = (void*)data;
  GError *error = NULL;
  GCancellable *cancellable = NULL;
  struct archive *a = archive_read_new ();
  OstreeRepoImportArchiveOptions opts = { 0, };
  glnx_unref_object OstreeMutableTree *mtree = ostree_mutable_tree_new ();
  glnx_unref_object GFile *root = NULL;
  g_autofree char *commit_checksum = NULL;

  g_assert_cmpint (0, ==, lseek (td->fd, 0, SEEK_SET));
  g_assert_cmpint (0, ==, archive_read_support_format_all (a));
  g_assert_cmpint (0, ==, archive_read_support_filter_all (a));
  g_assert_cmpint (0, ==, archive_read_open_fd (a, td->fd, 8192));

  opts.ignore_unsupported_content = TRUE;

  if (!ostree_repo_prepare_transaction (td->repo, NULL, cancellable, &error))
    goto out;

  if (!ostree_repo_import_archive_to_mtree (td->repo, &opts, a, mtree, NULL, NULL, &error))
    goto out;

  if (!ostree_repo_write_mtree (td->repo, mtree, &root, cancellable, &error))
    goto out;

  if (!ostree_repo_write_commit (td->repo, NULL, "", "", NULL,
                                 OSTREE_REPO_FILE (root),
                                 &commit_checksum, cancellable, &error))
    goto out;

  ostree_repo_transaction_set_ref (td->repo, NULL, "foo", commit_checksum);
  
  if (!ostree_repo_commit_transaction (td->repo, NULL, cancellable, &error))
    goto out;

  if (!spawn_cmdline ("ostree --repo=repo ls foo file", &error))
    goto out;

  if (!spawn_cmdline ("ostree --repo=repo ls foo anotherfile", &error))
    goto out;

  if (spawn_cmdline ("ostree --repo=repo ls foo devnull", &error))
    g_assert_not_reached ();
  g_assert (error != NULL);
  g_clear_error (&error);

 out:
  g_assert_no_error (error);
}

int main (int argc, char **argv)
{
  TestData td = {NULL,};
  int r;

  test_data_init (&td);

  g_test_init (&argc, &argv, NULL);

  g_test_add_data_func ("/libarchive/noautocreate-empty", &td, test_libarchive_noautocreate_empty);
  g_test_add_data_func ("/libarchive/autocreate-empty", &td, test_libarchive_autocreate_empty);
  g_test_add_data_func ("/libarchive/error-device-file", &td, test_libarchive_error_device_file);
  g_test_add_data_func ("/libarchive/ignore-device-file", &td, test_libarchive_ignore_device_file);

  r = g_test_run();

  if (td.tmpd)
    (void) glnx_shutil_rm_rf_at (AT_FDCWD, td.tmpd, NULL, NULL);
  return r;
}
