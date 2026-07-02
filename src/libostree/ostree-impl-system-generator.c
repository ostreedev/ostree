/*
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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

#include <errno.h>
#include <gio/gunixoutputstream.h>
#include <glib-unix.h>
#include <stdio.h>
#ifdef HAVE_LIBMOUNT
#include <libmount.h>
#endif
#include "otutil.h"
#include <stdbool.h>
#include <sys/statvfs.h>

#include "ostree-cmd-private.h"
#include "ostree-core-private.h"
#include "ostree-mount-util.h"
#include "ostree-sysroot-private.h"
#include "otcore.h"

#ifdef HAVE_LIBMOUNT
typedef FILE OtLibMountFile;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OtLibMountFile, endmntent)

/* Taken from systemd path-util.c */
static bool
is_path (const char *p)
{
  return !!strchr (p, '/');
}

/* Taken from systemd path-util.c */
static char *
path_kill_slashes (char *path)
{
  char *f, *t;
  bool slash = false;

  /* Removes redundant inner and trailing slashes. Modifies the
   * passed string in-place.
   *
   * For example: ///foo///bar/ becomes /foo/bar
   */

  for (f = path, t = path; *f; f++)
    {
      if (*f == '/')
        {
          slash = true;
          continue;
        }

      if (slash)
        {
          slash = false;
          *(t++) = '/';
        }

      *(t++) = *f;
    }

  /* Special rule, if we are talking of the root directory, a
     trailing slash is good */

  if (t == path && slash)
    *(t++) = '/';

  *t = 0;
  return path;
}

#endif

/* Forcibly enable our internal units, since we detected ostree= on the kernel cmdline */
static gboolean
require_internal_units (const char *normal_dir, const char *early_dir, const char *late_dir,
                        GError **error)
{
#ifdef SYSTEM_DATA_UNIT_PATH
  GCancellable *cancellable = NULL;

  glnx_autofd int normal_dir_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, normal_dir, TRUE, &normal_dir_dfd, error))
    return FALSE;

  if (!glnx_shutil_mkdir_p_at (normal_dir_dfd, "local-fs.target.requires", 0755, cancellable,
                               error))
    return FALSE;
  if (symlinkat (SYSTEM_DATA_UNIT_PATH "/ostree-remount.service", normal_dir_dfd,
                 "local-fs.target.requires/ostree-remount.service")
      < 0)
    return glnx_throw_errno_prefix (error, "symlinkat");

  if (symlinkat (SYSTEM_DATA_UNIT_PATH "/ostree-relabel-etc.service", normal_dir_dfd,
                 "local-fs.target.requires/ostree-relabel-etc.service")
      < 0)
    return glnx_throw_errno_prefix (error, "symlinkat");

  if (!glnx_shutil_mkdir_p_at (normal_dir_dfd, "multi-user.target.wants", 0755, cancellable, error))
    return FALSE;
  if (symlinkat (SYSTEM_DATA_UNIT_PATH "/ostree-boot-complete.service", normal_dir_dfd,
                 "multi-user.target.wants/ostree-boot-complete.service")
      < 0)
    return glnx_throw_errno_prefix (error, "symlinkat");

  return TRUE;
#else
  return glnx_throw (error, "Not implemented");
#endif
}

