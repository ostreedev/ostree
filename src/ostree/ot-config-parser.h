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

#ifndef __OT_CONFIG_PARSER_H__
#define __OT_CONFIG_PARSER_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#define OT_TYPE_CONFIG_PARSER (ot_config_parser_get_type ())
#define OT_CONFIG_PARSER(inst) (G_TYPE_CHECK_INSTANCE_CAST ((inst), OT_TYPE_CONFIG_PARSER, OtConfigParser))
#define OT_IS_CONFIG_PARSER(inst) (G_TYPE_CHECK_INSTANCE_TYPE ((inst), OT_TYPE_CONFIG_PARSER))

typedef struct _OtConfigParser OtConfigParser;

GType ot_config_parser_get_type (void) G_GNUC_CONST;

OtConfigParser * ot_config_parser_new (const char *separator);

gboolean ot_config_parser_parse (OtConfigParser  *self,
                                 GFile           *path,
                                 GCancellable    *cancellable,
                                 GError         **error);

gboolean ot_config_parser_write (OtConfigParser   *self,
                                 GFile            *output,
                                 GCancellable     *cancellable,
                                 GError          **error);

void ot_config_parser_set (OtConfigParser  *self,
                           const char      *key,
                           const char      *value);

const char *ot_config_parser_get (OtConfigParser  *self,
                                  const char      *key);


G_END_DECLS

#endif /* __OT_CONFIG_PARSER_H__ */
