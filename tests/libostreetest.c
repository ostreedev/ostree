/*
 * Copyright (C) 2015 Red Hat, Inc.
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
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <linux/magic.h>
#include <sys/vfs.h>

#include "libglnx.h"
#include "libostreetest.h"

/* This function hovers in a quantum superposition of horrifying and
 * beautiful.  Future generations may interpret it as modern art.
 */
gboolean
ot_test_run_libtest (const char *cmd, GError **error)
{
  const char *srcdir = g_getenv ("G_TEST_SRCDIR");
  g_autoptr(GPtrArray) argv = g_ptr_array_new ();
  g_autoptr(GString) cmdstr = g_string_new ("");

  g_ptr_array_add (argv, "bash");
  g_ptr_array_add (argv, "-c");

  g_string_append (cmdstr, "set -xeuo pipefail; . ");
  g_string_append (cmdstr, srcdir);
  g_string_append (cmdstr, "/tests/libtest.sh; ");
  g_string_append (cmdstr, cmd);

  g_ptr_array_add (argv, cmdstr->str);
  g_ptr_array_add (argv, NULL);

  int estatus;
  if (!g_spawn_sync (NULL, (char**)argv->pdata, NULL, G_SPAWN_SEARCH_PATH,
                     NULL, NULL, NULL, NULL, &estatus, error))
    return FALSE;
  if (!g_spawn_check_exit_status (estatus, error))
    return FALSE;

  return TRUE;
}

OstreeRepo *
ot_test_setup_repo (GCancellable *cancellable,
                    GError **error)
{
  if (!ot_test_run_libtest ("setup_test_repository archive", error))
    return NULL;

  g_autoptr(GFile) repo_path = g_file_new_for_path ("repo");
  g_autoptr(OstreeRepo) ret_repo = ostree_repo_new (repo_path);
  if (!ostree_repo_open (ret_repo, cancellable, error))
    return NULL;

  return g_steal_pointer (&ret_repo);
}

/* Determine whether we're able to relabel files. Needed for bare tests. */
gboolean
ot_check_relabeling (gboolean *can_relabel,
                     GError  **error)
{
  g_auto(GLnxTmpfile) tmpf = { 0, };
  if (!glnx_open_tmpfile_linkable_at (AT_FDCWD, ".", O_RDWR | O_CLOEXEC, &tmpf, error))
    return FALSE;

  g_autoptr(GError) local_error = NULL;
  g_autoptr(GBytes) bytes = glnx_fgetxattr_bytes (tmpf.fd, "security.selinux", &local_error);
  if (!bytes)
    {
      /* libglnx preserves errno. The EOPNOTSUPP case can't be part of a
       * 'case' statement because on most but not all architectures,
       * it's numerically equal to ENOTSUP. */
      if (G_IN_SET (errno, ENOTSUP, ENODATA) || errno == EOPNOTSUPP)
        {
          *can_relabel = FALSE;
          return TRUE;
        }
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  gsize data_len;
  const guint8 *data = g_bytes_get_data (bytes, &data_len);
  if (fsetxattr (tmpf.fd, "security.selinux", data, data_len, 0) < 0)
    {
      if (errno == ENOTSUP || errno == EOPNOTSUPP)
        {
          *can_relabel = FALSE;
          return TRUE;
        }
      return glnx_throw_errno_prefix (error, "fsetxattr");
    }

  *can_relabel = TRUE;
  return TRUE;
}

/* Determine whether the filesystem supports getting/setting user xattrs. */
gboolean
ot_check_user_xattrs (gboolean *has_user_xattrs,
                      GError  **error)
{
  g_auto(GLnxTmpfile) tmpf = { 0, };
  if (!glnx_open_tmpfile_linkable_at (AT_FDCWD, ".", O_RDWR | O_CLOEXEC, &tmpf, error))
    return FALSE;

  if (fsetxattr (tmpf.fd, "user.test", "novalue", strlen ("novalue"), 0) < 0)
    {
      if (errno == ENOTSUP || errno == EOPNOTSUPP)
        {
          *has_user_xattrs = FALSE;
          return TRUE;
        }
      return glnx_throw_errno_prefix (error, "fsetxattr");
    }

  *has_user_xattrs = TRUE;
  return TRUE;
}

OstreeSysroot *
ot_test_setup_sysroot (GCancellable *cancellable,
                       GError **error)
{
  if (!ot_test_run_libtest ("setup_os_repository \"archive\" \"syslinux\"", error))
    return FALSE;

  g_autoptr(GString) buf = g_string_new ("mutable-deployments");

  gboolean can_relabel;
  if (!ot_check_relabeling (&can_relabel, error))
    return FALSE;
  if (!can_relabel)
    {
      g_print ("libostreetest: can't relabel, turning off xattrs\n");
      g_string_append (buf, ",no-xattrs");
    }

  /* Make sure deployments are mutable */
  g_setenv ("OSTREE_SYSROOT_DEBUG", buf->str, TRUE);

  g_autoptr(GFile) sysroot_path = g_file_new_for_path ("sysroot");
  return ostree_sysroot_new (sysroot_path);
}