// Resolve symlink to return osname
static gboolean
_ostree_sysroot_parse_bootlink_aboot (const char *bootlink, char **out_osname, GError **error)
{
  static gsize regex_initialized;
  static GRegex *regex;
  g_autofree char *symlink_val = glnx_readlinkat_malloc (-1, bootlink, NULL, error);
  if (!symlink_val)
    return glnx_prefix_error (error, "Failed to read '%s' symlink", bootlink);

  if (g_once_init_enter (&regex_initialized))
    {
      regex = g_regex_new ("^deploy/([^/]+)/", 0, 0, NULL);
      g_assert (regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  g_autoptr (GMatchInfo) match = NULL;
  if (!g_regex_match (regex, symlink_val, 0, &match))
    return glnx_throw (error,
                       "Invalid aboot symlink in /ostree, expected symlink to resolve to "
                       "deploy/OSNAME/... instead it resolves to '%s'",
                       symlink_val);

  *out_osname = g_match_info_fetch (match, 1);
  return TRUE;
}

/* Generate a drop-in for a mount unit to set DefaultDependencies=no.
 *
 * By default, systemd auto-generates mount units from /proc/self/mountinfo with
 * DefaultDependencies=yes, which includes Conflicts=umount.target. This causes
 * the mount to be unmounted during soft-reboot shutdown. Since soft-reboot doesn't
 * re-run the initramfs, these mounts are never remounted, breaking bootc/ostree.
 *
 * By generating a drop-in, we override just the DefaultDependencies setting
 * while letting systemd handle the actual mount parameters (What=, etc.) from
 * the existing mount in /proc/self/mountinfo.
 *
 * @mount_unit: The mount unit name (e.g., "sysroot.mount", "boot.mount")
 * @mount_point: The mount point path for the comment (e.g., "/sysroot", "/boot")
 */
static gboolean
generate_mount_unit_dropin (int normal_dir_dfd, const char *mount_unit, const char *mount_point,
                            GError **error)
{
  GCancellable *cancellable = NULL;

  /* Create the drop-in directory (e.g., sysroot.mount.d) */
  g_autofree char *dropin_dir = g_strdup_printf ("%s.d", mount_unit);
  if (!glnx_shutil_mkdir_p_at (normal_dir_dfd, dropin_dir, 0755, cancellable, error))
    return FALSE;

  g_auto (GLnxTmpfile) tmpf = {
    0,
  };
  if (!glnx_open_tmpfile_linkable_at (normal_dir_dfd, ".", O_WRONLY | O_CLOEXEC, &tmpf, error))
    return FALSE;
  g_autoptr (GOutputStream) outstream = g_unix_output_stream_new (tmpf.fd, FALSE);
  gsize bytes_written;

  /* Generate drop-in to set DefaultDependencies=no.
   *
   * Key points:
   * - DefaultDependencies=no prevents Conflicts=umount.target from being added
   * - This allows the mount to survive soft-reboot
   * - The actual mount (What=, Where=, etc.) comes from /proc/self/mountinfo
   */
  if (!g_output_stream_printf (outstream, &bytes_written, cancellable, error,
                               "##\n# Automatically generated by ostree-system-generator\n"
                               "# Preserve %s across soft-reboot\n##\n\n"
                               "[Unit]\n"
                               "DefaultDependencies=no\n"
                               "After=local-fs-pre.target\n"
                               "Before=local-fs.target\n",
                               mount_point))
    return FALSE;
  if (!g_output_stream_flush (outstream, cancellable, error))
    return FALSE;
  g_clear_object (&outstream);
  if (!glnx_fchmod (tmpf.fd, 0644, error))
    return FALSE;

  g_autofree char *dropin_path = g_strdup_printf ("%s/ostree-softreboot.conf", dropin_dir);
  if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_NOREPLACE, normal_dir_dfd, dropin_path,
                             error))
    return FALSE;

  return TRUE;
}

/* Generate a drop-in for sysroot.mount to preserve it across soft-reboot.
 *
 * /sysroot is special: it's mounted in the initramfs and can never be
 * remounted without re-running the initramfs. Since soft-reboot skips
 * the initramfs, we must prevent systemd from unmounting it.
 */
static gboolean
sysroot_mount_generator (const char *normal_dir, GError **error)
{
  glnx_autofd int normal_dir_dfd = -1;

  if (!glnx_opendirat (AT_FDCWD, normal_dir, TRUE, &normal_dir_dfd, error))
    return FALSE;

  if (!generate_mount_unit_dropin (normal_dir_dfd, "sysroot.mount", "/sysroot", error))
    return FALSE;

  return TRUE;
}

