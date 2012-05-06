/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>.
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

#ifndef __OSTREE_SPAWN_UTILS_H__
#define __OSTREE_SPAWN_UTILS_H__

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean ot_spawn_sync_checked (const char           *cwd,
                                char                **argv,
                                char                **envp,
                                GSpawnFlags           flags,
                                GSpawnChildSetupFunc  child_setup,
                                gpointer              user_data,
                                char                **stdout_data,
                                char                **stderr_data,
                                GError              **error);

G_END_DECLS

#endif
