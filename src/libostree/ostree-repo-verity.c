/*
 * Copyright (C) Red Hat, Inc.
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

#include <sys/ioctl.h>

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ot-fs-utils.h"
#include "otutil.h"
#ifdef HAVE_LINUX_FSVERITY_H
#include <linux/fsverity.h>
#endif

#if defined(HAVE_OPENSSL)
#include <openssl/bio.h>
#include <openssl/engine.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs7.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC (X509, X509_free);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (EVP_PKEY, EVP_PKEY_free);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (BIO, BIO_free);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PKCS7, PKCS7_free);
#endif

gboolean
_ostree_repo_parse_fsverity_config (OstreeRepo *self, GError **error)
{
  /* Currently experimental */
  static const char fsverity_key[] = "ex-fsverity";
  self->fs_verity_wanted = _OSTREE_FEATURE_NO;
#ifdef HAVE_LINUX_FSVERITY_H
  self->fs_verity_supported = _OSTREE_FEATURE_MAYBE;
#else
  self->fs_verity_supported = _OSTREE_FEATURE_NO;
#endif
  gboolean fsverity_required = FALSE;
  if (!ot_keyfile_get_boolean_with_default (self->config, fsverity_key, "required", FALSE,
                                            &fsverity_required, error))
    return FALSE;
  if (fsverity_required)
    {
      self->fs_verity_wanted = _OSTREE_FEATURE_YES;
      if (self->fs_verity_supported == _OSTREE_FEATURE_NO)
        return glnx_throw (error, "fsverity required, but libostree compiled without support");
    }
  else
    {
      gboolean fsverity_opportunistic = FALSE;
      if (!ot_keyfile_get_boolean_with_default (self->config, fsverity_key, "opportunistic", FALSE,
                                                &fsverity_opportunistic, error))
        return FALSE;
      if (fsverity_opportunistic)
        self->fs_verity_wanted = _OSTREE_FEATURE_MAYBE;
    }

  return TRUE;
}

/* Wrapper around the fsverity ioctl, compressing the result to
 * "success, unsupported or error".  This is used for /boot where
 * we enable verity if supported.
 * */
gboolean
_ostree_tmpf_fsverity_core (GLnxTmpfile *tmpf, _OstreeFeatureSupport fsverity_requested,
                            GBytes *signature, gboolean *supported, GError **error)
{
  /* Set this by default to simplify the code below */
  if (supported)
    *supported = FALSE;

  if (fsverity_requested == _OSTREE_FEATURE_NO)
    return TRUE;

#ifdef HAVE_LINUX_FSVERITY_H
  GLNX_AUTO_PREFIX_ERROR ("fsverity", error);

  /* fs-verity requires a read-only file descriptor */
  if (!glnx_tmpfile_reopen_rdonly (tmpf, error))
    return FALSE;

  struct fsverity_enable_arg arg = {
    0,
  };
  arg.version = 1;
  arg.hash_algorithm = FS_VERITY_HASH_ALG_SHA256; /* TODO configurable? */
  arg.block_size = 4096;                          /* FIXME query */
  arg.salt_size = 0;                              /* TODO store salt in ostree repo config */
  arg.salt_ptr = 0;
  arg.sig_size = signature ? g_bytes_get_size (signature) : 0;
  arg.sig_ptr = signature ? (guint64)g_bytes_get_data (signature, NULL) : 0;

  if (ioctl (tmpf->fd, FS_IOC_ENABLE_VERITY, &arg) < 0)
    {
      switch (errno)
        {
        case ENOTTY:
        case EOPNOTSUPP:
          return TRUE;
        default:
          return glnx_throw_errno_prefix (error, "ioctl(FS_IOC_ENABLE_VERITY)");
        }
    }

  if (supported)
    *supported = TRUE;
#endif
  return TRUE;
}

/* Enable verity on a file, respecting the "wanted" and "supported" states.
 * The main idea here is to optimize out pointlessly calling the ioctl()
 * over and over in cases where it's not supported for the repo's filesystem,
 * as well as to support "opportunistic" use (requested and if filesystem supports).
 * */
gboolean
_ostree_tmpf_fsverity (OstreeRepo *self, GLnxTmpfile *tmpf, GBytes *signature, GError **error)
{
#ifdef HAVE_LINUX_FSVERITY_H
  g_mutex_lock (&self->txn_lock);
  _OstreeFeatureSupport fsverity_wanted = self->fs_verity_wanted;
  _OstreeFeatureSupport fsverity_supported = self->fs_verity_supported;
  g_mutex_unlock (&self->txn_lock);

  switch (fsverity_wanted)
    {
    case _OSTREE_FEATURE_YES:
      {
        if (fsverity_supported == _OSTREE_FEATURE_NO)
          return glnx_throw (error, "fsverity required but filesystem does not support it");
      }
      break;
    case _OSTREE_FEATURE_MAYBE:
      break;
    case _OSTREE_FEATURE_NO:
      return TRUE;
    }

  gboolean supported = FALSE;
  if (!_ostree_tmpf_fsverity_core (tmpf, fsverity_wanted, signature, &supported, error))
    return FALSE;

  if (!supported)
    {
      if (G_UNLIKELY (fsverity_wanted == _OSTREE_FEATURE_YES))
        return glnx_throw (error, "fsverity required but filesystem does not support it");

      /* If we got here, we must be trying "opportunistic" use of fs-verity */
      g_assert_cmpint (fsverity_wanted, ==, _OSTREE_FEATURE_MAYBE);
      g_mutex_lock (&self->txn_lock);
      self->fs_verity_supported = _OSTREE_FEATURE_NO;
      g_mutex_unlock (&self->txn_lock);
      return TRUE;
    }

  g_mutex_lock (&self->txn_lock);
  self->fs_verity_supported = _OSTREE_FEATURE_YES;
  g_mutex_unlock (&self->txn_lock);
#else
  g_assert_cmpint (self->fs_verity_wanted, !=, _OSTREE_FEATURE_YES);
#endif
  return TRUE;
}