/* Generate boot.mount for /boot when it's on the same partition as /sysroot.
 *
 * When /boot is on a separate partition, systemd auto-generates a mount unit
 * from /proc/self/mountinfo and will remount it after soft-reboot. No action
 * needed in that case.
 *
 * When /boot is on the same partition (detected by /sysroot/boot/loader being
 * a symlink), a bind mount from /sysroot/boot is needed. Previously this was
 * done in the initramfs (ostree-prepare-root), but that breaks on bare
 * soft-reboot since the initramfs doesn't re-run. By generating boot.mount
 * here, the generator handles all three boot scenarios:
 *   1. Normal boot (generator runs from initramfs)
 *   2. Bare soft-reboot (generator re-runs)
 *   3. Staged deployment soft-reboot (generator re-runs for new root)
 *
 * See: https://github.com/ostreedev/ostree/pull/3487
 *      https://github.com/ostreedev/ostree/pull/3571
 */
static gboolean
boot_mount_generator (const char *normal_dir, GError **error)
{
  GCancellable *cancellable = NULL;
  static const char boot_path[] = "/boot";
  struct stat stbuf;

  /* Check if /boot is on the same partition as /sysroot by looking for
   * /sysroot/boot/loader as a symlink. This is the same check used by
   * otcore_mount_boot() in the initramfs path.
   */
  if (!(lstat ("/sysroot/boot/loader", &stbuf) == 0 && S_ISLNK (stbuf.st_mode)))
    return TRUE; /* /boot is a separate partition, systemd handles it */

  /* Verify the target /boot directory exists */
  if (!(lstat ("/boot", &stbuf) == 0 && S_ISDIR (stbuf.st_mode)))
    return TRUE;

  glnx_autofd int normal_dir_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, normal_dir, TRUE, &normal_dir_dfd, error))
    return FALSE;

  g_auto (GLnxTmpfile) tmpf = {
    0,
  };
  if (!glnx_open_tmpfile_linkable_at (normal_dir_dfd, ".", O_WRONLY | O_CLOEXEC, &tmpf, error))
    return FALSE;
  g_autoptr (GOutputStream) outstream = g_unix_output_stream_new (tmpf.fd, FALSE);
  gsize bytes_written;

  /* Generate a boot.mount unit that bind-mounts /sysroot/boot to /boot.
   *
   * We use DefaultDependencies=no for the same reasons as var.mount:
   * to avoid implicit device ordering that can stall after soft-reboot.
   * Since this is a bind mount from /sysroot, we only need sysroot.mount.
   */
  if (!g_output_stream_printf (outstream, &bytes_written, cancellable, error,
                               "##\n# Automatically generated by ostree-system-generator\n"
                               "# Bind mount /boot from /sysroot/boot (same partition)\n##\n\n"
                               "[Unit]\n"
                               "Documentation=man:ostree(1)\n"
                               "DefaultDependencies=no\n"
                               "After=local-fs-pre.target sysroot.mount\n"
                               "Before=local-fs.target\n"
                               "\n"
                               "[Mount]\n"
                               "Where=%s\n"
                               "What=/sysroot/boot\n"
                               "Options=bind\n",
                               boot_path))
    return FALSE;
  if (!g_output_stream_flush (outstream, cancellable, error))
    return FALSE;
  g_clear_object (&outstream);
  if (!glnx_fchmod (tmpf.fd, 0644, error))
    return FALSE;
  if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_NOREPLACE, normal_dir_dfd, "boot.mount",
                             error))
    return FALSE;

  /* Ensure it's pulled in by local-fs.target */
  if (!glnx_shutil_mkdir_p_at (normal_dir_dfd, "local-fs.target.requires", 0755, cancellable,
                               error))
    return FALSE;
  if (symlinkat ("../boot.mount", normal_dir_dfd, "local-fs.target.requires/boot.mount") < 0)
    return glnx_throw_errno_prefix (error, "symlinkat");

  return TRUE;
}

