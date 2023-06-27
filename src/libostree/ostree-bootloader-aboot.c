/*
 * Copyright (C) 2022 Eric Curtin <ericcurtin17@gmail.com>
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
 * Public License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ostree-bootloader-aboot.h"
#include "ostree-deployment-private.h"
#include "ostree-libarchive-private.h"
#include "ostree-sysroot-private.h"
#include "otutil.h"
#include <sys/mount.h>

#include <string.h>

/* This is specific to aboot and zipl today, but in the future we could also
 * use it for the grub2-mkconfig case.
 */
static const char aboot_requires_execute_path[] = "boot/ostree-bootloader-update.stamp";

struct _OstreeBootloaderAboot
{
  GObject parent_instance;

  OstreeSysroot *sysroot;
};

typedef GObjectClass OstreeBootloaderAbootClass;
static void _ostree_bootloader_aboot_bootloader_iface_init (OstreeBootloaderInterface *iface);
G_DEFINE_TYPE_WITH_CODE (OstreeBootloaderAboot, _ostree_bootloader_aboot, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_BOOTLOADER,
                                                _ostree_bootloader_aboot_bootloader_iface_init));

static gboolean
_ostree_bootloader_aboot_query (OstreeBootloader *bootloader, gboolean *out_is_active,
                                GCancellable *cancellable, GError **error)
{
  /* We don't auto-detect this one; should be explicitly chosen right now.
   * see also https://github.com/coreos/coreos-assembler/pull/849
   */
  *out_is_active = FALSE;
  return TRUE;
}

static const char *
_ostree_bootloader_aboot_get_name (OstreeBootloader *bootloader)
{
  return "aboot";
}

static gboolean
_ostree_bootloader_aboot_write_config (OstreeBootloader *bootloader, int bootversion,
                                       GPtrArray *new_deployments, GCancellable *cancellable,
                                       GError **error)
{
  OstreeBootloaderAboot *self = OSTREE_BOOTLOADER_ABOOT (bootloader);

  /* Write our stamp file */
  if (!glnx_file_replace_contents_at (self->sysroot->sysroot_fd, aboot_requires_execute_path,
                                      (guint8 *)"", 0, GLNX_FILE_REPLACE_NODATASYNC, cancellable,
                                      error))
    return FALSE;

  return TRUE;
}

static gboolean
_ostree_aboot_get_bls_config (OstreeBootloaderAboot *self, int bootversion, gchar **aboot,
                              gchar **abootcfg, gchar **version, gchar **vmlinuz, gchar **initramfs,
                              gchar **options, GCancellable *cancellable, GError **error)
{
  g_autoptr (GPtrArray) configs = NULL;
  if (!_ostree_sysroot_read_boot_loader_configs (self->sysroot, bootversion, &configs, cancellable,
                                                 error))
    return glnx_prefix_error (error, "aboot: loading bls configs");

  if (!configs || configs->len == 0)
    return glnx_throw (error, "aboot: no bls config");

  OstreeBootconfigParser *parser = (OstreeBootconfigParser *)g_ptr_array_index (configs, 0);
  const gchar *val = NULL;

  val = ostree_bootconfig_parser_get (parser, "aboot");
  if (!val)
    {
      return glnx_throw (error, "aboot: no \"aboot\" key in bootloader config");
    }
  *aboot = g_strdup (val);

  val = ostree_bootconfig_parser_get (parser, "abootcfg");
  if (!val)
    {
      return glnx_throw (error, "aboot: no \"abootcfg\" key in bootloader config");
    }
  *abootcfg = g_strdup (val);

  val = ostree_bootconfig_parser_get (parser, "version");
  if (!val)
    return glnx_throw (error, "aboot: no \"version\" key in bootloader config");
  *version = g_strdup (val);

  val = ostree_bootconfig_parser_get (parser, "linux");
  if (!val)
    return glnx_throw (error, "aboot: no \"linux\" key in bootloader config");
  *vmlinuz = g_build_filename ("/boot", val, NULL);

  val = ostree_bootconfig_parser_get (parser, "initrd");
  if (!val)
    return glnx_throw (error, "aboot: no \"initrd\" key in bootloader config");
  *initramfs = g_build_filename ("/boot", val, NULL);

  val = ostree_bootconfig_parser_get (parser, "options");
  if (!val)
    return glnx_throw (error, "aboot: no \"options\" key in bootloader config");
  *options = g_strdup (val);

  return TRUE;
}

static gboolean
_ostree_bootloader_aboot_post_bls_sync (OstreeBootloader *bootloader, int bootversion,
                                        GCancellable *cancellable, GError **error)
{
  OstreeBootloaderAboot *self = OSTREE_BOOTLOADER_ABOOT (bootloader);

  /* Note that unlike the grub2-mkconfig backend, we make no attempt to
   * chroot().
   */
  // g_assert (self->sysroot->booted_deployment);

  if (!glnx_fstatat_allow_noent (self->sysroot->sysroot_fd, aboot_requires_execute_path, NULL, 0,
                                 error))
    return FALSE;

  /* If there's no stamp file, nothing to do */
  if (errno == ENOENT)
    return TRUE;

  g_autofree gchar *aboot = NULL;
  g_autofree gchar *abootcfg = NULL;
  g_autofree gchar *version = NULL;
  g_autofree gchar *vmlinuz = NULL;
  g_autofree gchar *initramfs = NULL;
  g_autofree gchar *options = NULL;
  if (!_ostree_aboot_get_bls_config (self, bootversion, &aboot, &abootcfg, &version, &vmlinuz,
                                     &initramfs, &options, cancellable, error))
    return FALSE;

  g_autofree char *path_str = g_file_get_path (self->sysroot->path);

  const char *const aboot_argv[]
      = { "aboot-deploy", "-r", path_str, "-c", abootcfg, "-o", options, aboot, NULL };
  int estatus;
  if (!g_spawn_sync (NULL, (char **)aboot_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL,
                     &estatus, error))
    {
      return FALSE;
    }

  if (!g_spawn_check_exit_status (estatus, error))
    {
      return FALSE;
    }

  if (!glnx_unlinkat (self->sysroot->sysroot_fd, aboot_requires_execute_path, 0, error))
    {
      return FALSE;
    }

  return TRUE;
}

static void
_ostree_bootloader_aboot_finalize (GObject *object)
{
  OstreeBootloaderAboot *self = OSTREE_BOOTLOADER_ABOOT (object);

  g_clear_object (&self->sysroot);

  G_OBJECT_CLASS (_ostree_bootloader_aboot_parent_class)->finalize (object);
}

void
_ostree_bootloader_aboot_init (OstreeBootloaderAboot *self)
{
}

static void
_ostree_bootloader_aboot_bootloader_iface_init (OstreeBootloaderInterface *iface)
{
  iface->query = _ostree_bootloader_aboot_query;
  iface->get_name = _ostree_bootloader_aboot_get_name;
  iface->write_config = _ostree_bootloader_aboot_write_config;
  iface->post_bls_sync = _ostree_bootloader_aboot_post_bls_sync;
}

void
_ostree_bootloader_aboot_class_init (OstreeBootloaderAbootClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = _ostree_bootloader_aboot_finalize;
}

OstreeBootloaderAboot *
_ostree_bootloader_aboot_new (OstreeSysroot *sysroot)
{
  OstreeBootloaderAboot *self = g_object_new (OSTREE_TYPE_BOOTLOADER_ABOOT, NULL);
  self->sysroot = g_object_ref (sysroot);
  return self;
}
