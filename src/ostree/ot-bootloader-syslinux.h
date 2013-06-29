/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
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
 */

#ifndef __OT_BOOTLOADER_SYSLINUX_H__
#define __OT_BOOTLOADER_SYSLINUX_H__

#include "ot-bootloader.h"

G_BEGIN_DECLS

#define OT_TYPE_BOOTLOADER_SYSLINUX (ot_bootloader_syslinux_get_type ())
#define OT_BOOTLOADER_SYSLINUX(inst) (G_TYPE_CHECK_INSTANCE_CAST ((inst), OT_TYPE_BOOTLOADER_SYSLINUX, OtBootloaderSyslinux))
#define OT_IS_BOOTLOADER_SYSLINUX(inst) (G_TYPE_CHECK_INSTANCE_TYPE ((inst), OT_TYPE_BOOTLOADER_SYSLINUX))

typedef struct _OtBootloaderSyslinux OtBootloaderSyslinux;

GType ot_bootloader_syslinux_get_type (void) G_GNUC_CONST;

OtBootloaderSyslinux * ot_bootloader_syslinux_new (GFile *sysroot);

G_END_DECLS

#endif /* __OT_BOOTLOADER_SYSLINUX_H__ */