/* Generate var.mount */
static gboolean
fstab_generator (const char *ostree_target, const bool is_aboot, const char *normal_dir,
                 const char *early_dir, const char *late_dir, GError **error)
{
#ifdef HAVE_LIBMOUNT
  /* Not currently cancellable, but define a var in case we care later */
  GCancellable *cancellable = NULL;
  /* Some path constants to avoid typos */
  static const char fstab_path[] = "/etc/fstab";
  static const char var_path[] = "/var";

  /* Written by ostree-sysroot-deploy.c. We parse out the stateroot here since we
   * need to know it to mount /var. Unfortunately we can't easily use the
   * libostree API to find the booted deployment since /boot might not have been
   * mounted yet.
   */
  g_autofree char *stateroot = NULL;
  if (is_aboot)
    {
      if (!_ostree_sysroot_parse_bootlink_aboot (ostree_target, &stateroot, error))
        return glnx_prefix_error (error, "Parsing aboot stateroot");
    }
  else if (!_ostree_sysroot_parse_bootlink (ostree_target, NULL, &stateroot, NULL, NULL, error))
    return glnx_prefix_error (error, "Parsing stateroot");

  /* Load /etc/fstab if it exists, and look for a /var mount */
  g_autoptr (OtLibMountFile) fstab = setmntent (fstab_path, "re");
  gboolean found_var_mnt = FALSE;
  if (!fstab)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "Reading %s", fstab_path);
    }
  else
    {
      /* Parse it */
      struct mntent *me;
      while ((me = getmntent (fstab)))
        {
          g_autofree char *where = g_strdup (me->mnt_dir);
          if (is_path (where))
            path_kill_slashes (where);

          /* We're only looking for /var here */
          if (strcmp (where, var_path) != 0)
            continue;

          found_var_mnt = TRUE;
          break;
        }
    }

  /* If we found /var, we're done */
  if (found_var_mnt)
    return TRUE;

  /* Prepare to write to the output unit dir; we use the "normal" dir
   * that overrides /usr, but not /etc.
   */
  glnx_autofd int normal_dir_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, normal_dir, TRUE, &normal_dir_dfd, error))
    return FALSE;

  /* Generate our bind mount unit */
  const char *stateroot_var_path = glnx_strjoina ("/sysroot/ostree/deploy/", stateroot, "/var");

  g_auto (GLnxTmpfile) tmpf = {
    0,
  };
  if (!glnx_open_tmpfile_linkable_at (normal_dir_dfd, ".", O_WRONLY | O_CLOEXEC, &tmpf, error))
    return FALSE;
  g_autoptr (GOutputStream) outstream = g_unix_output_stream_new (tmpf.fd, FALSE);
  gsize bytes_written;
  /* This code is inspired by systemd's fstab-generator.c.
   *
   * Note that our unit doesn't run if systemd.volatile is enabled;
   * see https://github.com/ostreedev/ostree/pull/856
   *
   * To avoid having submounts of /var propagate into $stateroot/var, the mount
   * is made with slave+shared propagation. This means that /var will receive
   * mount events from the parent /sysroot mount, but not vice versa. Adding a
   * shared peer group below the slave group means that submounts of /var will
   * inherit normal shared propagation. See mount_namespaces(7), Linux
   * Documentation/filesystems/sharedsubtree.txt and
   * https://github.com/ostreedev/ostree/issues/2086. This also happens in
   * ostree-prepare-root.c for the INITRAMFS_MOUNT_VAR case.
   *
   * We use DefaultDependencies=no to avoid implicit ordering dependencies that
   * can cause the mount to stall after a bare `systemctl soft-reboot`. Without
   * this, systemd may add implicit After= dependencies on device units, which
   * can get stuck in 'tentative' state while udev restarts after soft-reboot.
   * Since this is a bind mount from /sysroot (which survives soft-reboot), we
   * only need to wait for sysroot.mount and local-fs-pre.target.
   *
   * Note: We intentionally do NOT add Conflicts=umount.target or
   * Before=umount.target here. Adding those creates a dependency deadlock
   * with ostree-remount.service during soft-reboot shutdown, because:
   * - ostree-remount.service has After=var.mount and Before=local-fs.target
   * - Adding Conflicts=umount.target to var.mount creates circular ordering
   *   where umount.target waits for ostree-remount.service, which waits for
   *   local-fs.target, which waits for var.mount
   * Since /var survives soft-reboot (as a bind mount from /sysroot), systemd
   * catches it up from mountinfo and it doesn't need special unmount handling.
   *
   * See https://issues.redhat.com/browse/RHEL-154075
   */
  if (!g_output_stream_printf (outstream, &bytes_written, cancellable, error,
                               "##\n# Automatically generated by ostree-system-generator\n##\n\n"
                               "[Unit]\n"
                               "Documentation=man:ostree(1)\n"
                               "DefaultDependencies=no\n"
                               "ConditionKernelCommandLine=!systemd.volatile\n"
                               "After=local-fs-pre.target sysroot.mount\n"
                               "Before=local-fs.target\n"
                               "\n"
                               "[Mount]\n"
                               "Where=%s\n"
                               "What=%s\n"
                               "Options=bind,slave,shared\n",
                               var_path, stateroot_var_path))
    return FALSE;
  if (!g_output_stream_flush (outstream, cancellable, error))
    return FALSE;
  g_clear_object (&outstream);
  /* It should be readable */
  if (!glnx_fchmod (tmpf.fd, 0644, error))
    return FALSE;
  /* Error out if somehow it already exists, that'll help us debug conflicts */
  if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_NOREPLACE, normal_dir_dfd, "var.mount",
                             error))
    return FALSE;

  /* And ensure it's required; newer systemd will auto-inject fs dependencies
   * via RequiresMountsFor and the like, but on older versions (e.g. CentOS) we
   * need this. It's what the fstab generator does.  And my mother always said,
   * listen to the fstab generator.
   */
  if (!glnx_shutil_mkdir_p_at (normal_dir_dfd, "local-fs.target.requires", 0755, cancellable,
                               error))
    return FALSE;
  if (symlinkat ("../var.mount", normal_dir_dfd, "local-fs.target.requires/var.mount") < 0)
    return glnx_throw_errno_prefix (error, "symlinkat");

  return TRUE;
