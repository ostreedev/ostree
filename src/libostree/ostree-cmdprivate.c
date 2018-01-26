/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#include "config.h"

#include "ostree-cmdprivate.h"
#include "ostree-repo-private.h"
#include "ostree-core-private.h"
#include "ostree-repo-pull-private.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-sysroot.h"
#include "ostree-bootloader-grub2.h"

#include "otutil.h"

static gboolean 
impl_ostree_generate_grub2_config (OstreeSysroot *sysroot, int bootversion, int target_fd, GCancellable *cancellable, GError **error)
{
  return _ostree_bootloader_grub2_generate_config (sysroot, bootversion, target_fd, cancellable, error);
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
    _ostree_impl_system_generator,
    impl_ostree_generate_grub2_config,
    _ostree_repo_static_delta_dump,
    _ostree_repo_static_delta_query_exists,
    _ostree_repo_static_delta_delete,
    _ostree_repo_verify_bindings
  };

  return &table;
}
