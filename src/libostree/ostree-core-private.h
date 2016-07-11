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
 * u - rdev (must be 0)
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
 * u - rdev (must be 0)
 * s - symlink target 
 * a(ayay) - xattrs
 * ---
 * zlib-compressed data
 */
#define _OSTREE_ZLIB_FILE_HEADER_GVARIANT_FORMAT G_VARIANT_TYPE ("(tuuuusa(ayay))")


GVariant *_ostree_file_header_new (GFileInfo         *file_info,
                                   GVariant          *xattrs);

GVariant *_ostree_zlib_file_header_new (GFileInfo         *file_info,
                                        GVariant          *xattrs);

gboolean _ostree_write_variant_with_size (GOutputStream      *output,
                                          GVariant           *variant,
                                          guint64             alignment_offset,
                                          gsize              *out_bytes_written,
                                          GChecksum          *checksum,
                                          GCancellable       *cancellable,
                                          GError            **error);

gboolean
_ostree_make_temporary_symlink_at (int             tmp_dirfd,
                                   const char     *target,
                                   char          **out_name,
                                   GCancellable   *cancellable,
                                   GError        **error);

GFileInfo * _ostree_header_gfile_info_new (mode_t mode, uid_t uid, gid_t gid);

/* XX/checksum-2.extension, but let's just use 256 for a
 * bit of overkill.
 */
#define _OSTREE_LOOSE_PATH_MAX (256)

char *
_ostree_get_relative_object_path (const char        *checksum,
                                  OstreeObjectType   type,
                                  gboolean           compressed);


char *
_ostree_get_relative_static_delta_path (const char        *from,
                                        const char        *to,
                                        const char        *target);

char *
_ostree_get_relative_static_delta_superblock_path (const char        *from,
                                                   const char        *to);

char *
_ostree_get_relative_static_delta_detachedmeta_path (const char        *from,
                                                     const char        *to);

char *
_ostree_get_relative_static_delta_part_path (const char        *from,
                                             const char        *to,
                                             guint              i);

static inline char * _ostree_get_commitpartial_path (const char *checksum)
{
  return g_strconcat ("state/", checksum, ".commitpartial", NULL);
}

void
_ostree_parse_delta_name (const char  *delta_name,
                          char        **out_from,
                          char        **out_to);

void
_ostree_loose_path (char              *buf,
                    const char        *checksum,
                    OstreeObjectType   objtype,
                    OstreeRepoMode     repo_mode);

#define _OSTREE_METADATA_GPGSIGS_NAME "ostree.gpgsigs"
#define _OSTREE_METADATA_GPGSIGS_TYPE G_VARIANT_TYPE ("aay")

GVariant *
_ostree_detached_metadata_append_gpg_sig (GVariant   *existing_metadata,
                                          GBytes     *signature_bytes);

GFile *
_ostree_get_default_sysroot_path (void);

G_END_DECLS
