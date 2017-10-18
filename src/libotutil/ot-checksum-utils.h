/*
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

#include "libglnx.h"

G_BEGIN_DECLS

void ot_bin2hex (char *out_buf, const guint8 *inbuf, gsize len);

guchar *ot_csum_from_gchecksum (GChecksum *checksum);

struct OtChecksum {
  gboolean initialized;
  guint uints[2];
  gpointer data[2];
};
typedef struct OtChecksum OtChecksum;

/* Same as OSTREE_SHA256_DIGEST_LEN, but this header can't depend on that */
#define _OSTREE_SHA256_DIGEST_LEN (32)
#if defined(OSTREE_SHA256_DIGEST_LEN) && _OSTREE_SHA256_DIGEST_LEN != OSTREE_SHA256_DIGEST_LEN
#error Mismatched OSTREE_SHA256_DIGEST_LEN
#endif
/* See above */
#define _OSTREE_SHA256_STRING_LEN (64)
#if defined(OSTREE_SHA256_STRING_LEN) && _OSTREE_SHA256_STRING_LEN != OSTREE_SHA256_STRING_LEN
#error Mismatched OSTREE_SHA256_STRING_LEN
#endif

void ot_checksum_init (OtChecksum *checksum);
void ot_checksum_update (OtChecksum *checksum,
                         const guint8   *buf,
                         size_t          len);
static inline void
ot_checksum_update_bytes (OtChecksum *checksum,
                          GBytes     *buf)
{
  gsize len;
  const guint8 *bufdata = g_bytes_get_data (buf, &len);
  ot_checksum_update (checksum, bufdata, len);
}
void ot_checksum_get_digest (OtChecksum *checksum,
                             guint8      *buf,
                             size_t       buflen);
void ot_checksum_get_hexdigest (OtChecksum *checksum,
                                char           *buf,
                                size_t          buflen);
void ot_checksum_clear (OtChecksum *checksum);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(OtChecksum, ot_checksum_clear)

gboolean ot_gio_write_update_checksum (GOutputStream  *out,
                                       gconstpointer   data,
                                       gsize           len,
                                       gsize          *out_bytes_written,
                                       OtChecksum     *checksum,
                                       GCancellable   *cancellable,
                                       GError        **error);

gboolean ot_gio_splice_get_checksum (GOutputStream  *out,
                                     GInputStream   *in,
                                     guchar        **out_csum,
                                     GCancellable   *cancellable,
                                     GError        **error);

gboolean ot_gio_splice_update_checksum (GOutputStream  *out,
                                        GInputStream   *in,
                                        OtChecksum     *checksum,
                                        GCancellable   *cancellable,
                                        GError        **error);

char * ot_checksum_file_at (int             dfd,
                            const char     *path,
                            GChecksumType   checksum_type,
                            GCancellable   *cancellable,
                            GError        **error);

G_END_DECLS
