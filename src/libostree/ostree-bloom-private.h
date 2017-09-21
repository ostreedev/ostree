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

G_BEGIN_DECLS

/**
 * OstreeBloom:
 *
 * An implementation of a [bloom filter](https://en.wikipedia.org/wiki/Bloom_filter)
 * which is suitable for building a filter and looking keys up in an existing
 * filter.
 *
 * Since: 2017.8
 */
typedef struct _OstreeBloom OstreeBloom;

/**
 * OstreeBloomHashFunc:
 * @element: a pointer to the element to hash
 * @k: hash function parameter
 *
 * Function prototype for a
 * [universal hash function](https://en.wikipedia.org/wiki/Universal_hashing),
 * parameterised on @k, which hashes @element to a #guint64 hash value.
 *
 * It is up to the implementer of the hash function whether %NULL is valid for
 * @element.
 *
 * Since: 2017.8
 */
typedef guint64 (*OstreeBloomHashFunc) (gconstpointer element,
                                        guint8        k);

#define OSTREE_TYPE_BLOOM (ostree_bloom_get_type ())

G_GNUC_INTERNAL
GType ostree_bloom_get_type (void);

G_GNUC_INTERNAL
OstreeBloom *ostree_bloom_new (gsize               n_bytes,
                               guint8              k,
                               OstreeBloomHashFunc hash_func);

G_GNUC_INTERNAL
OstreeBloom *ostree_bloom_new_from_bytes (GBytes              *bytes,
                                          guint8               k,
                                          OstreeBloomHashFunc  hash_func);

G_GNUC_INTERNAL
OstreeBloom *ostree_bloom_ref (OstreeBloom *bloom);
G_GNUC_INTERNAL
void ostree_bloom_unref (OstreeBloom *bloom);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeBloom, ostree_bloom_unref)

G_GNUC_INTERNAL
gboolean ostree_bloom_maybe_contains (OstreeBloom   *bloom,
                                      gconstpointer  element);

G_GNUC_INTERNAL
GBytes *ostree_bloom_seal (OstreeBloom *bloom);

G_GNUC_INTERNAL
void ostree_bloom_add_element (OstreeBloom   *bloom,
                               gconstpointer  element);

G_GNUC_INTERNAL
gsize ostree_bloom_get_size (OstreeBloom *bloom);
G_GNUC_INTERNAL
guint8 ostree_bloom_get_k (OstreeBloom *bloom);
G_GNUC_INTERNAL
OstreeBloomHashFunc ostree_bloom_get_hash_func (OstreeBloom *bloom);

G_GNUC_INTERNAL
guint64 ostree_str_bloom_hash (gconstpointer element,
                               guint8        k);

G_END_DECLS
