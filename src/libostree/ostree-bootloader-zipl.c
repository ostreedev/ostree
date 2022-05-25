/*
 * Copyright (C) 2019 Colin Walters <walters@verbum.org>
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

#include "ostree-sysroot-private.h"
#include "ostree-bootloader-zipl.h"
#include "ostree-deployment-private.h"
#include "otutil.h"
#include <sys/mount.h>
#include <sys/stat.h>
#include <string.h>

#define SECURE_EXECUTION_SYSFS_FLAG     "/sys/firmware/uv/prot_virt_guest"
#define SECURE_EXECUTION_PARTITION      "/dev/disk/by-label/se"
#define SECURE_EXECUTION_MOUNTPOINT     "/sysroot/se"
#define SECURE_EXECUTION_BOOT_IMAGE     SECURE_EXECUTION_MOUNTPOINT "/sd-boot"
#define SECURE_EXECUTION_HOSTKEY_PATH   "/etc/se-hostkeys/"
#define SECURE_EXECUTION_HOSTKEY_PREFIX "ibm-z-hostkey"
#define SECURE_EXECUTION_LUKS_ROOT_KEY  "/etc/luks/root"
#define SECURE_EXECUTION_LUKS_BOOT_KEY  "/etc/luks/boot"
#define SECURE_EXECUTION_LUKS_CONFIG    "/etc/crypttab"
#define SECURE_EXECUTION_RAMDISK_TOOL   PKGLIBEXECDIR "/s390x-se-luks-gencpio"

/* This is specific to zipl today, but in the future we could also
 * use it for the grub2-mkconfig case.
 */
static const char zipl_requires_execute_path[] = "boot/ostree-bootloader-update.stamp";

struct _OstreeBootloaderZipl
{
  GObject       parent_instance;

  OstreeSysroot  *sysroot;
};

typedef GObjectClass OstreeBootloaderZiplClass;

static void _ostree_bootloader_zipl_bootloader_iface_init (OstreeBootloaderInterface *iface);
G_DEFINE_TYPE_WITH_CODE (OstreeBootloaderZipl, _ostree_bootloader_zipl, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_BOOTLOADER, _ostree_bootloader_zipl_bootloader_iface_init));

static gboolean
_ostree_bootloader_zipl_query (OstreeBootloader *bootloader,
                                   gboolean         *out_is_active,
                                   GCancellable     *cancellable,
                                   GError          **error)
{
  /* We don't auto-detect this one; should be explicitly chosen right now.
   * see also https://github.com/coreos/coreos-assembler/pull/849
   */
  *out_is_active = FALSE;
  return TRUE;
}

static const char *
_ostree_bootloader_zipl_get_name (OstreeBootloader *bootloader)
{
  return "zipl";
}

static gboolean
_ostree_secure_execution_mount(GError **error)
{
  const char *device = realpath (SECURE_EXECUTION_PARTITION, NULL);
  if (device == NULL)
    return glnx_throw_errno_prefix(error, "s390x SE: resolving %s", SECURE_EXECUTION_PARTITION);
  if (mount (device, SECURE_EXECUTION_MOUNTPOINT, "ext4", 0, NULL) < 0)
    return glnx_throw_errno_prefix (error, "s390x SE: Mounting %s", device);
  return TRUE;
}

static gboolean
_ostree_secure_execution_umount(GError **error)
{
  if (umount (SECURE_EXECUTION_MOUNTPOINT) < 0)
    return glnx_throw_errno_prefix (error, "s390x SE: Unmounting %s", SECURE_EXECUTION_MOUNTPOINT);
  return TRUE;
}

