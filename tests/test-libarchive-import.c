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
#include "ostree-libarchive-private.h"
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
  ot_cleanup_write_archive struct archive *a = archive_write_new ();
  struct archive_entry *ae;
  uid_t uid = getuid ();
  gid_t gid = getgid ();

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
  archive_entry_set_uid (ae, uid);
  archive_entry_set_gid (ae, gid);
  g_assert_cmpint (0, ==, archive_write_header (a, ae));
  archive_entry_free (ae);

  ae = archive_entry_new ();
  archive_entry_set_pathname (ae, "/file");
  archive_entry_set_mode (ae, S_IFREG | 0777);
  archive_entry_set_uid (ae, uid);
  archive_entry_set_gid (ae, gid);
  archive_entry_set_size (ae, 4);
  g_assert_cmpint (0, ==, archive_write_header (a, ae));
  g_assert_cmpint (4, ==, archive_write_data (a, "foo\n", 4));
  archive_entry_free (ae);

  ae = archive_entry_new ();
  archive_entry_set_pathname (ae, "/devnull");
  archive_entry_set_mode (ae, S_IFCHR | 0777);
  archive_entry_set_uid (ae, uid);
  archive_entry_set_gid (ae, gid);
  archive_entry_set_devmajor (ae, 1);
  archive_entry_set_devminor (ae, 3);
  g_assert_cmpint (0, ==, archive_write_header (a, ae));
  archive_entry_free (ae);

  ae = archive_entry_new ();
  archive_entry_set_pathname (ae, "/anotherfile");
  archive_entry_set_mode (ae, S_IFREG | 0777);
  archive_entry_set_uid (ae, uid);
  archive_entry_set_gid (ae, gid);
  archive_entry_set_size (ae, 4);
  g_assert_cmpint (0, ==, archive_write_header (a, ae));
  g_assert_cmpint (4, ==, archive_write_data (a, "bar\n", 4));
  archive_entry_free (ae);

  ae = archive_entry_new ();
  archive_entry_set_pathname (ae, "/etc");
  archive_entry_set_mode (ae, S_IFDIR | 0755);
  archive_entry_set_uid (ae, uid);
  archive_entry_set_gid (ae, gid);
  g_assert_cmpint (0, ==, archive_write_header (a, ae));
  archive_entry_free (ae);

  ae = archive_entry_new ();
  archive_entry_set_pathname (ae, "/etc/file");
  archive_entry_set_mode (ae, S_IFREG | 0777);
  archive_entry_set_uid (ae, uid);
  archive_entry_set_gid (ae, gid);
  archive_entry_set_size (ae, 4);
  g_assert_cmpint (0, ==, archive_write_header (a, ae));
  g_assert_cmpint (4, ==, archive_write_data (a, "bar\n", 4));
  archive_entry_free (ae);

  g_assert_cmpint (ARCHIVE_OK, ==, archive_write_close (a));

  td->fd_empty = openat (AT_FDCWD, "empty.tar.gz", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
  g_assert (td->fd_empty >= 0);
  (void) unlink ("empty.tar.gz");

  g_assert_cmpint (ARCHIVE_OK, ==, archive_write_free (a));
  a = archive_write_new ();
  g_assert (a);

  g_assert_cmpint (0, ==, archive_write_set_format_pax (a));
  g_assert_cmpint (0, ==, archive_write_add_filter_gzip (a));
  g_assert_cmpint (0, ==, archive_write_open_fd (a, td->fd_empty));
  g_assert_cmpint (ARCHIVE_OK, ==, archive_write_close (a));

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
test_archive_setup (int fd, struct archive *a)
{
  g_assert_cmpint (0, ==, lseek (fd, 0, SEEK_SET));
  g_assert_cmpint (0, ==, archive_read_support_format_all (a));
  g_assert_cmpint (0, ==, archive_read_support_filter_all (a));
  g_assert_cmpint (0, ==, archive_read_open_fd (a, fd, 8192));
}

static void
test_libarchive_noautocreate_empty (gconstpointer data)
{
  TestData *td = (void*)data;
  GError *error = NULL;
  ot_cleanup_read_archive struct archive *a = archive_read_new ();
  OstreeRepoImportArchiveOptions opts = { 0, };
  glnx_unref_object OstreeMutableTree *mtree = ostree_mutable_tree_new ();

  test_archive_setup (td->fd_empty, a);

  (void)ostree_repo_import_archive_to_mtree (td->repo, &opts, a, mtree, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert (ostree_mutable_tree_get_metadata_checksum (mtree) == NULL);
}

static void
test_libarchive_autocreate_empty (gconstpointer data)
{
  TestData *td = (void*)data;
  g_autoptr(GError) error = NULL;
  ot_cleanup_read_archive struct archive *a = archive_read_new ();
  OstreeRepoImportArchiveOptions opts = { 0, };
  glnx_unref_object OstreeMutableTree *mtree = ostree_mutable_tree_new ();

  opts.autocreate_parents = 1;

  test_archive_setup (td->fd_empty, a);

  (void)ostree_repo_import_archive_to_mtree (td->repo, &opts, a, mtree, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert (ostree_mutable_tree_get_metadata_checksum (mtree) != NULL);
}

static void
test_libarchive_error_device_file (gconstpointer data)
{
  TestData *td = (void*)data;
  g_autoptr(GError) error = NULL;
  ot_cleanup_read_archive struct archive *a = archive_read_new ();
  OstreeRepoImportArchiveOptions opts = { 0, };
  glnx_unref_object OstreeMutableTree *mtree = ostree_mutable_tree_new ();

  test_archive_setup (td->fd, a);

  (void)ostree_repo_import_archive_to_mtree (td->repo, &opts, a, mtree, NULL, NULL, &error);
  g_assert (error != NULL);
  g_clear_error (&error);
}

static gboolean
skip_if_no_xattr (TestData *td)
{
  /* /var/tmp might actually be a tmpfs */
  if (setxattr (td->tmpd, "user.test-xattr-support", "yes", 4, 0) != 0)
    {
      int saved_errno = errno;
      g_autofree gchar *message
        = g_strdup_printf ("unable to setxattr on \"%s\": %s",
                           td->tmpd, g_strerror (saved_errno));
      g_test_skip (message);
      return TRUE;
    }

  return FALSE;
}

static gboolean
import_write_and_ref (OstreeRepo      *repo,
                      OstreeRepoImportArchiveOptions *opts,
                      struct archive  *a,
                      const char      *ref,
                      OstreeRepoCommitModifier *modifier,
                      GError         **error)
{
  gboolean ret = FALSE;
  glnx_unref_object GFile *root = NULL;
  g_autofree char *commit_checksum = NULL;
  glnx_unref_object OstreeMutableTree *mtree = ostree_mutable_tree_new ();

  if (!ostree_repo_prepare_transaction (repo, NULL, NULL, error))
    goto out;

  if (!ostree_repo_import_archive_to_mtree (repo, opts, a, mtree, modifier,
                                            NULL, error))
    goto out;

  if (!ostree_repo_write_mtree (repo, mtree, &root, NULL, error))
    goto out;

  if (!ostree_repo_write_commit (repo, NULL, "", "", NULL,
                                 OSTREE_REPO_FILE (root),
                                 &commit_checksum, NULL, error))
    goto out;

  ostree_repo_transaction_set_ref (repo, NULL, ref, commit_checksum);

  if (!ostree_repo_commit_transaction (repo, NULL, NULL, error))
    goto out;

  ret = TRUE;
out:
  return ret;
}

static void
test_libarchive_ignore_device_file (gconstpointer data)
{
  TestData *td = (void*)data;
  g_autoptr(GError) error = NULL;
  ot_cleanup_read_archive struct archive *a = archive_read_new ();
  OstreeRepoImportArchiveOptions opts = { 0, };

  if (skip_if_no_xattr (td))
    goto out;

  test_archive_setup (td->fd, a);

  opts.ignore_unsupported_content = TRUE;

  if (!import_write_and_ref (td->repo, &opts, a, "foo", NULL, &error))
    goto out;

  /* check contents */
  if (!spawn_cmdline ("ostree --repo=repo ls foo file", &error))
    goto out;

  if (!spawn_cmdline ("ostree --repo=repo ls foo anotherfile", &error))
    goto out;

  if (!spawn_cmdline ("ostree --repo=repo ls foo /etc/file", &error))
    goto out;

  if (spawn_cmdline ("ostree --repo=repo ls foo devnull", &error))
    g_assert_not_reached ();
  g_assert (error != NULL);
  g_clear_error (&error);

 out:
  g_assert_no_error (error);
}

static gboolean
check_ostree_convention (GError *error)
{
  if (!spawn_cmdline ("ostree --repo=repo ls bar file", &error))
    return FALSE;

  if (!spawn_cmdline ("ostree --repo=repo ls bar anotherfile", &error))
    return FALSE;

  if (!spawn_cmdline ("ostree --repo=repo ls bar /usr/etc/file", &error))
    return FALSE;

  if (spawn_cmdline ("ostree --repo=repo ls bar /etc/file", &error))
    g_assert_not_reached ();
  g_assert (error != NULL);
  g_clear_error (&error);

  if (spawn_cmdline ("ostree --repo=repo ls bar devnull", &error))
    g_assert_not_reached ();
  g_assert (error != NULL);
  g_clear_error (&error);

  return TRUE;
}

static void
test_libarchive_ostree_convention (gconstpointer data)
{
  TestData *td = (void*)data;
  GError *error = NULL;
  ot_cleanup_read_archive struct archive *a = archive_read_new ();
  OstreeRepoImportArchiveOptions opts = { 0, };

  if (skip_if_no_xattr (td))
    goto out;

  test_archive_setup (td->fd, a);

  opts.autocreate_parents = TRUE;
  opts.use_ostree_convention = TRUE;
  opts.ignore_unsupported_content = TRUE;

  if (!import_write_and_ref (td->repo, &opts, a, "bar", NULL, &error))
    goto out;

  if (!check_ostree_convention (error))
    goto out;

 out:
  g_assert_no_error (error);
}

static GVariant*
xattr_cb (OstreeRepo  *repo,
          const char  *path,
          GFileInfo   *file_info,
          gpointer     user_data)
{
  g_auto(GVariantBuilder) builder;
  g_variant_builder_init (&builder, (GVariantType*)"a(ayay)");
  if (strcmp (path, "/anotherfile") == 0)
    g_variant_builder_add (&builder, "(@ay@ay)",
                           g_variant_new_bytestring ("user.data"),
                           g_variant_new_bytestring ("mydata"));
  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static void
test_libarchive_xattr_callback (gconstpointer data)
{
  TestData *td = (void*)data;
  GError *error = NULL;
  ot_cleanup_read_archive struct archive *a = archive_read_new ();
  OstreeRepoImportArchiveOptions opts = { 0 };
  OstreeRepoCommitModifier *modifier = NULL;
  char buf[7] = { 0 };

  if (skip_if_no_xattr (td))
    goto out;

  modifier = ostree_repo_commit_modifier_new (0, NULL, NULL, NULL);
  ostree_repo_commit_modifier_set_xattr_callback (modifier, xattr_cb,
                                                  NULL, NULL);

  test_archive_setup (td->fd, a);

  opts.ignore_unsupported_content = TRUE;

  if (!import_write_and_ref (td->repo, &opts, a, "baz", modifier, &error))
    goto out;

  /* check contents */
  if (!spawn_cmdline ("ostree --repo=repo checkout baz baz-checkout", &error))
    goto out;

  g_assert_cmpint (0, >, getxattr ("baz-checkout/file", "user.data", NULL, 0));
  g_assert_cmpint (ENODATA, ==, errno);

  if (getxattr ("baz-checkout/anotherfile", "user.data", buf, sizeof buf) < 0)
    {
      glnx_set_prefix_error_from_errno (&error, "%s", "getxattr");
      goto out;
    }

  g_assert_cmpstr (buf, ==, "mydata");

 out:
  if (modifier)
    ostree_repo_commit_modifier_unref (modifier);
  g_assert_no_error (error);
}

static GVariant*
path_cb (OstreeRepo  *repo,
         const char  *path,
         GFileInfo   *file_info,
         gpointer     user_data)
{
  if (strcmp (path, "/etc/file") == 0)
    *(gboolean*)user_data = TRUE;
  return NULL;
}

static void
entry_pathname_test_helper (gconstpointer data, gboolean on)
{
  TestData *td = (void*)data; GError *error = NULL;
  ot_cleanup_read_archive struct archive *a = archive_read_new ();
  OstreeRepoImportArchiveOptions opts = { 0, };
  OstreeRepoCommitModifier *modifier = NULL;
  gboolean met_etc_file = FALSE;

  if (skip_if_no_xattr (td))
    goto out;

  modifier = ostree_repo_commit_modifier_new (0, NULL, NULL, NULL);
  ostree_repo_commit_modifier_set_xattr_callback (modifier, path_cb,
                                                  NULL, &met_etc_file);

  test_archive_setup (td->fd, a);

  opts.autocreate_parents = TRUE;
  opts.use_ostree_convention = TRUE;
  opts.ignore_unsupported_content = TRUE;
  opts.callback_with_entry_pathname = on;

  if (!import_write_and_ref (td->repo, &opts, a, "bar", modifier, &error))
    goto out;

  /* the flag shouldn't have any effect on the final tree */
  if (!check_ostree_convention (error))
    goto out;

  if (!on && met_etc_file)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Received callback with /etc/file");
      goto out;
    }

  if (on && !met_etc_file)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Did not receive callback with /etc/file");
      goto out;
    }

  ostree_repo_commit_modifier_unref (modifier);
 out:
  g_assert_no_error (error);
}

static void
test_libarchive_no_use_entry_pathname (gconstpointer data)
{
  entry_pathname_test_helper (data, FALSE);
}

static void
test_libarchive_use_entry_pathname (gconstpointer data)
{
  entry_pathname_test_helper (data, TRUE);
}

static void
test_libarchive_selinux (gconstpointer data)
{
  TestData *td = (void*)data;
  GError *error = NULL;
  ot_cleanup_read_archive struct archive *a = archive_read_new ();
  OstreeRepoImportArchiveOptions opts = { 0 };
  glnx_unref_object OstreeSePolicy *sepol = NULL;
  OstreeRepoCommitModifier *modifier = NULL;
  char buf[64] = { 0 };

  if (skip_if_no_xattr (td))
    goto out;

  {
    glnx_unref_object GFile *root = g_file_new_for_path ("/");

    sepol = ostree_sepolicy_new (root, NULL, NULL);
  }

  if (sepol == NULL || ostree_sepolicy_get_name (sepol) == NULL)
    {
      g_test_skip ("SELinux disabled");
      goto out;
    }

  modifier = ostree_repo_commit_modifier_new (0, NULL, NULL, NULL);
  ostree_repo_commit_modifier_set_sepolicy (modifier, sepol);

  test_archive_setup (td->fd, a);

  opts.ignore_unsupported_content = TRUE;

  if (!import_write_and_ref (td->repo, &opts, a, "bob", modifier, &error))
    goto out;

  /* check contents */
  if (!spawn_cmdline ("ostree --repo=repo checkout bob bob-checkout", &error))
    goto out;

  if (getxattr ("bob-checkout/etc", "security.selinux", buf, sizeof buf) < 0)
    {
      glnx_set_prefix_error_from_errno (&error, "%s", "getxattr");
      goto out;
    }

  buf[(sizeof buf) - 1] = '\0';
  g_assert_cmpstr (buf, ==, "system_u:object_r:etc_t:s0");

 out:
  if (modifier)
    ostree_repo_commit_modifier_unref (modifier);
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
  g_test_add_data_func ("/libarchive/ostree-convention", &td, test_libarchive_ostree_convention);
  g_test_add_data_func ("/libarchive/xattr-callback", &td, test_libarchive_xattr_callback);
  g_test_add_data_func ("/libarchive/no-use-entry-pathname", &td, test_libarchive_no_use_entry_pathname);
  g_test_add_data_func ("/libarchive/use-entry-pathname", &td, test_libarchive_use_entry_pathname);
  g_test_add_data_func ("/libarchive/selinux", &td, test_libarchive_selinux);

  r = g_test_run();

  g_clear_object (&td.repo);
  if (td.tmpd && g_getenv ("TEST_SKIP_CLEANUP") == NULL)
    (void) glnx_shutil_rm_rf_at (AT_FDCWD, td.tmpd, NULL, NULL);
  g_free (td.tmpd);
  return r;
}
