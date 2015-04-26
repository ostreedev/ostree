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

void
ot_gpgme_error_to_gio_error (gpgme_error_t   gpg_error,
                             GError        **error)
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
