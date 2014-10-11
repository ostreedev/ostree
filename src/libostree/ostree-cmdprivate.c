/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include "config.h"

#include "ostree-cmdprivate.h"
#include "ostree-sysroot.h"
#include "ostree-bootloader-grub2.h"

#include "otutil.h"

static gboolean 
impl_ostree_generate_grub2_config (OstreeSysroot *sysroot, int bootversion, int target_fd, GCancellable *cancellable, GError **error)
{
  gs_unref_object OstreeBootloaderGrub2 *grub2 = _ostree_bootloader_grub2_new (sysroot);

  return _ostree_bootloader_grub2_generate_config (grub2, bootversion, target_fd, cancellable, error);
}

/**
 * ostree_cmdprivate: (skip)
 *
 * Do not call this function; it is used to share private API between
 * the OSTree commandline and the library.
 */
OstreeCmdPrivateVTable *
ostree_cmd__private__ (void)
{
  static OstreeCmdPrivateVTable table = {
    impl_ostree_generate_grub2_config
  };

  return &table;
}