#else
  return glnx_throw (error, "Not implemented");
#endif
}

/* Implementation of ostree-system-generator */
gboolean
_ostree_impl_system_generator (const char *normal_dir, const char *early_dir, const char *late_dir,
                               GError **error)
{
  /* We conflict with the magic ostree-mount-deployment-var file for ostree-prepare-root.
   * If this file is present, we have nothing to do! */
  if (unlinkat (AT_FDCWD, INITRAMFS_MOUNT_VAR, 0) == 0)
    return TRUE;

#ifdef OSTREE_PREPARE_ROOT_STATIC
  // Create /run/ostree-booted now, because other things rely on it.
  // If the system compiled with a static prepareroot, then our generator
  // makes a hard assumption that ostree is in use.
  touch_run_ostree ();
#else
  // If we're not booted via ostree, do nothing
  if (!glnx_fstatat_allow_noent (AT_FDCWD, OTCORE_RUN_OSTREE, NULL, 0, error))
    return FALSE;
  if (errno == ENOENT)
    return TRUE;
#endif

  g_autofree char *cmdline = read_proc_cmdline ();
  if (!cmdline)
    return glnx_throw (error, "Failed to read /proc/cmdline");

  g_autofree char *ostree_target = NULL;
  gboolean is_aboot = false;
  if (!otcore_get_ostree_target (cmdline, &is_aboot, &ostree_target, error))
    return glnx_prefix_error (error, "Invalid aboot ostree target");

  /* If no `ostree=` karg exists, gracefully no-op.
   * This could happen in CoreOS live environments, where we hackily mock
   * the `ostree=` karg for `ostree-prepare-root.service` specifically, but
   * otherwise that karg doesn't exist on the real command-line. */
  if (!ostree_target)
    return TRUE;

  if (!require_internal_units (normal_dir, early_dir, late_dir, error))
    return FALSE;
  if (!sysroot_mount_generator (normal_dir, error))
    return FALSE;
  if (!boot_mount_generator (normal_dir, error))
    return FALSE;
  if (!fstab_generator (ostree_target, is_aboot, normal_dir, early_dir, late_dir, error))
    return FALSE;

  return TRUE;
}
