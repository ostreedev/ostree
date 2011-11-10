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

#ifndef __OSBUILD_BUILTINS__
#define __OSBUILD_BUILTINS__

#include <glib-object.h>

G_BEGIN_DECLS

typedef enum {
  OSBUILD_BUILTIN_FLAG_NONE = 0,
} OsbuildBuiltinFlags;

typedef struct {
  const char *name;
  gboolean (*fn) (int argc, char **argv, const char *prefix, GError **error);
  int flags; /* OsbuildBuiltinFlags */
} OsbuildBuiltin;

gboolean osbuild_builtin_buildone (int argc, char **argv, const char *prefix, GError **error);

G_END_DECLS

#endif
