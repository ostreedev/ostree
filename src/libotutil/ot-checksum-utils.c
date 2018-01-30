/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "otutil.h"
#if defined(HAVE_OPENSSL)
#include <openssl/evp.h>
#elif defined(HAVE_GNUTLS)
#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>
#endif

#include <string.h>

void
ot_bin2hex (char *out_buf, const guint8 *inbuf, gsize len)
{
  static const gchar hexchars[] = "0123456789abcdef";
  guint i, j;

  for (i = 0, j = 0; i < len; i++, j += 2)
    {
      guchar byte = inbuf[i];
      out_buf[j] = hexchars[byte >> 4];
      out_buf[j+1] = hexchars[byte & 0xF];
    }
  out_buf[j] = '\0';
}

/* I like to think of this as AbstractChecksumProxyFactoryBean. In homage to
 * https://news.ycombinator.com/item?id=4549544
 * aka http://static.springsource.org/spring/docs/2.5.x/api/org/springframework/aop/framework/AbstractSingletonProxyFactoryBean.html
 */
typedef struct {
  gboolean initialized;
#if defined(HAVE_OPENSSL)
  EVP_MD_CTX *checksum;
#elif defined(HAVE_GNUTLS)
  gnutls_hash_hd_t checksum;
#else
  GChecksum *checksum;
#endif
  guint digest_len;
} OtRealChecksum;

G_STATIC_ASSERT (sizeof (OtChecksum) >= sizeof (OtRealChecksum));

void
ot_checksum_init (OtChecksum *checksum)
{
  OtRealChecksum *real = (OtRealChecksum*)checksum;
  g_return_if_fail (!real->initialized);
#if defined(HAVE_OPENSSL)
  real->checksum = EVP_MD_CTX_create ();
  g_assert (real->checksum);
  g_assert (EVP_DigestInit_ex (real->checksum, EVP_sha256 (), NULL));
  real->digest_len = EVP_MD_CTX_size (real->checksum);
#elif defined(HAVE_GNUTLS)
  g_assert (!gnutls_hash_init (&real->checksum, GNUTLS_DIG_SHA256));
  real->digest_len = gnutls_hash_get_len (GNUTLS_DIG_SHA256);
#else
  real->checksum = g_checksum_new (G_CHECKSUM_SHA256);
  real->digest_len = g_checksum_type_get_length (G_CHECKSUM_SHA256);
#endif
  g_assert_cmpint (real->digest_len, ==, _OSTREE_SHA256_DIGEST_LEN);
  real->initialized = TRUE;
}

void
ot_checksum_update (OtChecksum *checksum,
                    const guint8   *buf,
                    size_t          len)
{
  OtRealChecksum *real = (OtRealChecksum*)checksum;
  g_return_if_fail (real->initialized);
#if defined(HAVE_OPENSSL)
  g_assert (EVP_DigestUpdate (real->checksum, buf, len));
#elif defined(HAVE_GNUTLS)
  g_assert (!gnutls_hash (real->checksum, buf, len));
#else
  g_checksum_update (real->checksum, buf, len);
#endif
}

static void
ot_checksum_get_digest_internal (OtRealChecksum *real,
                                 guint8      *buf,
                                 size_t       buflen)
{
  g_return_if_fail (real->initialized);
  g_assert_cmpint (buflen, ==, _OSTREE_SHA256_DIGEST_LEN);
#if defined(HAVE_OPENSSL)
  guint digest_len = buflen;
  g_assert (EVP_DigestFinal_ex (real->checksum, buf, &digest_len));
  g_assert_cmpint (digest_len, ==, buflen);
#elif defined(HAVE_GNUTLS)
  gnutls_hash_output (real->checksum, buf);
#else
  gsize digest_len = buflen;
  g_checksum_get_digest (real->checksum, buf, &digest_len);
  g_assert_cmpint (digest_len, ==, buflen);
#endif
}

void
ot_checksum_get_digest (OtChecksum *checksum,
                        guint8      *buf,
                        size_t       buflen)
{
  OtRealChecksum *real = (OtRealChecksum*)checksum;
  ot_checksum_get_digest_internal (real, buf, buflen);
  real->initialized = FALSE;
}

