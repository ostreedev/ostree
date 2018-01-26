/*
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#include "config.h"

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

gboolean
ostree_builtin_trivial_httpd (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GPtrArray) new_argv = g_ptr_array_new ();

  g_ptr_array_add (new_argv, PKGLIBEXECDIR "/ostree-trivial-httpd");
  for (int i = 1; i < argc; i++)
    g_ptr_array_add (new_argv, argv[i]);
  g_ptr_array_add (new_argv, NULL);
  execvp (new_argv->pdata[0], (char**)new_argv->pdata);
  /* Fall through on error */
  glnx_set_error_from_errno (error);
  return FALSE;
}
