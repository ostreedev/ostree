/* -*- c-file-style: "gnu" -*-
 * Switch to new root directory and start init.
 *
 * Copyright 2011,2012,2013 Colin Walters <walters@verbum.org>
 *
 * Based on code from util-linux/sys-utils/switch_root.c,
 * Copyright 2002-2009 Red Hat, Inc.  All rights reserved.
 * Authors:
 *  Peter Jones <pjones@redhat.com>
 *  Jeremy Katz <katzj@redhat.com>
 *
 * Relicensed with permission to LGPLv2+.
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

/* The high level goal of ostree-prepare-root.service is to run inside
 * the initial ram disk (if one is in use) and set up the `/` mountpoint
 * to be the deployment root, using the ostree= kernel commandline
 * argument to find the target deployment root.
 *
 * It's really the heart of how ostree works - basically multiple
 * hardlinked chroot() targets are maintained, this one does the equivalent
 * of chroot().
 *
 * # ostree-prepare-root.service
 *
 * If using systemd, an excellent reference is `man bootup`.  This
 * service runs Before=initrd-root-fs.target.  At this point it's
 * assumed that the block storage and root filesystem are mounted at
 * /sysroot - i.e. /sysroot points to the *physical* root before
 * this service runs.  After, `/` is the deployment root, and /sysroot is
 * the physical root.
 *
 * # Running as pid 1
 *
 * There is also a secondary mode for this service when an initrd isn't
 * used - instead the binary must be statically linked (and the kernel
 * must have mounted the rootfs itself) - then we set things up and
 * exec the real init directly.  This can be popular in embedded
 * systems to increase bootup speed.
 */

#include "config.h"

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

/* We can't include both linux/fs.h and sys/mount.h, so define these directly */
#define FS_VERITY_FL 0x00100000 /* Verity protected inode */
#define FS_IOC_GETFLAGS _IOR ('f', 1, long)

// The name of the composefs metadata root
#define OSTREE_COMPOSEFS_NAME ".ostree.cfs"

#if defined(HAVE_LIBSYSTEMD) && !defined(OSTREE_PREPARE_ROOT_STATIC)
#define USE_LIBSYSTEMD
#endif

#ifdef USE_LIBSYSTEMD
#include <systemd/sd-journal.h>
#define OSTREE_PREPARE_ROOT_DEPLOYMENT_MSG \
  SD_ID128_MAKE (71, 70, 33, 6a, 73, ba, 46, 01, ba, d3, 1a, f8, 88, aa, 0d, f7)
#endif

// A temporary mount point
#define TMP_SYSROOT "/sysroot.tmp"

#ifdef HAVE_COMPOSEFS
#include <libcomposefs/lcfs-mount.h>
#include <libcomposefs/lcfs-writer.h>
#endif

#include "ostree-mount-util.h"

typedef enum
{
  OSTREE_COMPOSEFS_MODE_OFF,    /* Never use composefs */
  OSTREE_COMPOSEFS_MODE_MAYBE,  /* Use if supported and image exists in deploy */
  OSTREE_COMPOSEFS_MODE_ON,     /* Always use (and fail if not working) */
  OSTREE_COMPOSEFS_MODE_SIGNED, /* Always use and require it to be signed */
  OSTREE_COMPOSEFS_MODE_DIGEST, /* Always use and require specific digest */
} OstreeComposefsMode;

static inline bool
sysroot_is_configured_ro (const char *sysroot)
{
  char *config_path = NULL;
  assert (asprintf (&config_path, "%s/ostree/repo/config", sysroot) != -1);
  FILE *f = fopen (config_path, "r");
  if (!f)
    {
      fprintf (stderr, "Missing expected repo config: %s\n", config_path);
      free (config_path);
      return false;
    }
  free (config_path);

  bool ret = false;
  char *line = NULL;
  size_t len = 0;
  /* Note getline() will reuse the previous buffer */
  bool in_sysroot = false;
  while (getline (&line, &len, f) != -1)
    {
      /* This is an awful hack to avoid depending on GLib in the
       * initramfs right now.
       */
      if (strstr (line, "[sysroot]") == line)
        in_sysroot = true;
      else if (*line == '[')
        in_sysroot = false;
      else if (in_sysroot && strstr (line, "readonly=true") == line)
        {
          ret = true;
          break;
        }
    }

  fclose (f);
  free (line);
  return ret;
}

