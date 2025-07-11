/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include "ostree-bootloader-grub2.h"
#include "ostree-cmd-private.h"
#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-sysroot-private.h"

#include "otutil.h"

static gboolean
impl_ostree_generate_grub2_config (OstreeSysroot *sysroot, int bootversion, int target_fd,
                                   GCancellable *cancellable, GError **error)
{
  return _ostree_bootloader_grub2_generate_config (sysroot, bootversion, target_fd, cancellable,
                                                   error);
}

/**
 * ostree_cmdprivate: (skip)
 *
 * Do not call this function; it is used to share private API between
 * the OSTree commandline and the library.
 */
const OstreeCmdPrivateVTable *
ostree_cmd__private__ (void)
{
  static OstreeCmdPrivateVTable table = {
    _ostree_impl_system_generator,    impl_ostree_generate_grub2_config,
    _ostree_repo_static_delta_dump,   _ostree_repo_static_delta_query_exists,
    _ostree_repo_static_delta_delete, _ostree_repo_verify_bindings,
    _ostree_sysroot_finalize_staged,  _ostree_sysroot_boot_complete,
    _ostree_prepare_soft_reboot,
  };

  return &table;
}
