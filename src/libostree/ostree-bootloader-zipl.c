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

#include "ostree-bootloader-zipl.h"
#include "ostree-deployment-private.h"
#include "ostree-libarchive-private.h"
#include "ostree-sysroot-private.h"
#include "otutil.h"
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>

#define SECURE_EXECUTION_SYSFS_FLAG "/sys/firmware/uv/prot_virt_guest"
#define SECURE_EXECUTION_PARTITION "/dev/disk/by-label/se"
#define SECURE_EXECUTION_MOUNTPOINT "/sysroot/se"
#define SECURE_EXECUTION_BOOT_IMAGE SECURE_EXECUTION_MOUNTPOINT "/sdboot"
#define SECURE_EXECUTION_HOSTKEY_PATH "/etc/se-hostkeys/"
#define SECURE_EXECUTION_HOSTKEY_PREFIX "ibm-z-hostkey"
#define SECURE_EXECUTION_LUKS_ROOT_KEY "/etc/luks/root"
#define SECURE_EXECUTION_LUKS_BOOT_KEY "/etc/luks/boot"
#define SECURE_EXECUTION_LUKS_CONFIG "/etc/crypttab"
#define SECURE_BOOT_SYSFS_FLAG "/sys/firmware/ipl/secure"

#if !(defined HAVE_LIBARCHIVE) && defined(__s390x__)
#error libarchive is required for s390x
#endif

/* This is specific to zipl today, but in the future we could also
 * use it for the grub2-mkconfig case.
 */
static const char zipl_requires_execute_path[] = "boot/ostree-bootloader-update.stamp";

struct _OstreeBootloaderZipl
{
  GObject parent_instance;

  OstreeSysroot *sysroot;
};

typedef GObjectClass OstreeBootloaderZiplClass;

static void _ostree_bootloader_zipl_bootloader_iface_init (OstreeBootloaderInterface *iface);
G_DEFINE_TYPE_WITH_CODE (OstreeBootloaderZipl, _ostree_bootloader_zipl, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_BOOTLOADER,
                                                _ostree_bootloader_zipl_bootloader_iface_init));

static gboolean
_ostree_bootloader_zipl_query (OstreeBootloader *bootloader, gboolean *out_is_active,
                               GCancellable *cancellable, GError **error)
{
#if defined(__s390x__)
  *out_is_active = TRUE;
#else
  *out_is_active = FALSE;
#endif
  return TRUE;
}

static const char *
_ostree_bootloader_zipl_get_name (OstreeBootloader *bootloader)
{
  return "zipl";
}

static gboolean
_ostree_secure_execution_mount (GError **error)
{
  const char *device = realpath (SECURE_EXECUTION_PARTITION, NULL);
  if (device == NULL)
    return glnx_throw_errno_prefix (error, "s390x SE: resolving %s", SECURE_EXECUTION_PARTITION);
  if (mount (device, SECURE_EXECUTION_MOUNTPOINT, "ext4", 0, NULL) < 0)
    return glnx_throw_errno_prefix (error, "s390x SE: Mounting %s", device);
  return TRUE;
}

static gboolean
_ostree_secure_execution_umount (GError **error)
{
  if (umount (SECURE_EXECUTION_MOUNTPOINT) < 0)
    return glnx_throw_errno_prefix (error, "s390x SE: Unmounting %s", SECURE_EXECUTION_MOUNTPOINT);
  return TRUE;
}

static gboolean
_ostree_bootloader_zipl_write_config (OstreeBootloader *bootloader, int bootversion,
                                      GPtrArray *new_deployments, GCancellable *cancellable,
                                      GError **error)
{
  OstreeBootloaderZipl *self = OSTREE_BOOTLOADER_ZIPL (bootloader);

  /* Write our stamp file */
  if (!glnx_file_replace_contents_at (self->sysroot->sysroot_fd, zipl_requires_execute_path,
                                      (guint8 *)"", 0, GLNX_FILE_REPLACE_NODATASYNC, cancellable,
                                      error))
    return FALSE;

  return TRUE;
}

