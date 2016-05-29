/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Collabora Ltd
 *
 * Based on ot-bootloader-syslinux.c by Colin Walters <walters@verbum.org>
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
 *
 * Author: Javier Martinez Canillas <javier.martinez@collabora.co.uk>
 */

#include "config.h"

#include "ostree-sysroot-private.h"
#include "ostree-bootloader-uboot.h"
#include "otutil.h"

#include <string.h>

struct _OstreeBootloaderUboot
{
  GObject       parent_instance;

  OstreeSysroot  *sysroot;
  GFile          *config_path;
};

typedef GObjectClass OstreeBootloaderUbootClass;

static void _ostree_bootloader_uboot_bootloader_iface_init (OstreeBootloaderInterface *iface);
G_DEFINE_TYPE_WITH_CODE (OstreeBootloaderUboot, _ostree_bootloader_uboot, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_BOOTLOADER, _ostree_bootloader_uboot_bootloader_iface_init));

static gboolean
_ostree_bootloader_uboot_query (OstreeBootloader *bootloader,
                                gboolean         *out_is_active,
                                GCancellable     *cancellable,
                                GError          **error) 
{
  OstreeBootloaderUboot *self = OSTREE_BOOTLOADER_UBOOT (bootloader);

  *out_is_active = g_file_query_file_type (self->config_path, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_REGULAR;
  return TRUE;
}

static const char *
_ostree_bootloader_uboot_get_name (OstreeBootloader *bootloader)
{
  return "U-Boot";
}

static gboolean
create_config_from_boot_loader_entries (OstreeBootloaderUboot     *self,
                                        int                    bootversion,
                                        GPtrArray             *new_lines,
                                        GCancellable          *cancellable,
                                        GError               **error)
{
  g_autoptr(GPtrArray) boot_loader_configs = NULL;
  OstreeBootconfigParser *config;
  const char *val;

  if (!_ostree_sysroot_read_boot_loader_configs (self->sysroot, bootversion, &boot_loader_configs,
                                                 cancellable, error))
    return FALSE;

  /* U-Boot doesn't support a menu so just pick the first one since the list is ordered */
  config = boot_loader_configs->pdata[0];

  val = ostree_bootconfig_parser_get (config, "linux");
  if (!val)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No \"linux\" key in bootloader config");
      return FALSE;
    }
  g_ptr_array_add (new_lines, g_strdup_printf ("kernel_image=%s", val));

  val = ostree_bootconfig_parser_get (config, "initrd");
  if (val)
    g_ptr_array_add (new_lines, g_strdup_printf ("ramdisk_image=%s", val));

  val = ostree_bootconfig_parser_get (config, "options");
  if (val)
    g_ptr_array_add (new_lines, g_strdup_printf ("bootargs=%s", val));

  return TRUE;
}

static gboolean
_ostree_bootloader_uboot_write_config (OstreeBootloader          *bootloader,
                                  int                    bootversion,
                                  GCancellable          *cancellable,
                                  GError               **error)
{
  OstreeBootloaderUboot *self = OSTREE_BOOTLOADER_UBOOT (bootloader);
  g_autoptr(GFile) new_config_path = NULL;
  g_autofree char *config_contents = NULL;
  g_autofree char *new_config_contents = NULL;
  g_autoptr(GPtrArray) new_lines = NULL;

  /* This should follow the symbolic link to the current bootversion. */
  config_contents = glnx_file_get_contents_utf8_at (AT_FDCWD, gs_file_get_path_cached (self->config_path), NULL,
                                                    cancellable, error);
  if (!config_contents)
    return FALSE;

  new_config_path = ot_gfile_resolve_path_printf (self->sysroot->path, "boot/loader.%d/uEnv.txt",
                                                      bootversion);

  new_lines = g_ptr_array_new_with_free_func (g_free);

  if (!create_config_from_boot_loader_entries (self, bootversion, new_lines,
                                               cancellable, error))
    return FALSE;

  new_config_contents = _ostree_sysroot_join_lines (new_lines);
  {
    g_autoptr(GBytes) new_config_contents_bytes =
      g_bytes_new_static (new_config_contents,
                          strlen (new_config_contents));

    if (!ot_gfile_replace_contents_fsync (new_config_path, new_config_contents_bytes,
                                          cancellable, error))
      return FALSE;
  }

  return TRUE;
}

static void
_ostree_bootloader_uboot_finalize (GObject *object)
{
  OstreeBootloaderUboot *self = OSTREE_BOOTLOADER_UBOOT (object);

  g_clear_object (&self->sysroot);
  g_clear_object (&self->config_path);

  G_OBJECT_CLASS (_ostree_bootloader_uboot_parent_class)->finalize (object);
}

void
_ostree_bootloader_uboot_init (OstreeBootloaderUboot *self)
{
}

static void
_ostree_bootloader_uboot_bootloader_iface_init (OstreeBootloaderInterface *iface)
{
  iface->query = _ostree_bootloader_uboot_query;
  iface->get_name = _ostree_bootloader_uboot_get_name;
  iface->write_config = _ostree_bootloader_uboot_write_config;
}

void
_ostree_bootloader_uboot_class_init (OstreeBootloaderUbootClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = _ostree_bootloader_uboot_finalize;
}

OstreeBootloaderUboot *
_ostree_bootloader_uboot_new (OstreeSysroot *sysroot)
{
  OstreeBootloaderUboot *self = g_object_new (OSTREE_TYPE_BOOTLOADER_UBOOT, NULL);
  self->sysroot = g_object_ref (sysroot);
  self->config_path = g_file_resolve_relative_path (self->sysroot->path, "boot/loader/uEnv.txt");
  return self;
}
