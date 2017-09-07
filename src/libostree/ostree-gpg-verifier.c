/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 * Copyright (C) 2013 Sjoerd Simons <sjoerd.simons@collabora.co.uk>
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
 * Author: Sjoerd Simons <sjoerd.simons@collabora.co.uk>
 */

#include "config.h"

#include "libglnx.h"
#include "ostree-gpg-verifier.h"
#include "ot-gpg-utils.h"
#include "ostree-gpg-verify-result-private.h"
#include "otutil.h"

#include <stdlib.h>
#include <glib/gstdio.h>

typedef struct {
  GObjectClass parent_class;
} OstreeGpgVerifierClass;

struct OstreeGpgVerifier {
  GObject parent;

  GList *keyrings;
  GPtrArray *keyring_data;
  GPtrArray *key_ascii_files;
};

G_DEFINE_TYPE (OstreeGpgVerifier, _ostree_gpg_verifier, G_TYPE_OBJECT)

static void
ostree_gpg_verifier_finalize (GObject *object)
{
  OstreeGpgVerifier *self = OSTREE_GPG_VERIFIER (object);

  g_list_free_full (self->keyrings, g_object_unref);
  if (self->key_ascii_files)
    g_ptr_array_unref (self->key_ascii_files);
  g_clear_pointer (&self->keyring_data, (GDestroyNotify)g_ptr_array_unref);

  G_OBJECT_CLASS (_ostree_gpg_verifier_parent_class)->finalize (object);
}

static void
_ostree_gpg_verifier_class_init (OstreeGpgVerifierClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = ostree_gpg_verifier_finalize;

  /* Initialize GPGME */
  gpgme_check_version (NULL);
}

static void
_ostree_gpg_verifier_init (OstreeGpgVerifier *self)
{
  self->keyring_data = g_ptr_array_new_with_free_func ((GDestroyNotify)g_bytes_unref);
}

static void
verify_result_finalized_cb (gpointer data,
                            GObject *finalized_verify_result)
{
  g_autofree gchar *tmp_dir = data;  /* assume ownership */

  /* XXX OstreeGpgVerifyResult could do this cleanup in its own
   *     finalize() method, but I didn't want this keyring hack
   *     bleeding into multiple classes. */

  (void) glnx_shutil_rm_rf_at (AT_FDCWD, tmp_dir, NULL, NULL);
}

OstreeGpgVerifyResult *
_ostree_gpg_verifier_check_signature (OstreeGpgVerifier  *self,
                                      GBytes             *signed_data,
                                      GBytes             *signatures,
                                      GCancellable       *cancellable,
                                      GError            **error)
{
  GLNX_AUTO_PREFIX_ERROR("GPG", error);
  gpgme_error_t gpg_error = 0;
  g_auto(gpgme_data_t) data_buffer = NULL;
  g_auto(gpgme_data_t) signature_buffer = NULL;
  g_autofree char *tmp_dir = NULL;
  g_autoptr(GOutputStream) target_stream = NULL;
  OstreeGpgVerifyResult *result = NULL;
  gboolean success = FALSE;
  GList *link;
  int armor;

  /* GPGME has no API for using multiple keyrings (aka, gpg --keyring),
   * so we concatenate all the keyring files into one pubring.gpg in a
   * temporary directory, then tell GPGME to use that directory as the
   * home directory. */

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    goto out;

  result = g_initable_new (OSTREE_TYPE_GPG_VERIFY_RESULT,
                           cancellable, error, NULL);
  if (result == NULL)
    goto out;

  if (!ot_gpgme_ctx_tmp_home_dir (result->context,
                                  &tmp_dir, &target_stream,
                                  cancellable, error))
    goto out;

  for (link = self->keyrings; link != NULL; link = link->next)
    {
      g_autoptr(GFileInputStream) source_stream = NULL;
      GFile *keyring_file = link->data;
      gssize bytes_written;
      GError *local_error = NULL;

      source_stream = g_file_read (keyring_file, cancellable, &local_error);

      /* Disregard non-existent keyrings. */
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&local_error);
          continue;
        }
      else if (local_error != NULL)
        {
          g_propagate_error (error, local_error);
          goto out;
        }

      bytes_written = g_output_stream_splice (target_stream,
                                              G_INPUT_STREAM (source_stream),
                                              G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                              cancellable, error);
      if (bytes_written < 0)
        goto out;
    }

  for (guint i = 0; i < self->keyring_data->len; i++)
    {
      GBytes *keyringd = self->keyring_data->pdata[i];
      gsize len;
      gsize bytes_written;
      const guint8 *buf = g_bytes_get_data (keyringd, &len);
      if (!g_output_stream_write_all (target_stream, buf, len, &bytes_written,
                                      cancellable, error))
        goto out;
    }

  if (!g_output_stream_close (target_stream, cancellable, error))
    goto out;

  /* Save the previous armor value - we need it on for importing ASCII keys */
  armor = gpgme_get_armor (result->context);
  gpgme_set_armor (result->context, 1);

  /* Now, use the API to import ASCII-armored keys */
  if (self->key_ascii_files)
    {
      for (guint i = 0; i < self->key_ascii_files->len; i++)
        {
          const char *path = self->key_ascii_files->pdata[i];
          glnx_fd_close int fd = -1;
          g_auto(gpgme_data_t) kdata = NULL;

          if (!glnx_openat_rdonly (AT_FDCWD, path, TRUE, &fd, error))
            goto out;

          gpg_error = gpgme_data_new_from_fd (&kdata, fd);
          if (gpg_error != GPG_ERR_NO_ERROR)
            {
              ot_gpgme_throw (gpg_error, error, "Loading data from fd %i", fd);
              goto out;
            }

          gpg_error = gpgme_op_import (result->context, kdata);
          if (gpg_error != GPG_ERR_NO_ERROR)
            {
              ot_gpgme_throw (gpg_error, error, "Failed to import key");
              goto out;
            }
        }
    }

  gpgme_set_armor (result->context, armor);

  /* Both the signed data and signature GBytes instances will outlive the
   * gpgme_data_t structs, so we can safely reuse the GBytes memory buffer
   * directly and avoid a copy. */

  gpg_error = gpgme_data_new_from_mem (&data_buffer,
                                       g_bytes_get_data (signed_data, NULL),
                                       g_bytes_get_size (signed_data),
                                       0 /* do not copy */);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      ot_gpgme_throw (gpg_error, error, "Unable to read signed data");
      goto out;
    }

  gpg_error = gpgme_data_new_from_mem (&signature_buffer,
                                       g_bytes_get_data (signatures, NULL),
                                       g_bytes_get_size (signatures),
                                       0 /* do not copy */);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      ot_gpgme_throw (gpg_error, error, "Unable to read signature");
      goto out;
    }

  gpg_error = gpgme_op_verify (result->context, signature_buffer, data_buffer, NULL);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      ot_gpgme_throw (gpg_error, error, "Unable to complete signature verification");
      goto out;
    }

  /* Result data is owned by the context. */
  result->details = gpgme_op_verify_result (result->context);

  gpgme_result_ref (result->details);

  success = TRUE;

