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
  "    <method name='Overlay'>"
  "      <arg type='s' name='dir' direction='in'/>"
  "      <arg type='u' name='op_id' direction='out'/>"
  "    </method>"
  "    <method name='Diff'>"
  "      <arg type='s' name='dir' direction='in'/>"
  "      <arg type='h' name='handle' direction='out'/>"
  "    </method>"
  "    <signal name='OperationEnded'>"
  "      <arg type='u' name='op_id' />"
  "      <arg type='b' name='successful' />"
  "      <arg type='s' name='result_str' />"
  "    </signal>"
  "  </interface>"
  "</node>";

static void
operation_new (OstreeDaemon            *self,
               const char              *sender,
               guint                  *out_id,
               OstreeDaemonOperation **out_op)
{
  
  *out_id = ++self->op_id;
  *out_op = g_new0 (OstreeDaemonOperation, 1);
  (*out_op)->requestor_dbus_name = g_strdup (sender);
  (*out_op)->cancellable = g_cancellable_new ();
}

static void
operation_free (OstreeDaemonOperation   *op)
{
  g_free (op->requestor_dbus_name);
  g_object_unref (op->cancellable);
  g_free (op);
}

static gboolean
op_return (OstreeDaemon          *self,
           OstreeDaemonOperation *op,
           GError                *error_return)
{
  gboolean ret = FALSE;
  GVariant *args = NULL;

  g_hash_table_remove (self->ops, GUINT_TO_POINTER (op->id));

  if (error_return)
    args = g_variant_new ("(ubs)", op->id, FALSE, error_return->message);
  else
    args = g_variant_new ("(ubs)", op->id, TRUE, "Success");

  g_dbus_connection_emit_signal (self->bus,
                                 op->requestor_dbus_name,
                                 OSTREE_DAEMON_PATH,
                                 OSTREE_DAEMON_IFACE,
                                 "OperationEnded",
                                 args,
                                 NULL);

  ret = TRUE;
  operation_free (op);
  ot_clear_gvariant (&args);
  return ret;
}

typedef struct {
  OstreeDaemonOperation *op;
  GFile *dir;
} OverlayDirThreadData;

typedef struct {
  OverlayDirThreadData *tdata;
  GError *error;
} OverlayDirEmitInIdleData;

static gboolean
overlay_dir_emit_in_idle (gpointer data)
{
  OverlayDirEmitInIdleData *idledata = data;

  op_return (idledata->tdata->op->daemon,
             idledata->tdata->op,
             idledata->error);
             
  g_free (idledata);
  
  return FALSE;
}

static gpointer
overlay_dir_thread (gpointer data)
{
  OverlayDirThreadData *tdata = data;
  GMainContext *context = NULL;
  GFile *sysroot_f = NULL;
  OverlayDirEmitInIdleData *idledata = g_new0 (OverlayDirEmitInIdleData, 1);
 
  idledata->tdata = tdata;

  context = g_main_context_new ();

  sysroot_f = ot_gfile_new_for_path ("/sysroot/ostree/current");

  g_main_context_push_thread_default (context);

  (void)ot_gfile_merge_dirs (sysroot_f,
                             tdata->dir,
                             tdata->op->cancellable,
                             &(idledata->error));
  g_idle_add (overlay_dir_emit_in_idle, idledata);
  
  g_main_context_pop_thread_default (context);

  g_main_context_unref (context);

  g_clear_object (&tdata->dir);
  g_free (tdata);
  
  return NULL;
}

static void
do_op_overlay (OstreeDaemon            *self,
               const char              *dir,
               OstreeDaemonOperation   *op)
{
  OverlayDirThreadData *tdata = g_new0 (OverlayDirThreadData, 1);
  
  tdata->op = op;
  tdata->dir = ot_gfile_new_for_path (dir);

  g_thread_create_full (overlay_dir_thread, tdata, 0, FALSE, FALSE,
                        G_THREAD_PRIORITY_NORMAL, NULL);
}

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
  OstreeDaemon *self = user_data;
  guint32 op_id;
  OstreeDaemonOperation *op;

  if (g_strcmp0 (method_name, "Overlay") == 0)
    {
      const gchar *dirpath;

      g_variant_get (parameters, "(&s)", &dirpath);

      operation_new (self, sender, &op_id, &op);

      do_op_overlay (self, dirpath, op);

      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(u)", op_id));
    }
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
  char *repo_path;

  repo_path = g_build_filename (ot_gfile_get_path_cached (self->prefix), "repo", NULL);
  self->repo = ostree_repo_new (repo_path);
  g_free (repo_path);
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
    self->prefix = ot_gfile_new_for_path (config->dummy_test_path);
  else
    self->prefix = ot_gfile_new_for_path ("/sysroot/ostree");
      
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
