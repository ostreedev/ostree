/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <sys/wait.h>

#include "ostree.h"
#include "otutil.h"

static const char * const sysroot_environ[] = {
  "HOME=/",
  "PWD=/",
  "HOSTNAME=ostreesysroot",
  "LANG=C",
  "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
  "SHELL=/bin/bash",
  "TERM=vt100",
  "TMPDIR=/tmp",
  "TZ=EST5EDT",
  NULL
};

const char *const*
ostree_get_sysroot_environ (void)
{
  return (const char *const*)sysroot_environ;
}

/**
 * @root: (allow-none): Change to this root; if %NULL, don't chroot
 *
 * Triggers are a set of programs to run on a root to regenerate cache
 * files.  This API call will simply run them against the given root.
 */
gboolean
ostree_run_triggers_in_root (GFile                  *root,
                             GCancellable           *cancellable,
                             GError                **error)
{
  gboolean ret = FALSE;
  int estatus;
  gs_free char *rel_triggerdir = NULL;
  gs_unref_object GFile *triggerdir = NULL;
  gs_unref_ptrarray GPtrArray *argv = NULL;

  rel_triggerdir = g_build_filename ("usr", "libexec", "ostree", "triggers.d", NULL);

  if (root)
    triggerdir = g_file_resolve_relative_path (root, rel_triggerdir);
  else
    triggerdir = g_file_new_for_path (rel_triggerdir);

  if (g_file_query_exists (triggerdir, cancellable))
    {
      argv = g_ptr_array_new ();
      if (root)
        {
          g_ptr_array_add (argv, "linux-user-chroot");
          g_ptr_array_add (argv, "--unshare-pid");
          g_ptr_array_add (argv, "--unshare-ipc");
          /* FIXME - unshare net too */
          g_ptr_array_add (argv, "--mount-proc");
          g_ptr_array_add (argv, "/proc");
          g_ptr_array_add (argv, "--mount-bind");
          g_ptr_array_add (argv, "/dev");
          g_ptr_array_add (argv, "/dev");
          g_ptr_array_add (argv, (char*)gs_file_get_path_cached (root));
        }
      g_ptr_array_add (argv, "ostree-run-triggers");
      g_ptr_array_add (argv, NULL);

      if (!g_spawn_sync (NULL, (char**)argv->pdata,
                         (char**) ostree_get_sysroot_environ (),
                         G_SPAWN_SEARCH_PATH,
                         NULL, NULL, NULL, NULL, &estatus, error))
        goto out;

      if (!g_spawn_check_exit_status (estatus, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}