void
ot_checksum_get_hexdigest (OtChecksum *checksum,
                           char           *buf,
                           size_t          buflen)
{
  OtRealChecksum *real = (OtRealChecksum*)checksum;
  const guint digest_len = real->digest_len;
  guint8 digest_buf[digest_len];
  ot_checksum_get_digest (checksum, digest_buf, digest_len);
  ot_bin2hex (buf, (guint8*)digest_buf, digest_len);
  real->initialized = FALSE;
}

void
ot_checksum_clear (OtChecksum *checksum)
{
  OtRealChecksum *real = (OtRealChecksum*)checksum;
  if (!real->initialized)
    return;
#if defined(HAVE_OPENSSL)
  EVP_MD_CTX_destroy (real->checksum);
#elif defined(HAVE_GNUTLS)
  gnutls_hash_deinit (real->checksum, NULL);
#else
  g_checksum_free (real->checksum);
#endif
  real->initialized = FALSE;
}

guchar *
ot_csum_from_gchecksum (GChecksum  *checksum)
{
  guchar *ret = g_malloc (32);
  gsize len = 32;

  g_checksum_get_digest (checksum, ret, &len);
  g_assert (len == 32);
  return ret;
}

gboolean
ot_gio_write_update_checksum (GOutputStream  *out,
                              gconstpointer   data,
                              gsize           len,
                              gsize          *out_bytes_written,
                              OtChecksum     *checksum,
                              GCancellable   *cancellable,
                              GError        **error)
{
  if (out)
    {
      if (!g_output_stream_write_all (out, data, len, out_bytes_written,
                                      cancellable, error))
        return FALSE;
    }
  else if (out_bytes_written)
    {
      *out_bytes_written = len;
    }

  if (checksum)
    ot_checksum_update (checksum, data, len);
  return TRUE;
}

gboolean
ot_gio_splice_update_checksum (GOutputStream  *out,
                               GInputStream   *in,
                               OtChecksum     *checksum,
                               GCancellable   *cancellable,
                               GError        **error)
{
  g_return_val_if_fail (out != NULL || checksum != NULL, FALSE);

  if (checksum != NULL)
    {
      gsize bytes_read, bytes_written;
      char buf[4096];
      do
        {
          if (!g_input_stream_read_all (in, buf, sizeof (buf), &bytes_read, cancellable, error))
            return FALSE;
          if (!ot_gio_write_update_checksum (out, buf, bytes_read, &bytes_written, checksum,
                                             cancellable, error))
            return FALSE;
        }
      while (bytes_read > 0);
    }
  else if (out != NULL)
    {
      if (g_output_stream_splice (out, in, 0, cancellable, error) < 0)
        return FALSE;
    }

  return TRUE;
}

/* Copy @in to @out, return in @out_csum the binary checksum for
 * all data read.
 */
gboolean
ot_gio_splice_get_checksum (GOutputStream  *out,
                            GInputStream   *in,
                            guchar        **out_csum,
                            GCancellable   *cancellable,
                            GError        **error)
{
  g_auto(OtChecksum) checksum = { 0, };
  ot_checksum_init (&checksum);

  if (!ot_gio_splice_update_checksum (out, in, &checksum, cancellable, error))
    return FALSE;

  guint8 digest[_OSTREE_SHA256_DIGEST_LEN];
  ot_checksum_get_digest (&checksum, digest, sizeof (digest));
  g_autofree guchar *ret_csum = g_malloc (sizeof (digest));
  memcpy (ret_csum, digest, sizeof (digest));
  ot_transfer_out_value (out_csum, &ret_csum);
  return TRUE;
}

char *
ot_checksum_file_at (int             dfd,
                     const char     *path,
                     GChecksumType   checksum_type,
                     GCancellable   *cancellable,
                     GError        **error)
{
  g_autoptr(GInputStream) in = NULL;
  if (!ot_openat_read_stream (dfd, path, TRUE, &in, cancellable, error))
    return FALSE;

  g_auto(OtChecksum) checksum = { 0, };
  ot_checksum_init (&checksum);
  if (!ot_gio_splice_update_checksum (NULL, in, &checksum, cancellable, error))
    return FALSE;

  char hexdigest[_OSTREE_SHA256_STRING_LEN+1];
  ot_checksum_get_hexdigest (&checksum, hexdigest, sizeof (hexdigest));
  return g_strdup (hexdigest);
}