static gboolean
_ostree_secure_boot_is_enabled (gboolean *out_enabled, GCancellable *cancellable, GError **error)
{
  *out_enabled = FALSE;
  glnx_autofd int fd = -1;
  if (!ot_openat_ignore_enoent (AT_FDCWD, SECURE_BOOT_SYSFS_FLAG, &fd, error))
    return FALSE;
  if (fd != -1)
    {
      g_autofree char *data = glnx_fd_readall_utf8 (fd, NULL, cancellable, error);
      if (!data)
        return FALSE;
      *out_enabled = strstr (data, "1") != NULL;
      ot_journal_print (LOG_INFO, "s390x: sysfs: Secure Boot enabled: %d", *out_enabled);
      return TRUE;
    }

  // Fallback, RHEL 9 kernel is buggy and doesn't have sysfs flag.
  // Let's check kmsg, with Secure Boot enabled kernel prints smth like:
  // [    0.027998] Linux version 5.14.0-284.36.1.el9_2.s390x
  // [    0.023193] setup: Linux is running as a z/VM guest operating system in 64-bit mode
  // [    0.023193] setup: Linux is running with Secure-IPL enabled
  // [    0.023194] setup: The IPL report contains the following components:
  // [    0.023194] setup: 0000000000009000 - 000000000000a000 (not signed)
  // [    0.023196] setup: 000000000000a000 - 000000000000e000 (signed, verified)
  // [    0.023197] setup: 0000000000010000 - 0000000000866000 (signed, verified)
  // [    0.023198] setup: 0000000000867000 - 0000000000868000 (not signed)
  // [    0.023199] setup: 0000000000877000 - 0000000000878000 (not signed)
  // [    0.023200] setup: 0000000000880000 - 0000000003f98000 (not signed)
  fd = openat (AT_FDCWD, "/dev/kmsg", O_NONBLOCK | O_RDONLY);
  if (fd == -1)
    return glnx_throw_errno_prefix (error, "openat(/dev/kmsg)");
  unsigned max_lines = 5; // no need to read dozens of messages, ours comes really early
  while (*out_enabled != TRUE && max_lines > 0)
    {
      char buf[1024];
      ssize_t len = read (fd, buf, sizeof (buf));
      if (len == -EAGAIN)
        break;
      *out_enabled = strstr (buf, "Secure-IPL enabled") != NULL;
      --max_lines;
    }
  ot_journal_print (LOG_INFO, "s390x: kmsg: Secure Boot enabled: %d", *out_enabled);
  return TRUE;
}

static gboolean
_ostree_secure_execution_is_enabled (gboolean *out_enabled, GCancellable *cancellable,
                                     GError **error)
{
  *out_enabled = FALSE;
  glnx_autofd int fd = -1;
  if (!ot_openat_ignore_enoent (AT_FDCWD, SECURE_EXECUTION_SYSFS_FLAG, &fd, error))
    return FALSE;
  if (fd == -1)
    return TRUE; // ENOENT --> SecureExecution is disabled
  g_autofree char *data = glnx_fd_readall_utf8 (fd, NULL, cancellable, error);
  if (!data)
    return FALSE;
  *out_enabled = strstr (data, "1") != NULL;
  return TRUE;
}

static gboolean
_ostree_secure_execution_get_keys (GPtrArray **keys, GCancellable *cancellable, GError **error)
{
  g_auto (GLnxDirFdIterator) it = {
    0,
  };
  if (!glnx_dirfd_iterator_init_at (-1, SECURE_EXECUTION_HOSTKEY_PATH, TRUE, &it, error))
    return glnx_prefix_error (error, "s390x SE: looking for SE keys");

  g_autoptr (GPtrArray) ret_keys = g_ptr_array_new_with_free_func (g_free);
  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent (&it, &dent, cancellable, error))
        return FALSE;

      if (!dent)
        break;

      if (g_str_has_prefix (dent->d_name, SECURE_EXECUTION_HOSTKEY_PREFIX))
        g_ptr_array_add (ret_keys,
                         g_build_filename (SECURE_EXECUTION_HOSTKEY_PATH, dent->d_name, NULL));
    }

  *keys = g_steal_pointer (&ret_keys);
  return TRUE;
}

