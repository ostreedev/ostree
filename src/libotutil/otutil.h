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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#pragma once

#include <gio/gio.h>
#include <libglnx.h>
#include <string.h> /* Yeah...let's just do that here. */
#include <syslog.h>

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-journal.h>
#endif

/* https://bugzilla.gnome.org/show_bug.cgi?id=766370 */
#if !GLIB_CHECK_VERSION(2, 49, 3)
#define OT_VARIANT_BUILDER_INITIALIZER \
  { \
    { \
      0, \
    } \
  }
#else
#define OT_VARIANT_BUILDER_INITIALIZER \
  { \
    { \
      { \
        0, \
      } \
    } \
  }
#endif

static inline const char *
ot_booltostr (int b)
{
  return b ? "true" : "false";
}

#define ot_gobject_refz(o) (o ? g_object_ref (o) : o)

#define ot_transfer_out_value(outp, srcp) \
  G_STMT_START \
  { \
    if (outp) \
      { \
        *outp = *srcp; \
        *(srcp) = NULL; \
      } \
  } \
  G_STMT_END;

#ifdef HAVE_LIBSYSTEMD
#define ot_journal_send(...) sd_journal_send (__VA_ARGS__)
#define ot_journal_print(...) sd_journal_print (__VA_ARGS__)
#else
#define ot_journal_send(...) \
  { \
  }
#define ot_journal_print(...) \
  { \
  }
#endif

typedef GMainContext GMainContextPopDefault;
static inline void
_ostree_main_context_pop_default_destroy (void *p)
{
  GMainContext *main_context = p;

  if (main_context)
    {
      g_main_context_pop_thread_default (main_context);
      g_main_context_unref (main_context);
    }
}

static inline GMainContextPopDefault *
_ostree_main_context_new_default (void)
{
  GMainContext *main_context = g_main_context_new ();

  g_main_context_push_thread_default (main_context);
  return main_context;
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GMainContextPopDefault, _ostree_main_context_pop_default_destroy)

#include <ot-checksum-instream.h>
#include <ot-checksum-utils.h>
#include <ot-fs-utils.h>
#include <ot-gio-utils.h>
#include <ot-keyfile-utils.h>
#include <ot-opt-utils.h>
#include <ot-tool-util.h>
#include <ot-unix-utils.h>
#include <ot-variant-builder.h>
#include <ot-variant-utils.h>

#ifndef OSTREE_DISABLE_GPGME
#include <ot-gpg-utils.h>
#endif