static gboolean
_ostree_bootloader_zipl_write_config (OstreeBootloader  *bootloader,
                                          int                bootversion,
                                          GPtrArray         *new_deployments,
                                          GCancellable      *cancellable,
                                          GError           **error)
{
  OstreeBootloaderZipl *self = OSTREE_BOOTLOADER_ZIPL (bootloader);

  /* Write our stamp file */
  if (!glnx_file_replace_contents_at (self->sysroot->sysroot_fd, zipl_requires_execute_path,
                                      (guint8*)"", 0, GLNX_FILE_REPLACE_NODATASYNC,
                                      cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean _ostree_secure_execution_is_enabled (gboolean *out_enabled,
                                                     GCancellable *cancellable,
                                                     GError **error)
{
  *out_enabled = FALSE;
  glnx_autofd int fd = -1;
  if (!ot_openat_ignore_enoent (AT_FDCWD, SECURE_EXECUTION_SYSFS_FLAG, &fd, error))
    return FALSE;
  if (fd == -1)
    return TRUE; //ENOENT --> SecureExecution is disabled
  g_autofree char *data = glnx_fd_readall_utf8 (fd, NULL, cancellable, error);
  if (!data)
    return FALSE;
  *out_enabled = strstr (data, "1") != NULL;
  return TRUE;
}

static gboolean
_ostree_secure_execution_get_keys (GPtrArray **keys,
                                   GCancellable *cancellable,
                                   GError **error)
{
  g_auto (GLnxDirFdIterator) it = { 0,};
  if ( !glnx_dirfd_iterator_init_at (-1, SECURE_EXECUTION_HOSTKEY_PATH, TRUE, &it, error))
    return glnx_prefix_error (error, "s390x SE: looking for SE keys");

  g_autoptr(GPtrArray) ret_keys = g_ptr_array_new_with_free_func (g_free);
  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent (&it, &dent, cancellable, error))
        return FALSE;

      if (!dent)
        break;

      if (g_str_has_prefix (dent->d_name, SECURE_EXECUTION_HOSTKEY_PREFIX))
        g_ptr_array_add (ret_keys, g_build_filename (SECURE_EXECUTION_HOSTKEY_PATH, dent->d_name, NULL));
    }

  *keys = g_steal_pointer (&ret_keys);
  return TRUE;
}

static gboolean
_ostree_secure_execution_get_bls_config (OstreeBootloaderZipl *self,
                                         int bootversion,
                                         gchar **vmlinuz,
                                         gchar **initramfs,
                                         gchar **options,
                                         GCancellable *cancellable,
                                         GError **error)
{
  g_autoptr (GPtrArray) configs = NULL;
  if ( !_ostree_sysroot_read_boot_loader_configs (self->sysroot, bootversion, &configs, cancellable, error))
    return glnx_prefix_error (error, "s390x SE: loading bls configs");

  if (!configs || configs->len == 0)
    return glnx_throw (error, "s390x SE: no bls config");

  OstreeBootconfigParser *parser = (OstreeBootconfigParser *) g_ptr_array_index (configs, 0);
  const gchar *val = NULL;

  val = ostree_bootconfig_parser_get (parser, "linux");
  if (!val)
    return glnx_throw (error, "s390x SE: no \"linux\" key in bootloader config");
  *vmlinuz = g_build_filename ("/boot", val, NULL);

  val = ostree_bootconfig_parser_get (parser, "initrd");
  if (!val)
    return glnx_throw (error, "s390x SE: no \"initrd\" key in bootloader config");
  *initramfs = g_build_filename ("/boot", val, NULL);

  val = ostree_bootconfig_parser_get (parser, "options");
  if (!val)
    return glnx_throw (error, "s390x SE: no \"options\" key in bootloader config");
  *options = g_strdup(val);

  return TRUE;
}

static gboolean
_ostree_secure_execution_luks_key_exists (void)
{
  return (access(SECURE_EXECUTION_LUKS_CONFIG, F_OK) == 0 &&
    (access(SECURE_EXECUTION_LUKS_ROOT_KEY, F_OK) == 0 || access(SECURE_EXECUTION_LUKS_BOOT_KEY, F_OK) == 0));
}

static gboolean
_ostree_secure_execution_enable_luks(const gchar *oldramfs,
                                     const gchar *newramfs,
                                     GError **error)
{
  const char *const argv[] = {SECURE_EXECUTION_RAMDISK_TOOL, oldramfs, newramfs, NULL};
  g_autofree gchar *out = NULL;
  g_autofree gchar *err = NULL;
  int status = 0;
  if (!g_spawn_sync (NULL, (char**)argv, NULL, G_SPAWN_SEARCH_PATH,
                     NULL, NULL, &out, &err, &status, error))
    return glnx_prefix_error(error, "s390x SE: spawning %s", SECURE_EXECUTION_RAMDISK_TOOL);

  if (!g_spawn_check_exit_status (status, error))
    {
      g_printerr("s390x SE: `%s` stdout: %s\n", SECURE_EXECUTION_RAMDISK_TOOL, out);
      g_printerr("s390x SE: `%s` stderr: %s\n", SECURE_EXECUTION_RAMDISK_TOOL, err);
      return glnx_prefix_error(error, "s390x SE: `%s` failed", SECURE_EXECUTION_RAMDISK_TOOL);
    }

  ot_journal_print(LOG_INFO, "s390x SE: luks key added to initrd");
  return TRUE;
}

static gboolean
_ostree_secure_execution_generate_sdboot (gchar *vmlinuz,
                                          gchar *initramfs,
                                          gchar *options,
                                          GPtrArray *keys,
                                          GError **error)
{
  g_assert (vmlinuz && initramfs && options && keys && keys->len);
  ot_journal_print(LOG_INFO, "s390x SE: kernel: %s", vmlinuz);
  ot_journal_print(LOG_INFO, "s390x SE: initrd: %s", initramfs);
  ot_journal_print(LOG_INFO, "s390x SE: kargs: %s", options);

  pid_t self = getpid();

  // Store kernel options to temp file, so `genprotimg` can later embed it
  g_auto(GLnxTmpfile) cmdline = { 0, };
  if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &cmdline, error))
    return glnx_prefix_error(error, "s390x SE: opening cmdline file");
  if (glnx_loop_write (cmdline.fd, options, strlen (options)) < 0)
    return glnx_throw_errno_prefix (error, "s390x SE: writting cmdline file");
  g_autofree gchar *cmdline_filename = g_strdup_printf ("/proc/%d/fd/%d", self, cmdline.fd);

  // Copy initramfs to temp file and embed LUKS key and config into it
  g_auto(GLnxTmpfile) ramdisk = { 0, };
  g_autofree gchar *ramdisk_filename = NULL;
  if (_ostree_secure_execution_luks_key_exists ())
    {
      if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &ramdisk, error))
        return glnx_prefix_error(error, "s390x SE: creating new ramdisk");
      ramdisk_filename = g_strdup_printf ("/proc/%d/fd/%d", self, ramdisk.fd);
      if (!_ostree_secure_execution_enable_luks (initramfs, ramdisk_filename, error))
        return FALSE;
    }

  g_autoptr(GPtrArray) argv = g_ptr_array_new ();
  g_ptr_array_add (argv, "genprotimg");
  g_ptr_array_add (argv, "-i");
  g_ptr_array_add (argv, vmlinuz);
  g_ptr_array_add (argv, "-r");
  g_ptr_array_add (argv, (ramdisk_filename == NULL) ? initramfs: ramdisk_filename);
  g_ptr_array_add (argv, "-p");
  g_ptr_array_add (argv, cmdline_filename);
  for (guint i = 0; i < keys->len; ++i)
    {
      gchar *key = g_ptr_array_index (keys, i);
      g_ptr_array_add (argv, "-k");
      g_ptr_array_add (argv, key);
      ot_journal_print(LOG_INFO, "s390x SE: key[%d]: %s", i + 1, key);
    }
  g_ptr_array_add (argv, "--no-verify");
  g_ptr_array_add (argv, "-o");
  g_ptr_array_add (argv, SECURE_EXECUTION_BOOT_IMAGE);
  g_ptr_array_add (argv, NULL);

  gint status = 0;
  if (!g_spawn_sync (NULL, (char**)argv->pdata, NULL, G_SPAWN_SEARCH_PATH,
                     NULL, NULL, NULL, NULL, &status, error))
    return glnx_prefix_error(error, "s390x SE: spawning genprotimg");

  if (!g_spawn_check_exit_status (status, error))
    return glnx_prefix_error(error, "s390x SE: `genprotimg` failed");

  ot_journal_print(LOG_INFO, "s390x SE: `%s` generated", SECURE_EXECUTION_BOOT_IMAGE);
  return TRUE;
}

