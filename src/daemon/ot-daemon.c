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
  OstreeDaemon *self = user_data;
  guint id;

  self->bus = g_object_ref (connection);

  if (introspection_data == NULL)
    introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);

  id = g_dbus_connection_register_object (connection,
                                          OSTREE_DAEMON_PATH,
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
  OstreeDaemon *self = user_data;
  GError *error = NULL;
  GFile *repo_file = NULL;

  repo_file = g_file_get_child (self->prefix, "repo");
  self->repo = ostree_repo_new (repo_file);
  g_clear_object (&repo_file);
  if (!ostree_repo_check (self->repo, &error))
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
  OstreeDaemon *self = g_new0 (OstreeDaemon, 1);

  self->loop = g_main_loop_new (NULL, TRUE);
  self->ops = g_hash_table_new_full (g_int_hash, g_int_equal, NULL, NULL);

  return self;
}

void
ostree_daemon_free (OstreeDaemon  *self)
{
  g_main_loop_unref (self->loop);
  g_hash_table_unref (self->ops);
  g_free (self);
}

gboolean
ostree_daemon_config (OstreeDaemon *self,
                      OstreeDaemonConfig *config,
                      GError        **error)
{
  gboolean ret = FALSE;
  gboolean is_dummy = config->dummy_test_path != NULL;

  if (!is_dummy)
    {
      if (getuid () != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "This program must be run as root");
          goto out;
        }
    }

  if (is_dummy)
    self->prefix = g_file_new_for_path (config->dummy_test_path);
  else
    self->prefix = g_file_new_for_path ("/sysroot/ostree");
      
  self->name_id = g_bus_own_name (is_dummy ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM,
                                  OSTREE_DAEMON_NAME,
                                  G_BUS_NAME_OWNER_FLAGS_NONE,
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  self,
                                  NULL);
  ret = TRUE;
 out:
  return ret;
}
