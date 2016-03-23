/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>.
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
 * Author: Colin Walters <walters@verbum.org>
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

guchar *ot_csum_from_gchecksum (GChecksum *checksum);

gboolean ot_gio_write_update_checksum (GOutputStream  *out,
                                       gconstpointer   data,
                                       gsize           len,
                                       gsize          *out_bytes_written,
                                       GChecksum      *checksum,
                                       GCancellable   *cancellable,
                                       GError        **error);

gboolean ot_gio_splice_get_checksum (GOutputStream  *out,
                                     GInputStream   *in,
                                     guchar        **out_csum,
                                     GCancellable   *cancellable,
                                     GError        **error);

gboolean ot_gio_splice_update_checksum (GOutputStream  *out,
                                        GInputStream   *in,
                                        GChecksum      *checksum,
                                        GCancellable   *cancellable,
                                        GError        **error);

gboolean ot_gio_checksum_stream (GInputStream   *in,
                                 guchar        **out_csum,
                                 GCancellable   *cancellable,
                                 GError        **error);

char * ot_checksum_file_at (int             dfd,
                            const char     *path,
                            GChecksumType   checksum_type,
                            GCancellable   *cancellable,
                            GError        **error);

void ot_gio_checksum_stream_async (GInputStream         *in,
                                   int                   io_priority,
                                   GCancellable         *cancellable,
                                   GAsyncReadyCallback   callback,
                                   gpointer              user_data);

guchar * ot_gio_checksum_stream_finish (GInputStream   *in,
                                        GAsyncResult   *result,
                                        GError        **error);

G_END_DECLS
