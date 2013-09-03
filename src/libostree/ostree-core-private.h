/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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
 */

#pragma once

#include "ostree-core.h"

G_BEGIN_DECLS

/* This file contains private implementation data format definitions
 * read by multiple implementation .c files.
 */

/*
 * File objects are stored as a stream, with one #GVariant header,
 * followed by content.
 * 
 * The file header is of the following form:
 *
 * &lt;BE guint32 containing variant length&gt;
 * u - uid
 * u - gid
 * u - mode
 * u - rdev
 * s - symlink target 
 * a(ayay) - xattrs
 *
 * Then the rest of the stream is data.
 */
#define _OSTREE_FILE_HEADER_GVARIANT_FORMAT G_VARIANT_TYPE ("(uuuusa(ayay))")

/*
 * A variation on %OSTREE_FILE_HEADER_GVARIANT_FORMAT, used for
 * storing zlib-compressed content objects.
 *
 * &lt;BE guint32 containing variant length&gt;
 * t - size
 * u - uid
 * u - gid
 * u - mode
 * u - rdev
 * s - symlink target 
 * a(ayay) - xattrs
 * ---
 * zlib-compressed data
 */
#define _OSTREE_ZLIB_FILE_HEADER_GVARIANT_FORMAT G_VARIANT_TYPE ("(tuuuusa(ayay))")

GVariant *_ostree_zlib_file_header_new (GFileInfo         *file_info,
                                        GVariant          *xattrs);

G_END_DECLS

