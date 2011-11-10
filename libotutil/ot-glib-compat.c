/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

#include <gio/gio.h>

#include <string.h>

#include "otutil.h"

#if GLIB_CHECK_VERSION(2,32,0)
/* nothing */
#else
/* Code copied from glib/glib/genviron.c */
/* GLIB - Library of useful routines for C programming
 * Copyright (C) 1995-1998  Peter Mattis, Spencer Kimball and Josh MacDonald
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

/*
 * Modified by the GLib Team and others 1997-2000.  See the AUTHORS
 * file for a list of people on the GLib Team.  See the ChangeLog
 * files for a list of changes.  These files are distributed with
 * GLib at ftp://ftp.gtk.org/pub/gtk/. 
 */

static gint
ot_g_environ_find (gchar       **envp,
                const gchar  *variable)
{
  gint len, i;

  len = strlen (variable);

  for (i = 0; envp[i]; i++)
    {
      if (strncmp (envp[i], variable, len) == 0 &&
          envp[i][len] == '=')
        return i;
    }

  return -1;
}

const gchar *
ot_g_environ_getenv (gchar       **envp,
                  const gchar  *variable)
{
  gint index;

  g_return_val_if_fail (envp != NULL, NULL);
  g_return_val_if_fail (variable != NULL, NULL);

  index = ot_g_environ_find (envp, variable);
  if (index != -1)
    return envp[index] + strlen (variable) + 1;
  else
    return NULL;
}

gchar **
ot_g_environ_setenv (gchar       **envp,
                  const gchar  *variable,
                  const gchar  *value,
                  gboolean      overwrite)
{
  gint index;

  g_return_val_if_fail (envp != NULL, NULL);
  g_return_val_if_fail (variable != NULL, NULL);
  g_return_val_if_fail (strchr (variable, '=') == NULL, NULL);

  index = ot_g_environ_find (envp, variable);
  if (index != -1)
    {
      if (overwrite)
        {
          g_free (envp[index]);
          envp[index] = g_strdup_printf ("%s=%s", variable, value);
        }
    }
  else
    {
      gint length;

      length = g_strv_length (envp);
      envp = g_renew (gchar *, envp, length + 2);
      envp[length] = g_strdup_printf ("%s=%s", variable, value);
      envp[length + 1] = NULL;
    }

  return envp;
}

gchar **
ot_g_environ_unsetenv (gchar       **envp,
                    const gchar  *variable)
{
  gint len;
  gchar **e, **f;

  g_return_val_if_fail (envp != NULL, NULL);
  g_return_val_if_fail (variable != NULL, NULL);
  g_return_val_if_fail (strchr (variable, '=') == NULL, NULL);

  len = strlen (variable);

  /* Note that we remove *all* environment entries for
   * the variable name, not just the first.
   */
  e = f = envp;
  while (*e != NULL)
    {
      if (strncmp (*e, variable, len) != 0 || (*e)[len] != '=')
        {
          *f = *e;
          f++;
        }
      e++;
    }
  *f = NULL;

  return envp;
}

#endif
