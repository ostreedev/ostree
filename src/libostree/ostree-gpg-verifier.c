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

#include "ostree-gpg-verifier.h"
#include "otutil.h"

#include <stdlib.h>
#include <glib/gstdio.h>
#include <gpgme.h>

typedef struct {
  GObjectClass parent_class;
} OstreeGpgVerifierClass;

struct OstreeGpgVerifier {
  GObject parent;

  GList *keyrings;
};

static void _ostree_gpg_verifier_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (OstreeGpgVerifier, _ostree_gpg_verifier, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, _ostree_gpg_verifier_initable_iface_init))

static void
ostree_gpg_verifier_finalize (GObject *object)
{
  OstreeGpgVerifier *self = OSTREE_GPG_VERIFIER (object);

  g_list_free_full (self->keyrings, g_object_unref);

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
}

static gboolean
ostree_gpg_verifier_initable_init (GInitable        *initable,
                                   GCancellable     *cancellable,
                                   GError          **error)
{
  gboolean ret = FALSE;
  OstreeGpgVerifier *self = (OstreeGpgVerifier*)initable;
  const char *default_keyring_path = g_getenv ("OSTREE_GPG_HOME");
  gs_unref_object GFile *default_keyring_dir = NULL;

  if (!default_keyring_path)
    default_keyring_path = DATADIR "/ostree/trusted.gpg.d/";

  default_keyring_dir = g_file_new_for_path (default_keyring_path);
  if (!_ostree_gpg_verifier_add_keyring_dir (self, default_keyring_dir,
                                             cancellable, error))
    {
      g_prefix_error (error, "Reading keyring directory '%s'",
                      gs_file_get_path_cached (default_keyring_dir));
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static void
_ostree_gpg_verifier_initable_iface_init (GInitableIface *iface)
{
  iface->init = ostree_gpg_verifier_initable_init;
}

static gboolean
concatenate_keyrings (OstreeGpgVerifier *self,
                      GFile *destination,
                      GCancellable *cancellable,
                      GError **error)
{
  gs_unref_object GOutputStream *target_stream = NULL;
  GList *link;
  gboolean ret = FALSE;

  target_stream = (GOutputStream *) g_file_replace (destination,
                                                    NULL,   /* no etag */
                                                    FALSE,  /* no backup */
                                                    G_FILE_CREATE_NONE,
                                                    cancellable, error);
  if (target_stream == NULL)
    goto out;

  for (link = self->keyrings; link != NULL; link = link->next)
    {
      gs_unref_object GInputStream *source_stream = NULL;
      GFile *keyring_file = link->data;
      gssize bytes_written;
      GError *local_error = NULL;

      source_stream = (GInputStream *) g_file_read (keyring_file, cancellable, &local_error);

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
                                              source_stream,
                                              G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                              cancellable, error);
      if (bytes_written == -1)
        goto out;
    }

  if (!g_output_stream_close (target_stream, cancellable, error))
    goto out;

  ret = TRUE;

out:
  return ret;
}

static void
gpg_error_to_gio_error (gpgme_error_t gpg_error,
                        GError **error)
{
  GIOErrorEnum errcode;

  /* XXX This list is incomplete.  Add cases as needed. */

  switch (gpg_error)
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

static gboolean
override_gpgme_home_dir (gpgme_ctx_t gpg_ctx,
                         const char *home_dir,
                         GError **error)
{
  gpgme_engine_info_t gpg_engine_info;
  gboolean ret = FALSE;

  /* Override the OpenPGP engine's configuration directory without
   * affecting other parameters.  This requires finding the current
   * parameters since the engine API takes all parameters at once. */

  for (gpg_engine_info = gpgme_ctx_get_engine_info (gpg_ctx);
       gpg_engine_info != NULL;
       gpg_engine_info = gpg_engine_info->next)
    {
      if (gpg_engine_info->protocol == GPGME_PROTOCOL_OpenPGP)
        {
          gpgme_error_t gpg_error;

          gpg_error = gpgme_ctx_set_engine_info (gpg_ctx,
                                                 gpg_engine_info->protocol,
                                                 gpg_engine_info->file_name,
                                                 home_dir);
          if (gpg_error != GPG_ERR_NO_ERROR)
            {
              gpg_error_to_gio_error (gpg_error, error);
              goto out;
            }

          break;
        }
    }

  if (gpg_engine_info == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "GPGME: No OpenPGP engine available");
      goto out;
    }

  ret = TRUE;

out:
  return ret;
}

gboolean
_ostree_gpg_verifier_check_signature (OstreeGpgVerifier  *self,
                                      GFile              *file,
                                      GFile              *signature,
                                      gboolean           *out_had_valid_sig,
                                      GCancellable       *cancellable,
                                      GError            **error)
{
  gpgme_ctx_t gpg_ctx = NULL;
  gpgme_error_t gpg_error = NULL;
  gpgme_data_t data_buffer = NULL;
  gpgme_data_t signature_buffer = NULL;
  gpgme_verify_result_t verify_result;
  gpgme_signature_t signature_iter;
  gs_unref_object GFile *pubring_file = NULL;
  gs_free char *pubring_path = NULL;
  gs_free char *temp_dir = NULL;
  gboolean had_valid_sig = FALSE;
  gboolean ret = FALSE;

  /* GPGME has no API for using multiple keyrings (aka, gpg --keyring),
   * so we concatenate all the keyring files into one pubring.gpg in a
   * temporary directory, then tell GPGME to use that directory as the
   * home directory. */

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    goto out;

  temp_dir = g_build_filename (g_get_tmp_dir (), "ostree-gpg-XXXXXX", NULL);

  if (mkdtemp (temp_dir) == NULL)
    {
      gs_set_error_from_errno (error, errno);
      goto out;
    }

  pubring_path = g_build_filename (temp_dir, "pubring.gpg", NULL);

  pubring_file = g_file_new_for_path (pubring_path);
  if (!concatenate_keyrings (self, pubring_file, cancellable, error))
    goto out;

  gpg_error = gpgme_new (&gpg_ctx);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      gpg_error_to_gio_error (gpg_error, error);
      g_prefix_error (error, "Unable to create context: ");
      goto out;
    }

  if (!override_gpgme_home_dir (gpg_ctx, temp_dir, error))
    goto out;

  {
    gs_free char *path = g_file_get_path (file);
    gpg_error = gpgme_data_new_from_file (&data_buffer, path, 1);

    if (gpg_error != GPG_ERR_NO_ERROR)
      {
        gpg_error_to_gio_error (gpg_error, error);
        g_prefix_error (error, "Unable to read signed text: ");
        goto out;
      }
  }

  {
    gs_free char *path = g_file_get_path (signature);
    gpg_error = gpgme_data_new_from_file (&signature_buffer, path, 1);

    if (gpg_error != GPG_ERR_NO_ERROR)
      {
        gpg_error_to_gio_error (gpg_error, error);
        g_prefix_error (error, "Unable to read signature: ");
        goto out;
      }
  }

  gpg_error = gpgme_op_verify (gpg_ctx, signature_buffer, data_buffer, NULL);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      gpg_error_to_gio_error (gpg_error, error);
      g_prefix_error (error, "Unable to complete signature verification: ");
      goto out;
    }

  /* Result data is owned by the context. */
  verify_result = gpgme_op_verify_result (gpg_ctx);

  /* XXX Needs improvement.  We're throwing away a bunch of result data
   *     here that could be used for more advanced signature operations.
   *     Don't want to expose GPGME types in libostree but maybe we can
   *     wrapper it in GLib types. */

  for (signature_iter = verify_result->signatures;
       signature_iter != NULL;
       signature_iter = signature_iter->next)
    {
      /* Mimic the way librepo tests for a valid signature, checking both
       * summary and status fields.
       *
       * - VALID summary flag means the signature is fully valid.
       * - GREEN summary flag means the signature is valid with caveats.
       * - No summary but also no error means the signature is valid but
       *   the signing key is not certified with a trusted signature.
       */
      if ((signature_iter->summary & GPGME_SIGSUM_VALID) ||
          (signature_iter->summary & GPGME_SIGSUM_GREEN) ||
          (signature_iter->summary == 0 &&
           signature_iter->status == GPG_ERR_NO_ERROR))
        {
          had_valid_sig = TRUE;
          break;
        }
    }

  if (out_had_valid_sig != NULL)
    *out_had_valid_sig = had_valid_sig;

  ret = TRUE;

