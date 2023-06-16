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
  OtTristate use_composefs;
  OtTristate use_fsverity;

#ifdef HAVE_LINUX_FSVERITY_H
  self->fs_verity_supported = _OSTREE_FEATURE_MAYBE;
#else
  self->fs_verity_supported = _OSTREE_FEATURE_NO;
#endif

  /* Composefs use implies fsverity default of maybe */
  if (!ot_keyfile_get_tristate_with_default (self->config, _OSTREE_INTEGRITY_SECTION, "composefs",
                                             OT_TRISTATE_NO, &use_composefs, error))
    return FALSE;

  if (!ot_keyfile_get_tristate_with_default (self->config, _OSTREE_INTEGRITY_SECTION, "fsverity",
                                             (use_composefs != OT_TRISTATE_NO) ? OT_TRISTATE_MAYBE
                                                                               : OT_TRISTATE_NO,
                                             &use_fsverity, error))
    return FALSE;

  if (use_fsverity != OT_TRISTATE_NO)
    {
      self->fs_verity_wanted = (_OstreeFeatureSupport)use_fsverity;
    }
  else
    {
      /* Fall back to old configuration key */
      static const char fsverity_section[] = "ex-fsverity";

      self->fs_verity_wanted = _OSTREE_FEATURE_NO;
      gboolean fsverity_required = FALSE;
      if (!ot_keyfile_get_boolean_with_default (self->config, fsverity_section, "required", FALSE,
                                                &fsverity_required, error))
        return FALSE;
      if (fsverity_required)
        {
          self->fs_verity_wanted = _OSTREE_FEATURE_YES;
        }
      else
        {
          gboolean fsverity_opportunistic = FALSE;
          if (!ot_keyfile_get_boolean_with_default (self->config, fsverity_section, "opportunistic",
                                                    FALSE, &fsverity_opportunistic, error))
            return FALSE;
          if (fsverity_opportunistic)
            self->fs_verity_wanted = _OSTREE_FEATURE_MAYBE;
        }
    }

  if (self->fs_verity_wanted == _OSTREE_FEATURE_YES
      && self->fs_verity_supported == _OSTREE_FEATURE_NO)
    return glnx_throw (error, "fsverity required, but libostree compiled without support");

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
