/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define OSTREE_TYPE_BOOTCONFIG_PARSER (ostree_bootconfig_parser_get_type ())
#define OSTREE_BOOTCONFIG_PARSER(inst) (G_TYPE_CHECK_INSTANCE_CAST ((inst), OSTREE_TYPE_BOOTCONFIG_PARSER, OstreeBootconfigParser))
#define OSTREE_IS_BOOTCONFIG_PARSER(inst) (G_TYPE_CHECK_INSTANCE_TYPE ((inst), OSTREE_TYPE_BOOTCONFIG_PARSER))

typedef struct _OstreeBootconfigParser OstreeBootconfigParser;

_OSTREE_PUBLIC
GType ostree_bootconfig_parser_get_type (void) G_GNUC_CONST;

_OSTREE_PUBLIC
OstreeBootconfigParser * ostree_bootconfig_parser_new (void);

_OSTREE_PUBLIC
OstreeBootconfigParser * ostree_bootconfig_parser_clone (OstreeBootconfigParser *self);

_OSTREE_PUBLIC
gboolean ostree_bootconfig_parser_parse (OstreeBootconfigParser  *self,
                                         GFile           *path,
                                         GCancellable    *cancellable,
                                         GError         **error);

_OSTREE_PUBLIC
gboolean ostree_bootconfig_parser_parse_at (OstreeBootconfigParser  *self,
                                            int                      dfd,
                                            const char              *path,
                                            GCancellable    *cancellable,
                                            GError         **error);

_OSTREE_PUBLIC
gboolean ostree_bootconfig_parser_write (OstreeBootconfigParser   *self,
                                         GFile            *output,
                                         GCancellable     *cancellable,
                                         GError          **error);

_OSTREE_PUBLIC
gboolean ostree_bootconfig_parser_write_at (OstreeBootconfigParser   *self,
                                            int                       dfd,
                                            const char               *path,
                                            GCancellable             *cancellable,
                                            GError                  **error);

_OSTREE_PUBLIC
void ostree_bootconfig_parser_set (OstreeBootconfigParser  *self,
                                   const char      *key,
                                   const char      *value);

_OSTREE_PUBLIC
const char *ostree_bootconfig_parser_get (OstreeBootconfigParser  *self,
                                          const char      *key);


G_END_DECLS
