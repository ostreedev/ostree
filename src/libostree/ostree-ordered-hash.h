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

typedef struct {
  GPtrArray  *order;
  GHashTable *table;
} OstreeOrderedHash;

OstreeOrderedHash *_ostree_ordered_hash_new (void);
void _ostree_ordered_hash_free (OstreeOrderedHash *ohash);
void _ostree_ordered_hash_cleanup (void *loc);
void _ostree_ordered_hash_replace_key_take (OstreeOrderedHash  *ohash,
                                            char         *key,
                                            const char   *value);
void _ostree_ordered_hash_replace_key (OstreeOrderedHash  *ohash,
                                       const char   *key,
                                       const char   *val);


G_END_DECLS

