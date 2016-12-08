/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <string.h>
#include <glib-unix.h>

#include "ot-main.h"
#include "ot-admin-instutil-builtins.h"
#include "ostree-cmdprivate.h"

#include "otutil.h"

static GOptionEntry options[] = {
  { NULL }
};

gboolean
ot_admin_instutil_builtin_grub2_generate (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  guint bootversion;
  g_autoptr(GOptionContext) context = NULL;
  glnx_unref_object OstreeSysroot *sysroot = NULL;

  context = g_option_context_new ("[BOOTVERSION] - generate GRUB2 configuration from given BLS entries");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER | OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED,
                                          &sysroot, cancellable, error))
    goto out;

  if (!ostree_sysroot_load (sysroot, cancellable, error))
    goto out;

  if (argc >= 2)
    {
      bootversion = (guint) g_ascii_strtoull (argv[1], NULL, 10);
      if (!(bootversion == 0 || bootversion == 1))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid bootversion: %u", bootversion);
      goto out;
        }
    }
  else 
    {
      const char *bootversion_env = g_getenv ("_OSTREE_GRUB2_BOOTVERSION");
      if (bootversion_env)
        bootversion = g_ascii_strtoull (bootversion_env, NULL, 10);
      else
        bootversion = ostree_sysroot_get_bootversion (sysroot);
      g_assert (bootversion == 0 || bootversion == 1);
    }

  if (!ostree_cmd__private__()->ostree_generate_grub2_config (sysroot, bootversion, 1, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