static gboolean
_ostree_secure_execution_get_bls_config (OstreeBootloaderZipl *self, int bootversion,
                                         gchar **vmlinuz, gchar **initramfs, gchar **options,
                                         GCancellable *cancellable, GError **error)
{
  g_autoptr (GPtrArray) configs = NULL;
  if (!_ostree_sysroot_read_boot_loader_configs (self->sysroot, bootversion, &configs, cancellable,
                                                 error))
    return glnx_prefix_error (error, "s390x SE: loading bls configs");

  if (!configs || configs->len == 0)
    return glnx_throw (error, "s390x SE: no bls config");

  OstreeBootconfigParser *parser = (OstreeBootconfigParser *)g_ptr_array_index (configs, 0);
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
  *options = g_strdup (val);

  return TRUE;
}

static gboolean
_ostree_secure_execution_luks_key_exists (void)
{
  return (access (SECURE_EXECUTION_LUKS_CONFIG, F_OK) == 0
          && access (SECURE_EXECUTION_LUKS_ROOT_KEY, F_OK) == 0
          && access (SECURE_EXECUTION_LUKS_BOOT_KEY, F_OK) == 0);
}

static gboolean
_ostree_secure_execution_append_luks_keys (int initrd_fd, GCancellable *cancellable, GError **error)
{
#ifdef HAVE_LIBARCHIVE
  // appending cpio gzip archive with LUKS keys
  g_autoptr (OtAutoArchiveWrite) a = archive_write_new ();
  g_assert (a != NULL);

  if (archive_write_set_format_cpio_newc (a) != 0 || archive_write_add_filter_gzip (a) != 0
      || archive_write_open_fd (a, initrd_fd) != 0)
    return glnx_prefix_error (error, "s390x SE: initing cpio: %s", archive_error_string (a));

  const char *files[] = { "/etc", "/etc/luks", SECURE_EXECUTION_LUKS_CONFIG,
                          SECURE_EXECUTION_LUKS_BOOT_KEY, SECURE_EXECUTION_LUKS_ROOT_KEY };
  for (uint i = 0; i != G_N_ELEMENTS (files); ++i)
    {
      const char *path = files[i];
      struct stat st;
      if (stat (path, &st) != 0)
        glnx_throw_errno_prefix (error, "s390x SE: stat(%s) failed", path);

      g_autoptr (OtArchiveEntry) ae = archive_entry_new ();
      g_assert (ae != NULL);

      archive_entry_copy_stat (ae, &st);
      archive_entry_set_pathname (ae, path);
      if (archive_write_header (a, ae) != 0)
        glnx_prefix_error (error, "s390x SE: writing cpio header: %s", archive_error_string (a));

      if (S_ISREG (st.st_mode))
        {
          ot_journal_print (LOG_INFO, "s390x SE: appending %s to initrd", path);
          glnx_autofd int fd = -1;
          if (!glnx_openat_rdonly (AT_FDCWD, path, TRUE, &fd, error))
            return glnx_prefix_error (error, "s390x SE: opening %s", path);
          g_autoptr (GBytes) data = glnx_fd_readall_bytes (fd, cancellable, error);
          if (!data)
            return glnx_prefix_error (error, "s390x SE: reading %s", path);

          gsize size = 0;
          const char *ptr = (const char *)g_bytes_get_data (data, &size);
          ssize_t written = archive_write_data (a, ptr, size);
          if (written == -1)
            return glnx_prefix_error (error, "s390x SE: writing cpio entry: %s",
                                      archive_error_string (a));
          if (written != size)
            return glnx_prefix_error (error, "s390x SE: writing cpio entry %zd != %zu", written,
                                      size);
        }
    }
  ot_journal_print (LOG_INFO, "s390x SE: luks keys added to initrd");
  return TRUE;
#else
  return glnx_throw (error, "'libarchive' is required for s390x");
#endif
}

