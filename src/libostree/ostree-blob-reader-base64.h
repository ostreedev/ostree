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

#pragma once

#include "ostree-blob-reader.h"

G_BEGIN_DECLS

#define OSTREE_TYPE_BLOB_READER_BASE64 (_ostree_blob_reader_base64_get_type ())

_OSTREE_PUBLIC
G_DECLARE_FINAL_TYPE (OstreeBlobReaderBase64, _ostree_blob_reader_base64, OSTREE,
                      BLOB_READER_BASE64, GDataInputStream);

_OSTREE_PUBLIC
OstreeBlobReaderBase64 *_ostree_blob_reader_base64_new (GInputStream *stream);

_OSTREE_PUBLIC
GBytes *ostree_blob_reader_base64_read_blob (OstreeBlobReader *self, GCancellable *cancellable,
                                             GError **error);

G_END_DECLS