static char *
resolve_deploy_path (const char *root_mountpoint)
{
  char destpath[PATH_MAX];
  struct stat stbuf;
  char *deploy_path;
  autofree char *ostree_target = get_ostree_target ();
  if (!ostree_target)
    errx (EXIT_FAILURE, "No ostree= cmdline");

  if (snprintf (destpath, sizeof (destpath), "%s/%s", root_mountpoint, ostree_target) < 0)
    err (EXIT_FAILURE, "failed to assemble ostree target path");
  if (lstat (destpath, &stbuf) < 0)
    err (EXIT_FAILURE, "Couldn't find specified OSTree root '%s'", destpath);
  if (!S_ISLNK (stbuf.st_mode))
    errx (EXIT_FAILURE, "OSTree target is not a symbolic link: %s", destpath);
  deploy_path = realpath (destpath, NULL);
  if (deploy_path == NULL)
    err (EXIT_FAILURE, "realpath(%s) failed", destpath);
  if (stat (deploy_path, &stbuf) < 0)
    err (EXIT_FAILURE, "stat(%s) failed", deploy_path);
    /* Quiet logs if there's no journal */
#ifdef USE_LIBSYSTEMD
  const char *resolved_path = deploy_path + strlen (root_mountpoint);
  sd_journal_send ("MESSAGE=Resolved OSTree target to: %s", deploy_path,
                   "MESSAGE_ID=" SD_ID128_FORMAT_STR,
                   SD_ID128_FORMAT_VAL (OSTREE_PREPARE_ROOT_DEPLOYMENT_MSG), "DEPLOYMENT_PATH=%s",
                   resolved_path, "DEPLOYMENT_DEVICE=%" PRIu64, (uint64_t)stbuf.st_dev,
                   "DEPLOYMENT_INODE=%" PRIu64, (uint64_t)stbuf.st_ino, NULL);
#endif
  return deploy_path;
}

static int
pivot_root (const char *new_root, const char *put_old)
{
  return syscall (__NR_pivot_root, new_root, put_old);
}