out:
  if (success)
    {
      /* Keep the temporary directory around for the life of the result
       * object so its GPGME context remains valid.  It may yet have to
       * extract user details from signing keys and will need to access
       * the fabricated pubring.gpg keyring. */
      g_object_weak_ref (G_OBJECT (result),
                         verify_result_finalized_cb,
                         g_strdup (tmp_dir));
    }
  else
    {
      /* Destroy the result object on error. */
      g_clear_object (&result);

      /* Try to clean up the temporary directory. */
      if (tmp_dir != NULL)
        (void) glnx_shutil_rm_rf_at (AT_FDCWD, tmp_dir, NULL, NULL);
    }

  return result;
}

/* Given @path which should contain a GPG keyring file, add it
 * to the list of trusted keys.
 */
void
_ostree_gpg_verifier_add_keyring_file (OstreeGpgVerifier  *self,
                                       GFile              *path)
{
  g_return_if_fail (G_IS_FILE (path));

  self->keyrings = g_list_append (self->keyrings, g_object_ref (path));
}

/* Given @keyring which should be the contents of a GPG keyring file, add it to
 * the list of trusted keys.
 */
void
_ostree_gpg_verifier_add_keyring_data (OstreeGpgVerifier  *self,
                                       GBytes             *keyring)
{
  g_ptr_array_add (self->keyring_data, g_bytes_ref (keyring));
}

void
_ostree_gpg_verifier_add_key_ascii_file (OstreeGpgVerifier *self,
                                         const char        *path)
{
  if (!self->key_ascii_files)
    self->key_ascii_files = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (self->key_ascii_files, g_strdup (path));
}

gboolean
_ostree_gpg_verifier_add_keyring_dir (OstreeGpgVerifier   *self,
                                      GFile               *path,
                                      GCancellable        *cancellable,
                                      GError             **error)

{
  return _ostree_gpg_verifier_add_keyring_dir_at (self, AT_FDCWD,
                                                  gs_file_get_path_cached (path),
                                                  cancellable, error);
}

gboolean
_ostree_gpg_verifier_add_keyring_dir_at (OstreeGpgVerifier   *self,
                                         int                  dfd,
                                         const char          *path,
                                         GCancellable        *cancellable,
                                         GError             **error)

{
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  if (!glnx_dirfd_iterator_init_at (dfd, path, FALSE,
                                    &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (dent->d_type != DT_REG)
        continue;

      const char *name = dent->d_name;

      /* Files with a .gpg suffix are typically keyrings except
       * for trustdb.gpg, which is the GPG trust database. */

      if (!g_str_has_suffix (name, ".gpg"))
        continue;

      if (g_str_equal (name, "trustdb.gpg"))
        continue;

      if (g_str_equal (name, "secring.gpg"))
        continue;

      glnx_fd_close int fd = -1;
      if (!glnx_openat_rdonly (dfd_iter.fd, dent->d_name, TRUE, &fd, error))
        return FALSE;

      g_autoptr(GBytes) data = glnx_fd_readall_bytes (fd, cancellable, error);
      if (!data)
        return FALSE;

      g_ptr_array_add (self->keyring_data, g_steal_pointer (&data));
    }

  return TRUE;
}

gboolean
_ostree_gpg_verifier_add_global_keyring_dir (OstreeGpgVerifier  *self,
                                             GCancellable       *cancellable,
                                             GError            **error)
{
  g_return_val_if_fail (OSTREE_IS_GPG_VERIFIER (self), FALSE);

  const char *global_keyring_path = g_getenv ("OSTREE_GPG_HOME");
  if (global_keyring_path == NULL)
    global_keyring_path = DATADIR "/ostree/trusted.gpg.d/";

  if (g_file_test (global_keyring_path, G_FILE_TEST_IS_DIR))
    {
      g_autoptr(GFile) global_keyring_dir = g_file_new_for_path (global_keyring_path);
      if (!_ostree_gpg_verifier_add_keyring_dir (self, global_keyring_dir,
                                                 cancellable, error))
        return glnx_prefix_error (error, "Reading keyring directory '%s'",
                                  gs_file_get_path_cached (global_keyring_dir));
    }

  return TRUE;
}

OstreeGpgVerifier*
_ostree_gpg_verifier_new (void)
{
  return g_object_new (OSTREE_TYPE_GPG_VERIFIER, NULL);
}
