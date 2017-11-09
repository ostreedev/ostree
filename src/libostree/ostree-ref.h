/*
 * Copyright © 2017 Endless Mobile, Inc.
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
 * Authors:
 *  - Philip Withnall <withnall@endlessm.com>
 */

#pragma once

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>

#include "ostree-types.h"

G_BEGIN_DECLS

/**
 * OstreeCollectionRef:
 * @collection_id: (nullable): collection ID which provided the ref, or %NULL if there
 *    is no associated collection
 * @ref_name: ref name
 *
 * A structure which globally uniquely identifies a ref as the tuple
 * (@collection_id, @ref_name). For backwards compatibility, @collection_id may be %NULL,
 * indicating a ref name which is not globally unique.
 *
 * Since: 2017.8
 */
typedef struct
{
  gchar *collection_id;  /* (nullable) */
  gchar *ref_name;  /* (not nullable) */
} OstreeCollectionRef;

_OSTREE_PUBLIC
GType ostree_collection_ref_get_type (void);

_OSTREE_PUBLIC
OstreeCollectionRef *ostree_collection_ref_new (const gchar *collection_id,
                                                const gchar *ref_name);
_OSTREE_PUBLIC
OstreeCollectionRef *ostree_collection_ref_dup (const OstreeCollectionRef *ref);
_OSTREE_PUBLIC
void ostree_collection_ref_free (OstreeCollectionRef *ref);

_OSTREE_PUBLIC
guint ostree_collection_ref_hash (gconstpointer ref);
_OSTREE_PUBLIC
gboolean ostree_collection_ref_equal (gconstpointer ref1,
                                      gconstpointer ref2);

_OSTREE_PUBLIC
OstreeCollectionRef **ostree_collection_ref_dupv (const OstreeCollectionRef * const *refs);
_OSTREE_PUBLIC
void ostree_collection_ref_freev (OstreeCollectionRef **refs);

/**
 * OstreeCollectionRefv:
 *
 * A %NULL-terminated array of #OstreeCollectionRef instances, designed to
 * be used with g_auto():
 *
 * |[<!-- language="C" -->
 * g_auto(OstreeCollectionRefv) refs = NULL;
 * ]|
 *
 * Since: 2017.8
 */
typedef OstreeCollectionRef** OstreeCollectionRefv;

G_END_DECLS