int
main (int argc, char *argv[])
{
  char srcpath[PATH_MAX];

  /* If we're pid 1, that means there's no initramfs; in this situation
   * various defaults change:
   *
   * - Assume that the target root is /
   * - Quiet logging as there's no journal
   * etc.
   */
  bool running_as_pid1 = (getpid () == 1);

  const char *root_arg = NULL;
  bool we_mounted_proc = false;
  if (running_as_pid1)
    {
      root_arg = "/";
    }
  else
    {
      if (argc < 2)
        err (EXIT_FAILURE, "usage: ostree-prepare-root SYSROOT");
      root_arg = argv[1];
    }
#ifdef USE_LIBSYSTEMD
  sd_journal_send ("MESSAGE=preparing sysroot at %s", root_arg, NULL);
#endif

  struct stat stbuf;
  if (stat ("/proc/cmdline", &stbuf) < 0)
    {
      if (errno != ENOENT)
        err (EXIT_FAILURE, "stat(\"/proc/cmdline\") failed");
      /* We need /proc mounted for /proc/cmdline and realpath (on musl) to
       * work: */
      if (mount ("proc", "/proc", "proc", MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to mount proc on /proc");
      we_mounted_proc = 1;
    }

  /* This is the final target where we should prepare the rootfs.  The usual
   * case with systemd in the initramfs is that root_mountpoint = "/sysroot".
   * In the fastboot embedded case we're pid1 and will setup / ourself, and
   * then root_mountpoint = "/".
   * */
  const char *root_mountpoint = realpath (root_arg, NULL);
  if (root_mountpoint == NULL)
    err (EXIT_FAILURE, "realpath(\"%s\")", root_arg);
  char *deploy_path = resolve_deploy_path (root_mountpoint);

  if (we_mounted_proc)
    {
      /* Leave the filesystem in the state that we found it: */
      if (umount ("/proc"))
        err (EXIT_FAILURE, "failed to umount proc from /proc");
    }

  OstreeComposefsMode composefs_mode = OSTREE_COMPOSEFS_MODE_MAYBE;
  autofree char *ot_composefs = read_proc_cmdline_key ("ot-composefs");
  char *composefs_digest = NULL;
  if (ot_composefs)
    {
      if (strcmp (ot_composefs, "off") == 0)
        composefs_mode = OSTREE_COMPOSEFS_MODE_OFF;
      else if (strcmp (ot_composefs, "maybe") == 0)
        composefs_mode = OSTREE_COMPOSEFS_MODE_MAYBE;
      else if (strcmp (ot_composefs, "on") == 0)
        composefs_mode = OSTREE_COMPOSEFS_MODE_ON;
      else if (strcmp (ot_composefs, "signed") == 0)
        composefs_mode = OSTREE_COMPOSEFS_MODE_SIGNED;
      else if (strncmp (ot_composefs, "digest=", strlen ("digest=")) == 0)
        {
          composefs_mode = OSTREE_COMPOSEFS_MODE_DIGEST;
          composefs_digest = ot_composefs + strlen ("digest=");
        }
      else
        err (EXIT_FAILURE, "Unsupported ot-composefs option: '%s'", ot_composefs);
    }

#ifndef HAVE_COMPOSEFS
  if (composefs_mode == OSTREE_COMPOSEFS_MODE_MAYBE)
    composefs_mode = OSTREE_COMPOSEFS_MODE_OFF;
  (void)composefs_digest;
#endif

  /* Query the repository configuration - this is an operating system builder
   * choice.  More info: https://github.com/ostreedev/ostree/pull/1767
   */
  const bool sysroot_readonly = sysroot_is_configured_ro (root_arg);
  const bool sysroot_currently_writable = !path_is_on_readonly_fs (root_arg);
#ifdef USE_LIBSYSTEMD
  sd_journal_send ("MESSAGE=filesystem at %s currently writable: %d", root_arg,
                   (int)sysroot_currently_writable, NULL);
  sd_journal_send ("MESSAGE=sysroot.readonly configuration value: %d", (int)sysroot_readonly, NULL);
#endif

  /* Work-around for a kernel bug: for some reason the kernel
   * refuses switching root if any file systems are mounted
   * MS_SHARED. Hence remount them MS_PRIVATE here as a
   * work-around.
   *
   * https://bugzilla.redhat.com/show_bug.cgi?id=847418 */
  if (mount (NULL, "/", NULL, MS_REC | MS_PRIVATE | MS_SILENT, NULL) < 0)
    err (EXIT_FAILURE, "failed to make \"/\" private mount");

  if (mkdir (TMP_SYSROOT, 0755) < 0)
    err (EXIT_FAILURE, "couldn't create temporary sysroot %s", TMP_SYSROOT);

  /* Run in the deploy_path dir so we can use relative paths below */
  if (chdir (deploy_path) < 0)
    err (EXIT_FAILURE, "failed to chdir to deploy_path");

  bool using_composefs = false;

  /* We construct the new sysroot in /sysroot.tmp, which is either the composfs
     mount or a bind mount of the deploy-dir */
  if (composefs_mode != OSTREE_COMPOSEFS_MODE_OFF)
    {
#ifdef HAVE_COMPOSEFS
      const char *objdirs[] = { "/sysroot/ostree/repo/objects" };
      struct lcfs_mount_options_s cfs_options = {
        objdirs,
        1,
      };

      if (composefs_mode == OSTREE_COMPOSEFS_MODE_SIGNED)
        errx (EXIT_FAILURE, "composefs signature not supported");

      cfs_options.flags = LCFS_MOUNT_FLAGS_READONLY;

      if (snprintf (srcpath, sizeof (srcpath), "%s/.ostree.mnt", deploy_path) < 0)
        err (EXIT_FAILURE, "failed to assemble /boot/loader path");
      cfs_options.image_mountdir = srcpath;

      if (composefs_mode == OSTREE_COMPOSEFS_MODE_DIGEST)
        {
          cfs_options.flags |= LCFS_MOUNT_FLAGS_REQUIRE_VERITY;
          cfs_options.expected_fsverity_digest = composefs_digest;
        }

#ifdef USE_LIBSYSTEMD
      if (composefs_mode == OSTREE_COMPOSEFS_MODE_MAYBE)
        sd_journal_send ("MESSAGE=Trying to mount composefs rootfs", NULL);
      else if (composefs_mode == OSTREE_COMPOSEFS_MODE_DIGEST)
        sd_journal_send ("MESSAGE=Mounting composefs rootfs with expected digest '%s'",
                         composefs_digest, NULL);
      else
        sd_journal_send ("MESSAGE=Mounting composefs rootfs", NULL);
#endif

      if (lcfs_mount_image (OSTREE_COMPOSEFS_NAME, TMP_SYSROOT, &cfs_options) == 0)
        {
          int fd = open (_OSTREE_COMPOSEFS_ROOT_STAMP, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
          if (fd < 0)
            err (EXIT_FAILURE, "failed to create %s", _OSTREE_COMPOSEFS_ROOT_STAMP);
          (void)close (fd);

          using_composefs = 1;
        }
      else
        {
#ifdef USE_LIBSYSTEMD
          if (errno == ENOVERITY)
            sd_journal_send ("MESSAGE=No verity in composefs image", NULL);
          else if (errno == EWRONGVERITY)
            sd_journal_send ("MESSAGE=Wrong verity digest in composefs image", NULL);
          else if (errno == ENOSIGNATURE)
            sd_journal_send ("MESSAGE=Missing signature in composefs image", NULL);
          else
            sd_journal_send ("MESSAGE=Mounting composefs image failed: %s", strerror (errno), NULL);
#endif
        }
#else
      err (EXIT_FAILURE, "Composefs not supported");
#endif
    }

  if (!using_composefs)
    {
      if (composefs_mode > OSTREE_COMPOSEFS_MODE_MAYBE)
        err (EXIT_FAILURE, "Failed to mount composefs");

      /* The deploy root starts out bind mounted to sysroot.tmp */
      if (mount (deploy_path, TMP_SYSROOT, NULL, MS_BIND | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to make initial bind mount %s", deploy_path);
    }
  else
    {
#ifdef USE_LIBSYSTEMD
      sd_journal_send ("MESSAGE=Mounted composefs", NULL);
#endif
    }

  /* This will result in a system with /sysroot read-only. Thus, two additional
   * writable bind-mounts (for /etc and /var) are required later on. */
  if (sysroot_readonly)
    {
      if (!sysroot_currently_writable)
        errx (EXIT_FAILURE, "sysroot.readonly=true requires %s to be writable at this point",
              root_arg);
      /* Pass on the fact that we discovered a readonly sysroot to ostree-remount.service */
      int fd = open (_OSTREE_SYSROOT_READONLY_STAMP, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
      if (fd < 0)
        err (EXIT_FAILURE, "failed to create %s", _OSTREE_SYSROOT_READONLY_STAMP);
      (void)close (fd);
    }

  /* Prepare /boot.
   * If /boot is on the same partition, use a bind mount to make it visible
   * at /boot inside the deployment. */
  if (snprintf (srcpath, sizeof (srcpath), "%s/boot/loader", root_mountpoint) < 0)
    err (EXIT_FAILURE, "failed to assemble /boot/loader path");
  if (lstat (srcpath, &stbuf) == 0 && S_ISLNK (stbuf.st_mode))
    {
      if (lstat ("boot", &stbuf) == 0 && S_ISDIR (stbuf.st_mode))
        {
          if (snprintf (srcpath, sizeof (srcpath), "%s/boot", root_mountpoint) < 0)
            err (EXIT_FAILURE, "failed to assemble /boot path");
          if (mount (srcpath, TMP_SYSROOT "/boot", NULL, MS_BIND | MS_SILENT, NULL) < 0)
            err (EXIT_FAILURE, "failed to bind mount %s to boot", srcpath);
        }
    }

  /* Prepare /etc.
   * No action required if sysroot is writable. Otherwise, a bind-mount for
   * the deployment needs to be created and remounted as read/write. */
  if (sysroot_readonly || using_composefs)
    {
      /* Bind-mount /etc (at deploy path), and remount as writable. */
      if (mount ("etc", TMP_SYSROOT "/etc", NULL, MS_BIND | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to prepare /etc bind-mount at /sysroot.tmp/etc");
      if (mount (TMP_SYSROOT "/etc", TMP_SYSROOT "/etc", NULL, MS_BIND | MS_REMOUNT | MS_SILENT,
                 NULL)
          < 0)
        err (EXIT_FAILURE, "failed to make writable /etc bind-mount at /sysroot.tmp/etc");
    }

  /* Prepare /usr.
   * It may be either just a read-only bind-mount, or a persistent overlayfs. */
  if (lstat (".usr-ovl-work", &stbuf) == 0)
    {
      /* Do we have a persistent overlayfs for /usr?  If so, mount it now. */
      const char usr_ovl_options[]
          = "lowerdir=" TMP_SYSROOT "/usr,upperdir=.usr-ovl-upper,workdir=.usr-ovl-work";

      /* Except overlayfs barfs if we try to mount it on a read-only
       * filesystem.  For this use case I think admins are going to be
       * okay if we remount the rootfs here, rather than waiting until
       * later boot and `systemd-remount-fs.service`.
       */
      if (path_is_on_readonly_fs (TMP_SYSROOT))
        {
          if (mount (TMP_SYSROOT, TMP_SYSROOT, NULL, MS_REMOUNT | MS_SILENT, NULL) < 0)
            err (EXIT_FAILURE, "failed to remount rootfs writable (for overlayfs)");
        }

      if (mount ("overlay", TMP_SYSROOT "/usr", "overlay", MS_SILENT, usr_ovl_options) < 0)
        err (EXIT_FAILURE, "failed to mount /usr overlayfs");
    }
  else if (!using_composefs)
    {
      /* Otherwise, a read-only bind mount for /usr. (Not needed for composefs) */
      if (mount (TMP_SYSROOT "/usr", TMP_SYSROOT "/usr", NULL, MS_BIND | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to bind mount (class:readonly) /usr");
      if (mount (TMP_SYSROOT "/usr", TMP_SYSROOT "/usr", NULL,
                 MS_BIND | MS_REMOUNT | MS_RDONLY | MS_SILENT, NULL)
          < 0)
        err (EXIT_FAILURE, "failed to bind mount (class:readonly) /usr");
    }

  /* Prepare /var.
   * When a read-only sysroot is configured, this adds a dedicated bind-mount (to itself)
   * so that the stateroot location stays writable. */
  if (sysroot_readonly)
    {
      /* Bind-mount /var (at stateroot path), and remount as writable. */
      if (mount ("../../var", "../../var", NULL, MS_BIND | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to prepare /var bind-mount at %s", srcpath);
      if (mount ("../../var", "../../var", NULL, MS_BIND | MS_REMOUNT | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to make writable /var bind-mount at %s", srcpath);
    }

    /* When running under systemd, /var will be handled by a 'var.mount' unit outside
     * of initramfs.
     * Systemd auto-detection can be overridden by a marker file under /run. */
#ifdef HAVE_SYSTEMD_AND_LIBMOUNT
  bool mount_var = false;
#else
  bool mount_var = true;
#endif
  if (lstat (INITRAMFS_MOUNT_VAR, &stbuf) == 0)
    mount_var = true;

  /* If required, bind-mount `/var` in the deployment to the "stateroot", which is
   *  the shared persistent directory for a set of deployments.  More info:
   *  https://ostreedev.github.io/ostree/deployment/#stateroot-aka-osname-group-of-deployments-that-share-var
   */
  if (mount_var)
    {
      if (mount ("../../var", TMP_SYSROOT "/var", NULL, MS_BIND | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to bind mount ../../var to var");
    }

  /* We only stamp /run now if we're running in an initramfs, i.e. we're
   * not pid 1.  Otherwise it's handled later via ostree-system-generator.
   * https://mail.gnome.org/archives/ostree-list/2018-March/msg00012.html
   * https://github.com/ostreedev/ostree/pull/1675
   */
  if (!running_as_pid1)
    touch_run_ostree ();

  if (chdir (TMP_SYSROOT) < 0)
    err (EXIT_FAILURE, "failed to chdir to " TMP_SYSROOT);

  if (strcmp (root_mountpoint, "/") == 0)
    {
      /* pivot_root rotates two mount points around.  In this instance . (the
       * deploy location) becomes / and the existing / becomes /sysroot.  We
       * have to use pivot_root rather than mount --move in this instance
       * because our deploy location is mounted as a subdirectory of the real
       * sysroot, so moving sysroot would also move the deploy location.   In
       * reality attempting mount --move would fail with EBUSY. */
      if (pivot_root (".", "sysroot") < 0)
        err (EXIT_FAILURE, "failed to pivot_root to deployment");
    }
  else
    {
      /* In this instance typically we have our ready made-up up root at
       * /sysroot.tmp and the physical root at /sysroot (root_mountpoint).
       * We want to end up with our deploy root at /sysroot/ and the physical
       * root under /sysroot/sysroot as systemd will be responsible for
       * moving /sysroot to /.
       */
      if (mount (root_mountpoint, "sysroot", NULL, MS_MOVE | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to MS_MOVE '%s' to 'sysroot'", root_mountpoint);

      if (mount (".", root_mountpoint, NULL, MS_MOVE | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to MS_MOVE %s to %s", ".", root_mountpoint);

      if (chdir (root_mountpoint) < 0)
        err (EXIT_FAILURE, "failed to chdir to %s", root_mountpoint);

      if (rmdir (TMP_SYSROOT) < 0)
        err (EXIT_FAILURE, "couldn't remove temporary sysroot %s", TMP_SYSROOT);

      if (sysroot_readonly)
        {
          if (mount ("sysroot", "sysroot", NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_SILENT, NULL)
              < 0)
            err (EXIT_FAILURE, "failed to make /sysroot read-only");

          /* TODO(lucab): This will make the final '/' read-only.
           * Stabilize read-only '/sysroot' first, then enable this additional hardening too.
           *
           * if (mount (".", ".", NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_SILENT, NULL) < 0)
           *   err (EXIT_FAILURE, "failed to make / read-only");
           */
        }
    }

  /* The /sysroot mount needs to be private to avoid having a mount for e.g. /var/cache
   * also propagate to /sysroot/ostree/deploy/$stateroot/var/cache
   *
   * Now in reality, today this is overridden by systemd: the *actual* way we fix this up
   * is in ostree-remount.c.  But let's do it here to express the semantics we want
   * at the very start (perhaps down the line systemd will have compile/runtime option
   * to say that the initramfs environment did everything right from the start).
   */
  if (mount ("none", "sysroot", NULL, MS_PRIVATE | MS_SILENT, NULL) < 0)
    err (EXIT_FAILURE, "remounting 'sysroot' private");

  if (running_as_pid1)
    {
      execl ("/sbin/init", "/sbin/init", NULL);
      err (EXIT_FAILURE, "failed to exec init inside ostree");
    }
  else
    {
      exit (EXIT_SUCCESS);
    }
}
