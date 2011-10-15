/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#ifndef __HACKTREE_BUILTINS__
#define __HACKTREE_BUILTINS__

#include <glib-object.h>

G_BEGIN_DECLS

typedef enum {
  HACKTREE_BUILTIN_FLAG_NONE = 0,
} HacktreeBuiltinFlags;

typedef struct {
  const char *name;
  gboolean (*fn) (int argc, char **argv, const char *prefix, GError **error);
  int flags; /* HacktreeBuiltinFlags */
} HacktreeBuiltin;

gboolean hacktree_builtin_checkout (int argc, char **argv, const char *prefix, GError **error);
gboolean hacktree_builtin_commit (int argc, char **argv, const char *prefix, GError **error);
gboolean hacktree_builtin_init (int argc, char **argv, const char *prefix, GError **error);
gboolean hacktree_builtin_log (int argc, char **argv, const char *prefix, GError **error);
gboolean hacktree_builtin_link_file (int argc, char **argv, const char *prefix, GError **error);
gboolean hacktree_builtin_fsck (int argc, char **argv, const char *prefix, GError **error);
gboolean hacktree_builtin_show (int argc, char **argv, const char *prefix, GError **error);

G_END_DECLS

#endif
