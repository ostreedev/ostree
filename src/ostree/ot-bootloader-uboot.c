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

#include "ot-bootloader-uboot.h"
#include "otutil.h"
#include "ot-admin-functions.h"
#include "libgsystem.h"

#include <string.h>

struct _OtBootloaderUboot
{
  GObject       parent_instance;

  GFile        *sysroot;
  GFile        *config_path;
};

typedef GObjectClass OtBootloaderUbootClass;

static void ot_bootloader_uboot_bootloader_iface_init (OtBootloaderInterface *iface);
G_DEFINE_TYPE_WITH_CODE (OtBootloaderUboot, ot_bootloader_uboot, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OT_TYPE_BOOTLOADER, ot_bootloader_uboot_bootloader_iface_init));

static gboolean
ot_bootloader_uboot_query (OtBootloader *bootloader)
{
  OtBootloaderUboot *self = OT_BOOTLOADER_UBOOT (bootloader);

  return g_file_query_file_type (self->config_path, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_SYMBOLIC_LINK;
}

static const char *
ot_bootloader_uboot_get_name (OtBootloader *bootloader)
{
  return "U-Boot";
}

static gboolean
create_config_from_boot_loader_entries (OtBootloaderUboot     *self,
                                        int                    bootversion,
                                        GPtrArray             *new_lines,
                                        GCancellable          *cancellable,
                                        GError               **error)
{
  gs_unref_ptrarray GPtrArray *boot_loader_configs = NULL;
  OtConfigParser *config;
  const char *val;

  if (!ot_admin_read_boot_loader_configs (self->sysroot, bootversion, &boot_loader_configs,
                                          cancellable, error))
    return FALSE;

  /* U-Boot doesn't support a menu so just pick the first one since the list is ordered */
  config = boot_loader_configs->pdata[0];

  val = ot_config_parser_get (config, "linux");
  if (!val)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No \"linux\" key in bootloader config");
      return FALSE;
    }
  g_ptr_array_add (new_lines, g_strdup_printf ("kernel_image=%s", val));

  val = ot_config_parser_get (config, "initrd");
  if (val)
    g_ptr_array_add (new_lines, g_strdup_printf ("ramdisk_image=%s", val));

  val = ot_config_parser_get (config, "options");
  if (val)
    g_ptr_array_add (new_lines, g_strdup_printf ("bootargs=%s", val));

  return TRUE;
}

static gboolean
ot_bootloader_uboot_write_config (OtBootloader          *bootloader,
                                  int                    bootversion,
                                  GCancellable          *cancellable,
                                  GError               **error)
{
  OtBootloaderUboot *self = OT_BOOTLOADER_UBOOT (bootloader);
  gs_unref_object GFile *new_config_path = NULL;
  gs_free char *config_contents = NULL;
  gs_free char *new_config_contents = NULL;
  gs_unref_ptrarray GPtrArray *new_lines = NULL;

  /* This should follow the symbolic link to the current bootversion. */
  config_contents = gs_file_load_contents_utf8 (self->config_path, cancellable, error);
  if (!config_contents)
    return FALSE;

  new_config_path = ot_gfile_resolve_path_printf (self->sysroot, "boot/loader.%d/uEnv.txt",
                                                  bootversion);

  new_lines = g_ptr_array_new_with_free_func (g_free);

  if (!create_config_from_boot_loader_entries (self, bootversion, new_lines,
                                               cancellable, error))
    return FALSE;

  new_config_contents = ot_admin_join_lines (new_lines);

  if (strcmp (new_config_contents, config_contents) != 0)
    {
      if (!g_file_replace_contents (new_config_path, new_config_contents,
                                    strlen (new_config_contents),
                                    NULL, FALSE, G_FILE_CREATE_NONE,
                                    NULL, cancellable, error))
        return FALSE;
      g_print ("Saved new version of %s\n", gs_file_get_path_cached (self->config_path));
    }

  return TRUE;
}

static void
ot_bootloader_uboot_finalize (GObject *object)
{
  OtBootloaderUboot *self = OT_BOOTLOADER_UBOOT (object);

  g_clear_object (&self->sysroot);
  g_clear_object (&self->config_path);

  G_OBJECT_CLASS (ot_bootloader_uboot_parent_class)->finalize (object);
}

void
ot_bootloader_uboot_init (OtBootloaderUboot *self)
{
}

static void
ot_bootloader_uboot_bootloader_iface_init (OtBootloaderInterface *iface)
{
  iface->query = ot_bootloader_uboot_query;
  iface->get_name = ot_bootloader_uboot_get_name;
  iface->write_config = ot_bootloader_uboot_write_config;
}

void
ot_bootloader_uboot_class_init (OtBootloaderUbootClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = ot_bootloader_uboot_finalize;
}

OtBootloaderUboot *
ot_bootloader_uboot_new (GFile *sysroot)
{
  OtBootloaderUboot *self = g_object_new (OT_TYPE_BOOTLOADER_UBOOT, NULL);
  self->sysroot = g_object_ref (sysroot);
  self->config_path = g_file_resolve_relative_path (self->sysroot, "boot/uEnv.txt");
  return self;
}
