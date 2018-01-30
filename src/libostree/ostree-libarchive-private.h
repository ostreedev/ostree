/*
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#pragma once

#include "config.h"

#include <gio/gio.h>
#include "otutil.h"
#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

G_BEGIN_DECLS

#ifdef HAVE_LIBARCHIVE
typedef struct archive OtAutoArchiveWrite;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OtAutoArchiveWrite, archive_write_free)
typedef struct archive OtAutoArchiveRead;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OtAutoArchiveRead, archive_read_free)

static inline OtAutoArchiveRead *
ot_open_archive_read (const char *path, GError **error)
{
  g_autoptr(OtAutoArchiveRead) a = archive_read_new ();

#ifdef HAVE_ARCHIVE_READ_SUPPORT_FILTER_ALL
  archive_read_support_filter_all (a);
#else
  archive_read_support_compression_all (a);
#endif
  archive_read_support_format_all (a);
  if (archive_read_open_filename (a, path, 8192) != ARCHIVE_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "%s", archive_error_string (a));
      return NULL;
    }

  return g_steal_pointer (&a);
}

#endif

G_END_DECLS