out:

  /* Try to clean up the temporary directory. */
  if (temp_dir != NULL)
    (void) glnx_shutil_rm_rf_at (-1, temp_dir, NULL, NULL);

  if (gpg_ctx != NULL)
    gpgme_release (gpg_ctx);
  if (data_buffer != NULL)
    gpgme_data_release (data_buffer);
  if (signature_buffer != NULL)
    gpgme_data_release (signature_buffer);

  g_prefix_error (error, "GPG: ");

  return ret;
}

gboolean
_ostree_gpg_verifier_add_keyring (OstreeGpgVerifier  *self,
                                  GFile              *path,
                                  GCancellable       *cancellable,
                                  GError            **error)
{
  g_return_val_if_fail (path != NULL, FALSE);

  self->keyrings = g_list_append (self->keyrings, g_object_ref (path));
  return TRUE;
}

gboolean
_ostree_gpg_verifier_add_keyring_dir (OstreeGpgVerifier   *self,
                                      GFile               *path,
                                      GCancellable        *cancellable,
                                      GError             **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *enumerator = NULL;
  
  enumerator = g_file_enumerate_children (path, OSTREE_GIO_FAST_QUERYINFO,
                                          G_FILE_QUERY_INFO_NONE,
                                          cancellable, error);
  if (!enumerator)
    goto out;

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *path;

      if (!gs_file_enumerator_iterate (enumerator, &file_info, &path,
                                       cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR &&
          g_str_has_suffix (g_file_info_get_name (file_info), ".gpg"))
        self->keyrings = g_list_append (self->keyrings, g_object_ref (path));
    }

  ret = TRUE;
 out:
  return ret;
}

OstreeGpgVerifier*
_ostree_gpg_verifier_new (GCancellable   *cancellable,
                          GError        **error)
{
  return g_initable_new (OSTREE_TYPE_GPG_VERIFIER, cancellable, error, NULL);
}
