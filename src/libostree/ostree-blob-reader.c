/*
 * Copyright (C) 2024 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ostree-blob-reader.h"

G_DEFINE_INTERFACE (OstreeBlobReader, ostree_blob_reader, G_TYPE_OBJECT);

static void
ostree_blob_reader_default_init (OstreeBlobReaderInterface *iface)
{
  g_debug ("OstreeBlobReader initialization");
}

/**
 * ostree_blob_reader_read_blob
 * @self: A OstreeBlobReader
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Read one blob from the reader, or %NULL if there are no more.
 * On error, @error is set and %NULL is returned.
 *
 * Returns: (nullable): A #GBytes blob, or %NULL if there are no more
 *
 * Since: 2016.5
 */
GBytes *
ostree_blob_reader_read_blob (OstreeBlobReader *self, GCancellable *cancellable, GError **error)
{
  g_assert (OSTREE_IS_BLOB_READER (self));
  return OSTREE_BLOB_READER_GET_IFACE (self)->read_blob (self, cancellable, error);
}