static gboolean
_ostree_secure_execution_call_zipl (GError **error)
{
  int status = 0;
  const char *const zipl_argv[] = {"zipl", "-V", "-t", SECURE_EXECUTION_MOUNTPOINT, "-i", SECURE_EXECUTION_BOOT_IMAGE, NULL};
  if (!g_spawn_sync (NULL, (char**)zipl_argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, NULL, &status, error))
    return glnx_prefix_error(error, "s390x SE: spawning zipl");

  if (!g_spawn_check_exit_status (status, error))
    return glnx_prefix_error(error, "s390x SE: `zipl` failed");

  ot_journal_print(LOG_INFO, "s390x SE: `sd-boot` zipled");
  return TRUE;
}

static gboolean
_ostree_secure_execution_enable (OstreeBootloaderZipl *self,
                                 int bootversion,
                                 GPtrArray *keys,
                                 GCancellable *cancellable,
                                 GError **error)
{
  g_autofree gchar* vmlinuz = NULL;
  g_autofree gchar* initramfs = NULL;
  g_autofree gchar* options = NULL;

  gboolean rc =
      _ostree_secure_execution_mount (error) &&
      _ostree_secure_execution_get_bls_config (self, bootversion, &vmlinuz, &initramfs, &options, cancellable, error) &&
      _ostree_secure_execution_generate_sdboot (vmlinuz, initramfs, options, keys, error) &&
      _ostree_secure_execution_call_zipl (error) &&
      _ostree_secure_execution_umount (error);

  return rc;
}


