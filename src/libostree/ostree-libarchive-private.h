/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
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
#endif

G_END_DECLS
