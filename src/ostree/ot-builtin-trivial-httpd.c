/*
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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
