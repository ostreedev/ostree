/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>.
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

#pragma once

#include <gio/gio.h>
#include <string.h> /* Yeah...let's just do that here. */
#include <libglnx.h>

/* https://bugzilla.gnome.org/show_bug.cgi?id=766370 */
#if !GLIB_CHECK_VERSION(2, 49, 3)
#define OT_VARIANT_BUILDER_INITIALIZER {{0,}}
#else
#define OT_VARIANT_BUILDER_INITIALIZER {{{0,}}}
#endif

#define ot_gobject_refz(o) (o ? g_object_ref (o) : o)

#define ot_transfer_out_value(outp, srcp) G_STMT_START {   \
  if (outp)                                                \
    {                                                      \
      *outp = *srcp;                                       \
      *(srcp) = NULL;                                      \
    }                                                      \
  } G_STMT_END;

#ifdef HAVE_LIBSYSTEMD
#define ot_journal_send(...) sd_journal_send(__VA_ARGS__)
#define ot_journal_print(...) sd_journal_print(__VA_ARGS__)
#else
#define ot_journal_send(...) {}
#define ot_journal_print(...) {}
#endif

#include <ot-keyfile-utils.h>
#include <ot-gio-utils.h>
#include <ot-fs-utils.h>
#include <ot-opt-utils.h>
#include <ot-unix-utils.h>
#include <ot-variant-utils.h>
#include <ot-variant-builder.h>
#include <ot-checksum-utils.h>
#include <ot-checksum-instream.h>
#include <ot-tool-util.h>

#ifndef OSTREE_DISABLE_GPGME
#include <ot-gpg-utils.h>
#endif
