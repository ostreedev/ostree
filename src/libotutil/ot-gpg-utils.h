/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#pragma once

#include <gpgme.h>
#include <gio/gio.h>
#include "libglnx.h"

G_BEGIN_DECLS

G_DEFINE_AUTO_CLEANUP_FREE_FUNC(gpgme_data_t, gpgme_data_release, NULL)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC(gpgme_ctx_t, gpgme_release, NULL)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC(gpgme_key_t, gpgme_key_unref, NULL)

gboolean ot_gpgme_throw (gpgme_error_t gpg_error, GError **error,
                         const char *fmt, ...) G_GNUC_PRINTF (3, 4);

gboolean ot_gpgme_ctx_tmp_home_dir (gpgme_ctx_t     gpgme_ctx,
                                    char          **out_tmp_home_dir,
                                    GOutputStream **out_pubring_stream,
                                    GCancellable   *cancellable,
                                    GError        **error);

gpgme_data_t ot_gpgme_data_input (GInputStream *input_stream);
gpgme_data_t ot_gpgme_data_output (GOutputStream *output_stream);

gpgme_ctx_t ot_gpgme_new_ctx (const char *homedir,
                              GError    **error);

G_END_DECLS
