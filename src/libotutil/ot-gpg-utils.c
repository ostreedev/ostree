/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
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

#include "config.h"

#include "ot-gpg-utils.h"

#include <stdlib.h>

#include "libglnx.h"

void
ot_gpgme_error_to_gio_error (gpgme_error_t   gpg_error,
                             GError        **error)
{
  GIOErrorEnum errcode;

  /* XXX This list is incomplete.  Add cases as needed. */

  switch (gpgme_err_code (gpg_error))
    {
      /* special case - shouldn't be here */
      case GPG_ERR_NO_ERROR:
        g_return_if_reached ();

      /* special case - abort on out-of-memory */
      case GPG_ERR_ENOMEM:
        g_error ("%s: %s",
                 gpgme_strsource (gpg_error),
                 gpgme_strerror (gpg_error));

      case GPG_ERR_INV_VALUE:
        errcode = G_IO_ERROR_INVALID_ARGUMENT;
        break;

      default:
        errcode = G_IO_ERROR_FAILED;
        break;
    }

  g_set_error (error, G_IO_ERROR, errcode, "%s: %s",
               gpgme_strsource (gpg_error),
               gpgme_strerror (gpg_error));
}

gboolean
ot_gpgme_ctx_tmp_home_dir (gpgme_ctx_t     gpgme_ctx,
                           const char     *tmp_dir,
                           char          **out_tmp_home_dir,
                           GOutputStream **out_pubring_stream,
                           GCancellable   *cancellable,
                           GError        **error)
{
  g_autoptr(GFile) pubring_file = NULL;
  g_autoptr(GOutputStream) target_stream = NULL;
  g_autofree char *pubring_path = NULL;
  g_autofree char *tmp_home_dir = NULL;
  gpgme_error_t gpg_error;
  gboolean ret = FALSE;

  g_return_val_if_fail (gpgme_ctx != NULL, FALSE);

  /* GPGME has no API for using multiple keyrings (aka, gpg --keyring),
   * so we create a temporary directory and tell GPGME to use it as the
   * home directory.  Then (optionally) create a pubring.gpg file there
   * and hand the caller an open output stream to concatenate necessary
   * keyring files. */

  if (tmp_dir == NULL)
    tmp_dir = g_get_tmp_dir ();

  tmp_home_dir = g_build_filename (tmp_dir, "ostree-gpg-XXXXXX", NULL);

  if (mkdtemp (tmp_home_dir) == NULL)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  /* Not documented, but gpgme_ctx_set_engine_info() accepts NULL for
   * the executable file name, which leaves the old setting unchanged. */
  gpg_error = gpgme_ctx_set_engine_info (gpgme_ctx,
                                         GPGME_PROTOCOL_OpenPGP,
                                         NULL, tmp_home_dir);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      ot_gpgme_error_to_gio_error (gpg_error, error);
      goto out;
    }

  if (out_pubring_stream != NULL)
    {
      GFileOutputStream *pubring_stream;
      g_autoptr(GFile) pubring_file = NULL;
      g_autofree char *pubring_path = NULL;

      pubring_path = g_build_filename (tmp_home_dir, "pubring.gpg", NULL);
      pubring_file = g_file_new_for_path (pubring_path);

      pubring_stream = g_file_create (pubring_file,
                                      G_FILE_CREATE_NONE,
                                      cancellable, error);
      if (pubring_stream == NULL)
        goto out;

      /* Sneaky cast from GFileOutputStream to GOutputStream. */
      *out_pubring_stream = g_steal_pointer (&pubring_stream);
    }

  if (out_tmp_home_dir != NULL)
    *out_tmp_home_dir = g_steal_pointer (&tmp_home_dir);

  ret = TRUE;

out:
  if (!ret)
    {
      /* Clean up our mess on error. */
      (void) glnx_shutil_rm_rf_at (AT_FDCWD, tmp_home_dir, NULL, NULL);
    }

  return ret;
}
