/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

#pragma once

#include <ostree.h>

void
ot_common_pull_progress (OstreeAsyncProgress       *progress,
                         gpointer                   user_data);

gboolean
ot_common_parse_statoverride_file (const char    *statoverride_file,
                                   GHashTable   **out_mode_add,
                                   GCancellable  *cancellable,
                                   GError        **error);

#include "ot-editor.h"
  
gboolean
ot_common_commit_editor (OstreeRepo     *repo,
                         const char     *branch,
                         char          **subject,
                         char          **body,
                         GCancellable   *cancellable,
                         GError        **error);
