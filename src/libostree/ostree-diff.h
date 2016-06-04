/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

#include "ostree-core.h"
#include "ostree-types.h"

G_BEGIN_DECLS

/**
 * OstreeDiffFlags:
 */
typedef enum {
  OSTREE_DIFF_FLAGS_NONE = 0,
  OSTREE_DIFF_FLAGS_IGNORE_XATTRS = (1 << 0)
} OstreeDiffFlags;

/**
 * OstreeDiffItem:
 */
typedef struct _OstreeDiffItem OstreeDiffItem;
struct _OstreeDiffItem
{
  volatile gint refcount;

  GFile *src;
  GFile *target;

  GFileInfo *src_info;
  GFileInfo *target_info;

  char *src_checksum;
  char *target_checksum;
};

_OSTREE_PUBLIC
OstreeDiffItem *ostree_diff_item_ref (OstreeDiffItem *diffitem);
_OSTREE_PUBLIC
void ostree_diff_item_unref (OstreeDiffItem *diffitem);

_OSTREE_PUBLIC
GType ostree_diff_item_get_type (void);

_OSTREE_PUBLIC
gboolean ostree_diff_dirs (OstreeDiffFlags flags,
                           GFile          *a,
                           GFile          *b,
                           GPtrArray      *modified,
                           GPtrArray      *removed,
                           GPtrArray      *added,
                           GCancellable   *cancellable,
                           GError        **error);

_OSTREE_PUBLIC
void ostree_diff_print (GFile          *a,
                        GFile          *b,
                        GPtrArray      *modified,
                        GPtrArray      *removed,
                        GPtrArray      *added);

G_END_DECLS
