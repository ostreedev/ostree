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

#include "libglnx.h"
#include "ostree.h"

typedef enum {
  OSTREE_BUILTIN_FLAG_NONE = 0,
  OSTREE_BUILTIN_FLAG_NO_REPO = 1 << 0,
  OSTREE_BUILTIN_FLAG_NO_CHECK = 1 << 1
} OstreeBuiltinFlags;

typedef enum {
  OSTREE_ADMIN_BUILTIN_FLAG_NONE = 0,
  OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER = 1 << 0,
  OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED = 1 << 1
} OstreeAdminBuiltinFlags;

typedef struct {
  const char *name;
  gboolean (*fn) (int argc, char **argv, GCancellable *cancellable, GError **error);
} OstreeCommand;

int ostree_run (int argc, char **argv, OstreeCommand *commands, GError **error);

int ostree_usage (OstreeCommand *commands, gboolean is_error);

gboolean ostree_option_context_parse (GOptionContext *context,
                                      const GOptionEntry *main_entries,
                                      int *argc, char ***argv,
                                      OstreeBuiltinFlags flags,
                                      OstreeRepo **out_repo,
                                      GCancellable *cancellable, GError **error);

gboolean ostree_admin_option_context_parse (GOptionContext *context,
                                            const GOptionEntry *main_entries,
                                            int *argc, char ***argv,
                                            OstreeAdminBuiltinFlags flags,
                                            OstreeSysroot **out_sysroot,
                                            GCancellable *cancellable, GError **error);

gboolean ostree_ensure_repo_writable (OstreeRepo *repo, GError **error);

void ostree_print_gpg_verify_result (OstreeGpgVerifyResult *result);

gboolean ot_enable_tombstone_commits (OstreeRepo *repo, GError **error);
