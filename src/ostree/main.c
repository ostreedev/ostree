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

#include "config.h"

#include <gio/gio.h>

#include <string.h>

#include "ot-main.h"
#include "ot-builtins.h"

static OstreeBuiltin builtins[] = {
  { "cat", ostree_builtin_cat, 0 },
  { "checkout", ostree_builtin_checkout, 0 },
  { "checksum", ostree_builtin_checksum, OSTREE_BUILTIN_FLAG_NO_REPO },
  { "diff", ostree_builtin_diff, 0 },
  { "init", ostree_builtin_init, 0 },
  { "commit", ostree_builtin_commit, 0 },
  { "compose", ostree_builtin_compose, 0 },
  { "local-clone", ostree_builtin_local_clone, 0 },
  { "log", ostree_builtin_log, 0 },
  { "ls", ostree_builtin_ls, 0 },
  { "prune", ostree_builtin_prune, 0 },
  { "fsck", ostree_builtin_fsck, 0 },
  { "pack", ostree_builtin_pack, 0 },
  { "remote", ostree_builtin_remote, 0 },
  { "rev-parse", ostree_builtin_rev_parse, 0 },
  { "remote", ostree_builtin_remote, 0 },
  { "show", ostree_builtin_show, 0 },
  { "unpack", ostree_builtin_unpack, 0 },
  { NULL }
};

int
main (int    argc,
      char **argv)
{
  return ostree_main (argc, argv, builtins);
}