static gboolean
_ostree_secure_execution_generate_initrd (const gchar *initrd, GLnxTmpfile *out_initrd,
                                          GCancellable *cancellable, GError **error)
{
  if (!_ostree_secure_execution_luks_key_exists ())
    return glnx_throw (error, "s390x SE: missing luks keys and config");

  if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, out_initrd, error))
    return glnx_prefix_error (error, "s390x SE: opening new ramdisk");
  {
    glnx_autofd int fd = -1;
    if (!glnx_openat_rdonly (AT_FDCWD, initrd, TRUE, &fd, error))
      return glnx_prefix_error (error, "s390x SE: opening initrd");
    if (glnx_regfile_copy_bytes (fd, out_initrd->fd, (off_t)-1) < 0)
      return glnx_throw_errno_prefix (error, "s390x SE: copying ramdisk");
  }

  return _ostree_secure_execution_append_luks_keys (out_initrd->fd, cancellable, error);
}

static gboolean
_ostree_secure_execution_generate_sdboot (gchar *vmlinuz, gchar *initramfs, gchar *options,
                                          GPtrArray *keys, GCancellable *cancellable,
                                          GError **error)
{
  g_assert (vmlinuz && initramfs && options && keys && keys->len);
  ot_journal_print (LOG_INFO, "s390x SE: kernel: %s", vmlinuz);
  ot_journal_print (LOG_INFO, "s390x SE: initrd: %s", initramfs);
  ot_journal_print (LOG_INFO, "s390x SE: kargs: %s", options);

  pid_t self = getpid ();

  // Store kernel options to temp file, so `genprotimg` can later embed it
  g_auto (GLnxTmpfile) cmdline = {
    0,
  };
  if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &cmdline, error))
    return glnx_prefix_error (error, "s390x SE: opening cmdline file");
  if (glnx_loop_write (cmdline.fd, options, strlen (options)) < 0)
    return glnx_throw_errno_prefix (error, "s390x SE: writing cmdline file");
  g_autofree gchar *cmdline_filename = g_strdup_printf ("/proc/%d/fd/%d", self, cmdline.fd);

  // Copy initramfs to temp file and embed LUKS keys & config into it
  g_auto (GLnxTmpfile) ramdisk = {
    0,
  };
  if (!_ostree_secure_execution_generate_initrd (initramfs, &ramdisk, cancellable, error))
    return FALSE;
  g_autofree gchar *ramdisk_filename = g_strdup_printf ("/proc/%d/fd/%d", self, ramdisk.fd);

  g_autoptr (GPtrArray) argv = g_ptr_array_new ();
  g_ptr_array_add (argv, "genprotimg");
  g_ptr_array_add (argv, "-i");
  g_ptr_array_add (argv, vmlinuz);
  g_ptr_array_add (argv, "-r");
  g_ptr_array_add (argv, ramdisk_filename);
  g_ptr_array_add (argv, "-p");
  g_ptr_array_add (argv, cmdline_filename);
  for (guint i = 0; i < keys->len; ++i)
    {
      gchar *key = g_ptr_array_index (keys, i);
      g_ptr_array_add (argv, "-k");
      g_ptr_array_add (argv, key);
      ot_journal_print (LOG_INFO, "s390x SE: key[%d]: %s", i + 1, key);
    }
  g_ptr_array_add (argv, "--no-verify");
  g_ptr_array_add (argv, "-o");
  g_ptr_array_add (argv, SECURE_EXECUTION_BOOT_IMAGE);
  g_ptr_array_add (argv, NULL);

  gint status = 0;
  if (!g_spawn_sync (NULL, (char **)argv->pdata, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL,
                     &status, error))
    return glnx_prefix_error (error, "s390x SE: spawning genprotimg");

  if (!g_spawn_check_exit_status (status, error))
    return glnx_prefix_error (error, "s390x SE: `genprotimg` failed");

  ot_journal_print (LOG_INFO, "s390x SE: `%s` generated", SECURE_EXECUTION_BOOT_IMAGE);
  return TRUE;
}

static gboolean
_ostree_secure_execution_call_zipl (GError **error)
{
  int status = 0;
  const char *const zipl_argv[] = {
    "zipl", "-V", "-t", SECURE_EXECUTION_MOUNTPOINT, "-i", SECURE_EXECUTION_BOOT_IMAGE, NULL
  };
  if (!g_spawn_sync (NULL, (char **)zipl_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL,
                     &status, error))
    return glnx_prefix_error (error, "s390x SE: spawning zipl");

  if (!g_spawn_check_exit_status (status, error))
    return glnx_prefix_error (error, "s390x SE: `zipl` failed");

  ot_journal_print (LOG_INFO, "s390x SE: `sdboot` zipled");
  return TRUE;
}

