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
#include <gio/gunixfdlist.h>

#include <string.h>
#include <stdlib.h>

#include "ostree.h"
#include "ot-daemon.h"

static GDBusNodeInfo *introspection_data = NULL;

static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.gnome.OSTree'>"
  "    <method name='CancelOperation'>"
  "      <arg type='u' name='id' direction='in'/>"
  "    </method>"
  "    <method name='AddBootRoot'>"
  "      <arg type='s' name='revision' direction='in'/>"
  "      <arg type='u' name='op_id' direction='out'/>"
  "    </method>"
  "    <method name='RemoveBootRoot'>"
  "      <arg type='s' name='revision' direction='in'/>"
  "      <arg type='u' name='op_id' direction='out'/>"
  "    </method>"
  "    <method name='SetBootRoot'>"
  "      <arg type='s' name='revision' direction='in'/>"
  "      <arg type='u' name='op_id' direction='out'/>"
  "    </method>"
  "    <method name='OverlayTar'>"
  "      <arg type='s' name='filename' direction='in'/>"
  "      <arg type='u' name='op_id' direction='out'/>"
  "    </method>"
  "  </interface>"
  "</node>";
static void
handle_method_call (GDBusConnection       *connection,
                    const gchar           *sender,
                    const gchar           *object_path,
                    const gchar           *interface_name,
                    const gchar           *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
}

static const GDBusInterfaceVTable interface_vtable =
{
  handle_method_call,
  NULL,
  NULL 
};

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  guint id;

  id = g_dbus_connection_register_object (connection,
                                          "/org/gnome/OSTree",
                                          introspection_data->interfaces[0],
                                          &interface_vtable,
                                          NULL,  /* user_data */
                                          NULL,  /* user_data_free_func */
                                          NULL); /* GError** */
  g_assert (id > 0);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  OstreeDaemon *daemon = user_data;
  GError *error = NULL;

  daemon->repo = ostree_repo_new ("/sysroot/ostree/repo");
  if (!ostree_repo_check (daemon->repo, &error))
    {
      g_printerr ("%s\n", error->message);
      g_clear_error (&error);
      exit (1);
    }
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  exit (1);
}


OstreeDaemon *
ostree_daemon_new (void)
{
  OstreeDaemon *ret = g_new0 (OstreeDaemon, 1);

  ret->loop = g_main_loop_new (NULL, TRUE);

  introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);

  ret->name_id = g_bus_own_name (G_BUS_TYPE_SYSTEM,
                                 "org.gnome.OSTree",
                                 G_BUS_NAME_OWNER_FLAGS_NONE,
                                 on_bus_acquired,
                                 on_name_acquired,
                                 on_name_lost,
                                 NULL,
                                 NULL);

  return ret;
}