#if defined(HAVE_OPENSSL)
static gboolean
read_pem_x509_certificate (const char *certfile, X509 **cert_ret, GError **error)
{
  g_autoptr (BIO) bio = NULL;
  X509 *cert;

  errno = 0;
  bio = BIO_new_file (certfile, "r");
  if (!bio)
    return glnx_throw_errno_prefix (error, "Error loading composefs certfile '%s'", certfile);

  cert = PEM_read_bio_X509 (bio, NULL, NULL, NULL);
  if (!cert)
    return glnx_throw (error, "Error parsing composefs certfile '%s'", certfile);

  *cert_ret = cert;
  return TRUE;
}

static gboolean
read_pem_pkcs8_private_key (const char *keyfile, EVP_PKEY **pkey_ret, GError **error)
{
  g_autoptr (BIO) bio;
  EVP_PKEY *pkey;

  errno = 0;
  bio = BIO_new_file (keyfile, "r");
  if (!bio)
    return glnx_throw_errno_prefix (error, "Error loading composefs keyfile '%s'", keyfile);

  pkey = PEM_read_bio_PrivateKey (bio, NULL, NULL, NULL);
  if (!pkey)
    return glnx_throw (error, "Error parsing composefs keyfile '%s'", keyfile);

  *pkey_ret = pkey;
  return TRUE;
}

static gboolean
sign_pkcs7 (const void *data_to_sign, size_t data_size, EVP_PKEY *pkey, X509 *cert,
            const EVP_MD *md, BIO **res, GError **error)
{
  int pkcs7_flags = PKCS7_BINARY | PKCS7_DETACHED | PKCS7_NOATTR | PKCS7_NOCERTS | PKCS7_PARTIAL;
  g_autoptr (BIO) bio = NULL;
  g_autoptr (BIO) bio_res = NULL;
  g_autoptr (PKCS7) p7 = NULL;

  bio = BIO_new_mem_buf ((void *)data_to_sign, data_size);
  if (!bio)
    return glnx_throw (error, "Can't allocate buffer");

  p7 = PKCS7_sign (NULL, NULL, NULL, bio, pkcs7_flags);
  if (!p7)
    return glnx_throw (error, "Can't initialize PKCS#7");

  if (!PKCS7_sign_add_signer (p7, cert, pkey, md, pkcs7_flags))
    return glnx_throw (error, "Can't add signer to PKCS#7");

  if (PKCS7_final (p7, bio, pkcs7_flags) != 1)
    return glnx_throw (error, "Can't finalize PKCS#7");

  bio_res = BIO_new (BIO_s_mem ());
  if (!bio_res)
    return glnx_throw (error, "Can't allocate buffer");

  if (i2d_PKCS7_bio (bio_res, p7) != 1)
    return glnx_throw (error, "Can't DER-encode PKCS#7 signature object");

  *res = g_steal_pointer (&bio_res);
  return TRUE;
}

gboolean
_ostree_fsverity_sign (const char *certfile, const char *keyfile, const guchar *fsverity_digest,
                       GBytes **data_out, GCancellable *cancellable, GError **error)
{
  g_autofree struct fsverity_formatted_digest *d = NULL;
  gsize d_size;
  g_autoptr (X509) cert = NULL;
  g_autoptr (EVP_PKEY) pkey = NULL;
  g_autoptr (BIO) bio_sig = NULL;
  const EVP_MD *md;
  guchar *sig;
  long sig_size;

  if (certfile == NULL)
    return glnx_throw (error, "certfile not specified");

  if (keyfile == NULL)
    return glnx_throw (error, "keyfile not specified");

  if (!read_pem_x509_certificate (certfile, &cert, error))
    return FALSE;

  if (!read_pem_pkcs8_private_key (keyfile, &pkey, error))
    return FALSE;

  md = EVP_sha256 ();
  if (md == NULL)
    return glnx_throw (error, "No sha256 support in openssl");

  d_size = sizeof (struct fsverity_formatted_digest) + OSTREE_SHA256_DIGEST_LEN;
  d = g_malloc0 (d_size);

  memcpy (d->magic, "FSVerity", 8);
  d->digest_algorithm = GUINT16_TO_LE (FS_VERITY_HASH_ALG_SHA256);
  d->digest_size = GUINT16_TO_LE (OSTREE_SHA256_DIGEST_LEN);
  memcpy (d->digest, fsverity_digest, OSTREE_SHA256_DIGEST_LEN);

  if (!sign_pkcs7 (d, d_size, pkey, cert, md, &bio_sig, error))
    return FALSE;

  sig_size = BIO_get_mem_data (bio_sig, &sig);

  *data_out = g_bytes_new (sig, sig_size);

  return TRUE;
}
#else
gboolean
_ostree_fsverity_sign (const char *certfile, const char *keyfile, const guchar *fsverity_digest,
                       GBytes **data_out, GCancellable *cancellable, GError **error)
{
  return glnx_throw (error, "fsverity signature support not built");
}
#endif
