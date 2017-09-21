/*
 * Copyright (C) 2015 Red Hat, Inc.
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

#pragma once

#include <ostree.h>

G_BEGIN_DECLS

gboolean ot_remote_builtin_add (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_remote_builtin_delete (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_remote_builtin_gpg_import (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_remote_builtin_list (int argc, char **argv, GCancellable *cancellable, GError **error);
#ifdef HAVE_LIBSOUP
gboolean ot_remote_builtin_add_cookie (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_remote_builtin_list_cookies (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_remote_builtin_delete_cookie (int argc, char **argv, GCancellable *cancellable, GError **error);
#endif
gboolean ot_remote_builtin_show_url (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_remote_builtin_refs (int argc, char **argv, GCancellable *cancellable, GError **error);
gboolean ot_remote_builtin_summary (int argc, char **argv, GCancellable *cancellable, GError **error);

G_END_DECLS
