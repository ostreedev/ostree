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

GType ostree_bootconfig_parser_get_type (void) G_GNUC_CONST;

OstreeBootconfigParser * ostree_bootconfig_parser_new (void);

OstreeBootconfigParser * ostree_bootconfig_parser_clone (OstreeBootconfigParser *self);

gboolean ostree_bootconfig_parser_parse (OstreeBootconfigParser  *self,
                                         GFile           *path,
                                         GCancellable    *cancellable,
                                         GError         **error);

gboolean ostree_bootconfig_parser_write (OstreeBootconfigParser   *self,
                                         GFile            *output,
                                         GCancellable     *cancellable,
                                         GError          **error);

void ostree_bootconfig_parser_set (OstreeBootconfigParser  *self,
                                   const char      *key,
                                   const char      *value);

const char *ostree_bootconfig_parser_get (OstreeBootconfigParser  *self,
                                          const char      *key);


G_END_DECLS
