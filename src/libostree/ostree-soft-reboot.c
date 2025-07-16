/* -*- c-file-style: "gnu" -*-
 * Soft reboot for ostree. This code was originally derived from ostree-prepare-root.c,
 * but is now significantly cut down to target specifically soft rebooting.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 */

#include "config.h"

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libglnx.h>
#include <linux/magic.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <ostree-core.h>
#include <ostree-sysroot-private.h>

#include "ostree-mount-util.h"
#include "ot-keyfile-utils.h"
#include "otcore.h"

/* This key configures the / mount in the deployment root */
#define ROOT_KEY "root"
#define ETC_KEY "etc"
#define TRANSIENT_KEY "transient"

gboolean
_ostree_prepare_soft_reboot (GError **error)
{
#ifdef HAVE_SOFT_REBOOT
  const char *sysroot_path = "/sysroot";
  const char *target_deployment = ".";

  g_autoptr (GKeyFile) config = otcore_load_config (AT_FDCWD, PREPARE_ROOT_CONFIG_PATH, error);
  if (!config)
    return FALSE;

  gboolean root_transient = FALSE;
  if (!ot_keyfile_get_boolean_with_default (config, ROOT_KEY, TRANSIENT_KEY, FALSE, &root_transient,
                                            error))
    return FALSE;

  g_autofree char *kernel_cmdline = read_proc_cmdline ();
  g_autoptr (RootConfig) rootfs_config
      = otcore_load_rootfs_config (kernel_cmdline, config, TRUE, error);
  if (!rootfs_config)
    return FALSE;

  if (rootfs_config->enabled != OT_TRISTATE_YES)
    return glnx_throw (error, "soft reboot not supported without composefs");

  GVariantBuilder metadata_builder;
  g_variant_builder_init (&metadata_builder, G_VARIANT_TYPE ("a{sv}"));

  if (!glnx_shutil_mkdir_p_at (AT_FDCWD, OTCORE_RUN_NEXTROOT, 0755, NULL, error))
    return FALSE;

  // Tracks if we did successfully enable it at runtime
  bool using_composefs = false;
  if (!otcore_mount_rootfs (rootfs_config, &metadata_builder, root_transient, sysroot_path,
                            target_deployment, OTCORE_RUN_NEXTROOT, &using_composefs, error))
    return glnx_prefix_error (error, "failed to mount composefs");

  if (!using_composefs)
    return glnx_throw (error, "failed to mount with composefs");

  if (!otcore_mount_etc (config, &metadata_builder, OTCORE_RUN_NEXTROOT, error))
    return FALSE;

  // And set up /sysroot. Here since we hardcode composefs, we also hardcode
  // having a read-only /sysroot.
  g_variant_builder_add (&metadata_builder, "{sv}", OTCORE_RUN_BOOTED_KEY_SYSROOT_RO,
                         g_variant_new_boolean (true));
  {
    struct mount_attr attr = { .attr_set = MOUNT_ATTR_RDONLY };
    glnx_autofd int sysroot_fd
        = open_tree (AT_FDCWD, sysroot_path, OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC);
    if (sysroot_fd < 0)
      return glnx_throw_errno_prefix (error, "open_tree(%s)", sysroot_path);
    if (mount_setattr (sysroot_fd, "", AT_EMPTY_PATH, &attr, sizeof (struct mount_attr)) < 0)
      return glnx_throw_errno_prefix (error, "syscall(mount_setattr) of sysroot");
    g_autofree char *target_sysroot = g_build_filename (OTCORE_RUN_NEXTROOT, "sysroot", NULL);
    if (move_mount (sysroot_fd, "", -1, target_sysroot, MOVE_MOUNT_F_EMPTY_PATH) < 0)
      return glnx_throw_errno_prefix (error, "syscall(move_mount) of sysroot");

    g_debug ("initialized /sysroot");
  }

  /* This can be used by other things to signal ostree is in use */
  {
    g_autoptr (GVariant) metadata = g_variant_ref_sink (g_variant_builder_end (&metadata_builder));
    const guint8 *buf = g_variant_get_data (metadata) ?: (guint8 *)"";
    if (!glnx_file_replace_contents_at (AT_FDCWD, OTCORE_RUN_NEXTROOT_BOOTED, buf,
                                        g_variant_get_size (metadata), 0, NULL, error))
      return FALSE;
  }

  return TRUE;
#else
  return glnx_throw (error, "soft reboot not supported");
#endif
}
