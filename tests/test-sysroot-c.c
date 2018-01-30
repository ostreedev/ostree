/*
 * Copyright (C) 2016 Red Hat, Inc.
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
#include <gio/gio.h>
#include <string.h>

#include "libglnx.h"
#include "libostreetest.h"

static gboolean
run_sync (const char *cmdline, GError **error)
{
  int estatus;
  if (!g_spawn_command_line_sync (cmdline, NULL, NULL, &estatus, error))
    return FALSE;
  if (!g_spawn_check_exit_status (estatus, error))
    return FALSE;
  return TRUE;
}

static void
test_sysroot_reload (gconstpointer data)
{
  OstreeSysroot *sysroot = (void*)data;
  g_autoptr(GError) error = NULL;
  gboolean changed;

  if (!ostree_sysroot_load (sysroot, NULL, &error))
    goto out;

  if (!ostree_sysroot_load_if_changed (sysroot, &changed, NULL, &error))
    goto out;
  g_assert (!changed);

  if (!run_sync ("ostree --repo=sysroot/ostree/repo pull-local --remote=testos testos-repo testos/buildmaster/x86_64-runtime", &error))
    goto out;

  if (!run_sync ("ostree admin --sysroot=sysroot deploy --karg=root=LABEL=MOO --karg=quiet --os=testos testos:testos/buildmaster/x86_64-runtime", &error))
    goto out;

  if (!ostree_sysroot_load_if_changed (sysroot, &changed, NULL, &error))
    goto out;
  g_assert (changed);

  if (!ostree_sysroot_load_if_changed (sysroot, &changed, NULL, &error))
    goto out;
  g_assert (!changed);

 out:
  if (error)
    g_error ("%s", error->message);
}

int main (int argc, char **argv)
{
  g_autoptr(GError) error = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;

  g_test_init (&argc, &argv, NULL);

  sysroot = ot_test_setup_sysroot (NULL, &error); 
  if (!sysroot)
    goto out;
  
  g_test_add_data_func ("/sysroot-reload", sysroot, test_sysroot_reload);

  return g_test_run();
 out:
  if (error)
    g_error ("%s", error->message);
  return 1;
}
