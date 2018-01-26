/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
