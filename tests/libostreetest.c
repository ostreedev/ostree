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

OstreeSysroot *
ot_test_setup_sysroot (GCancellable *cancellable,
                       GError **error)
{
  if (!ot_test_run_libtest ("setup_os_repository \"archive\" \"syslinux\"", error))
    return FALSE;

  struct statfs stbuf;
  { g_autoptr(GString) buf = g_string_new ("mutable-deployments");
    if (statfs ("/", &stbuf) < 0)
      return glnx_null_throw_errno (error);
    /* Keep this in sync with the overlayfs bits in libtest.sh */
#ifndef OVERLAYFS_SUPER_MAGIC
#define OVERLAYFS_SUPER_MAGIC 0x794c7630
#endif
    if (stbuf.f_type == OVERLAYFS_SUPER_MAGIC)
      {
        g_print ("libostreetest: detected overlayfs\n");
        g_string_append (buf, ",no-xattrs");
      }
    /* Make sure deployments are mutable */
    g_setenv ("OSTREE_SYSROOT_DEBUG", buf->str, TRUE);
  }

  g_autoptr(GFile) sysroot_path = g_file_new_for_path ("sysroot");
  return ostree_sysroot_new (sysroot_path);
}
