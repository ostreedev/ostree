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

#include "ostree-blob-reader-raw.h"

struct _OstreeBlobReaderRaw
{
  GDataInputStream parent_instance;
};

static void ostree_blob_reader_raw_iface_init (OstreeBlobReaderInterface *iface);

G_DEFINE_TYPE_WITH_CODE (OstreeBlobReaderRaw, _ostree_blob_reader_raw, G_TYPE_DATA_INPUT_STREAM,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_BLOB_READER,
                                                ostree_blob_reader_raw_iface_init));

static void
ostree_blob_reader_raw_iface_init (OstreeBlobReaderInterface *iface)
{
  iface->read_blob = ostree_blob_reader_raw_read_blob;
}

static void
_ostree_blob_reader_raw_class_init (OstreeBlobReaderRawClass *klass)
{
}

static void
_ostree_blob_reader_raw_init (OstreeBlobReaderRaw *self)
{
}

OstreeBlobReaderRaw *
_ostree_blob_reader_raw_new (GInputStream *stream)
{
  return g_object_new (OSTREE_TYPE_BLOB_READER_RAW, "base-stream", stream, NULL);
}

GBytes *
ostree_blob_reader_raw_read_blob (OstreeBlobReader *self, GCancellable *cancellable, GError **error)
{
  gsize len = 0;
  g_autoptr (GError) local_error = NULL;
  g_autofree char *line
      = g_data_input_stream_read_line (G_DATA_INPUT_STREAM (self), &len, cancellable, &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  if (line == NULL)
    return NULL;

  return g_bytes_new_take (g_steal_pointer (&line), len);
}
