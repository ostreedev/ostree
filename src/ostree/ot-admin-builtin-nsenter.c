/*
* Copyright (C) 2024 Colin Walters <walters@verbum.org>
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
 * Author: Misaki Kasumi <misakikasumi@outlook.com>
 */

#include "config.h"

#include "libglnx.h"
#include "ostree.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"

#include <spawn.h>
#include <sys/wait.h>

static gboolean opt_lock;
static gboolean opt_exec;

static GOptionEntry options[] = {
        { "lock", 0, 0, G_OPTION_ARG_NONE, &opt_lock,
          "Make /sysroot writable in the mount namespace and acquire an exclusive multi-process write lock", NULL },
        { "exec", 0, 0, G_OPTION_ARG_NONE, &opt_exec,
          "Replace the process instead of spawning the program as child", NULL},
        { NULL } };

gboolean
ot_admin_builtin_nsenter (int argc, char **argv, OstreeCommandInvocation *invocation,
                         GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = NULL;
  g_autoptr (OstreeSysroot) sysroot = NULL;
  g_autofree char **arguments = NULL;

  context = g_option_context_new ("[PROGRAM [ARGUMENTS...]]");

  int new_argc = 0;
  char **new_argv = NULL;

  for (int i = 1; i < argc; i++)
    {
      if (g_str_equal (argv[i], "--"))
        {
          new_argc = argc - i;
          argc = i;
          new_argv = argv + i;
          argv[i] = NULL;
          break;
        }
    }

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED, invocation, &sysroot,
                                          cancellable, error))
    return FALSE;

  if (new_argv)
    {
      argc = new_argc;
      argv = new_argv;
    }
  if (argc <= 1)
    {
      arguments = g_malloc_n (2, sizeof (char *));
      if ((arguments[0] = getenv ("SHELL")) == NULL)
        arguments[0] = "/bin/sh";
      arguments[1] = NULL;
    }
  else
    {
      arguments = g_malloc_n (argc, sizeof (char *));
      memcpy (arguments, argv + 1, (argc - 1) * sizeof (char *));
      arguments[argc - 1] = NULL;
    }

  if (opt_lock)
    {
      if (opt_exec)
        return glnx_throw (error, "cannot specify both --lock and --exec");
      if (!ostree_sysroot_lock (sysroot, error))
        return FALSE;
    }

  pid_t child_pid;
  if (opt_exec)
    {
      if (execvp (arguments[0], arguments) < 0)
        return glnx_throw_errno_prefix (error, "execvp");
    }
  else
    {
      if (posix_spawnp (&child_pid, arguments[0], NULL, NULL, arguments, environ) != 0)
        return glnx_throw_errno_prefix (error, "posix_spawnp");
    }

  int status;
  while (waitpid (child_pid, &status, 0) < 0)
    {
      if (errno != EINTR)
        return glnx_throw_errno_prefix (error, "waitpid");
    }

  if (opt_lock)
    ostree_sysroot_unlock (sysroot);

  if (!WIFEXITED (status))
    return glnx_throw (error, "child process killed by signal");

  int exit_status = WEXITSTATUS (status);
  if (exit_status != EXIT_SUCCESS)
    exit (exit_status);

  return TRUE;
}