static gboolean
_ostree_bootloader_zipl_post_bls_sync (OstreeBootloader  *bootloader,
                                       int bootversion,
                                       GCancellable  *cancellable,
                                       GError       **error)
{
  OstreeBootloaderZipl *self = OSTREE_BOOTLOADER_ZIPL (bootloader);

  /* Note that unlike the grub2-mkconfig backend, we make no attempt to
   * chroot().
   */
  g_assert (self->sysroot->booted_deployment);

  if (!glnx_fstatat_allow_noent (self->sysroot->sysroot_fd, zipl_requires_execute_path, NULL, 0, error))
    return FALSE;

  /* If there's no stamp file, nothing to do */
  if (errno == ENOENT)
    return TRUE;

  /* Try with Secure Execution */
  gboolean se_enabled = FALSE;
  if ( !_ostree_secure_execution_is_enabled (&se_enabled, cancellable, error))
    return FALSE;
  if (se_enabled)
    {
      g_autoptr(GPtrArray) keys = NULL;
      if (!_ostree_secure_execution_get_keys (&keys, cancellable, error))
        return FALSE;
      if (!keys || keys->len == 0)
        return glnx_throw (error, "s390x SE: no keys");
      return _ostree_secure_execution_enable (self, bootversion, keys, cancellable, error);
    }
  /* Fallback to non-SE setup */
  const char *const zipl_argv[] = {"zipl", NULL};
  int estatus;
  if (!g_spawn_sync (NULL, (char**)zipl_argv, NULL, G_SPAWN_SEARCH_PATH,
                     NULL, NULL, NULL, NULL, &estatus, error))
    return FALSE;
  if (!g_spawn_check_exit_status (estatus, error))
    return FALSE;
  if (!glnx_unlinkat (self->sysroot->sysroot_fd, zipl_requires_execute_path, 0, error))
    return FALSE;
  return TRUE;
}

static void
_ostree_bootloader_zipl_finalize (GObject *object)
{
  OstreeBootloaderZipl *self = OSTREE_BOOTLOADER_ZIPL (object);

  g_clear_object (&self->sysroot);

  G_OBJECT_CLASS (_ostree_bootloader_zipl_parent_class)->finalize (object);
}

void
_ostree_bootloader_zipl_init (OstreeBootloaderZipl *self)
{
}

static void
_ostree_bootloader_zipl_bootloader_iface_init (OstreeBootloaderInterface *iface)
{
  iface->query = _ostree_bootloader_zipl_query;
  iface->get_name = _ostree_bootloader_zipl_get_name;
  iface->write_config = _ostree_bootloader_zipl_write_config;
  iface->post_bls_sync = _ostree_bootloader_zipl_post_bls_sync;
}

void
_ostree_bootloader_zipl_class_init (OstreeBootloaderZiplClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = _ostree_bootloader_zipl_finalize;
}

OstreeBootloaderZipl *
_ostree_bootloader_zipl_new (OstreeSysroot *sysroot)
{
  OstreeBootloaderZipl *self = g_object_new (OSTREE_TYPE_BOOTLOADER_ZIPL, NULL);
  self->sysroot = g_object_ref (sysroot);
  return self;
}
