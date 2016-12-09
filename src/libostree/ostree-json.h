/*
 * Copyright © 2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#pragma once

#include "ostree-core.h"
#include "ostree-types.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_JSON ostree_json_get_type ()

typedef struct _OstreeJsonProp OstreeJsonProp;

_OSTREE_PUBLIC
G_DECLARE_DERIVABLE_TYPE (OstreeJson, ostree_json, OSTREE, JSON, GObject)

struct _OstreeJsonClass {
  GObjectClass parent_class;

  OstreeJsonProp *props;
  const char *mediatype;
};

_OSTREE_PUBLIC
OstreeJson *ostree_json_from_bytes (GBytes         *bytes,
                                    GType           type,
                                    GError        **error);

_OSTREE_PUBLIC
GBytes     *ostree_json_to_bytes  (OstreeJson  *self);

G_END_DECLS