static gboolean
_ostree_secure_execution_enable (OstreeBootloaderZipl *self, int bootversion, GPtrArray *keys,
                                 GCancellable *cancellable, GError **error)
{
  g_autofree gchar *vmlinuz = NULL;
  g_autofree gchar *initramfs = NULL;
  g_autofree gchar *options = NULL;

  gboolean rc = _ostree_secure_execution_mount (error)
                && _ostree_secure_execution_get_bls_config (self, bootversion, &vmlinuz, &initramfs,
                                                            &options, cancellable, error)
                && _ostree_secure_execution_generate_sdboot (vmlinuz, initramfs, options, keys,
                                                             cancellable, error)
                && _ostree_secure_execution_call_zipl (error)
                && _ostree_secure_execution_umount (error);

  return rc;
}

static gboolean
_ostree_bootloader_zipl_post_bls_sync (OstreeBootloader *bootloader, int bootversion,
                                       GCancellable *cancellable, GError **error)
{
  OstreeBootloaderZipl *self = OSTREE_BOOTLOADER_ZIPL (bootloader);

  // This can happen in a unit testing environment; at some point what we want to do here
  // is move all of the zipl logic to a systemd unit instead that's keyed of
  // ostree-finalize-staged.service.
  if (getuid () != 0)
    return TRUE;

  // If we're in a booted deployment, we don't need to spawn a container.
  // Also avoid containerizing if there's no deployments to target, which shouldn't
  // generally happen.
  OstreeDeployment *target_deployment;
  if (self->sysroot->booted_deployment || self->sysroot->deployments->len == 0)
    {
      target_deployment = NULL;
    }
  else
    {
      g_assert_cmpint (self->sysroot->deployments->len, >, 0);
      target_deployment = self->sysroot->deployments->pdata[0];
    }

  if (!glnx_fstatat_allow_noent (self->sysroot->sysroot_fd, zipl_requires_execute_path, NULL, 0,
                                 error))
    return FALSE;

  /* If there's no stamp file, nothing to do */
  if (errno == ENOENT)
    return TRUE;

  /* Try with Secure Execution */
  gboolean se_enabled = FALSE;
  if (!_ostree_secure_execution_is_enabled (&se_enabled, cancellable, error))
    return FALSE;
  if (se_enabled)
    {
      g_autoptr (GPtrArray) keys = NULL;
      if (!_ostree_secure_execution_get_keys (&keys, cancellable, error))
        return FALSE;
      if (!keys || keys->len == 0)
        return glnx_throw (error, "s390x SE: no keys");
      return _ostree_secure_execution_enable (self, bootversion, keys, cancellable, error);
    }
  /* Fallback to non-SE setup */
  gboolean sb_enabled = FALSE;
  if (!_ostree_secure_boot_is_enabled (&sb_enabled, cancellable, error))
    return FALSE;
  const char *const zipl_argv[]
      = { "zipl", "--secure", (sb_enabled == TRUE) ? "1" : "auto", "-V", NULL };
  int estatus;
  if (target_deployment != NULL)
    {
      g_debug ("executing zipl in deployment root");
      g_autofree char *deployment_path
          = ostree_sysroot_get_deployment_dirpath (self->sysroot, target_deployment);
      glnx_autofd int deployment_dfd = -1;
      if (!glnx_opendirat (self->sysroot->sysroot_fd, deployment_path, TRUE, &deployment_dfd,
                           error))
        return FALSE;

      g_autofree char *sysroot_boot
          = g_build_filename (gs_file_get_path_cached (self->sysroot->path), "boot", NULL);
      const char *bwrap_args[] = { "--bind", sysroot_boot, "/boot", NULL };
      if (!_ostree_sysroot_run_in_deployment (deployment_dfd, bwrap_args, zipl_argv, &estatus, NULL,
                                              error))
        return glnx_prefix_error (error, "Failed to invoke zipl");
    }
  else
    {
      g_debug ("executing zipl from booted system");
      if (!g_spawn_sync (NULL, (char **)zipl_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL,
                         NULL, &estatus, error))
        return FALSE;
    }
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
