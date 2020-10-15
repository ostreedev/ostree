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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
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
 * If using systemd, an excellent reference is `man bootup`.  This
 * service runs Before=initrd-root-fs.target.  At this point it's
 * assumed that the block storage and root filesystem are mounted at
 * /sysroot - i.e. /sysroot points to the *physical* root before
 * this service runs.  After, `/` is the deployment root.
 *
 * There is also a secondary mode for this service when an initrd isn't
 * used - instead the binary must be statically linked (and the kernel
 * must have mounted the rootfs itself) - then we set things up and
 * exec the real init directly.  This can be popular in embedded
 * systems to increase bootup speed.
 */

#include "config.h"

#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <err.h>
#include <errno.h>
#include <ctype.h>

#if defined(HAVE_LIBSYSTEMD) && !defined(OSTREE_PREPARE_ROOT_STATIC)
#define USE_LIBSYSTEMD
#endif

#ifdef USE_LIBSYSTEMD
#include <systemd/sd-journal.h>
#define OSTREE_PREPARE_ROOT_DEPLOYMENT_MSG SD_ID128_MAKE(71,70,33,6a,73,ba,46,01,ba,d3,1a,f8,88,aa,0d,f7)
#endif

#include "ostree-mount-util.h"

/* Initialized early in main */
static bool running_as_pid1;

static inline bool
sysroot_is_configured_ro (const char *sysroot)
{
  char * config_path = NULL;
  assert (asprintf (&config_path, "%s/ostree/repo/config", sysroot) != -1);
  FILE *f = fopen(config_path, "r");
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

static char*
resolve_deploy_path (const char * root_mountpoint)
{
  char destpath[PATH_MAX];
  struct stat stbuf;
  char *ostree_target, *deploy_path;

  ostree_target = read_proc_cmdline_ostree ();
  if (!ostree_target)
    errx (EXIT_FAILURE, "No OSTree target; expected ostree=/ostree/boot.N/...");

  snprintf (destpath, sizeof(destpath), "%s/%s", root_mountpoint, ostree_target);
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
                   SD_ID128_FORMAT_VAL(OSTREE_PREPARE_ROOT_DEPLOYMENT_MSG),
                   "DEPLOYMENT_PATH=%s", resolved_path,
                   "DEPLOYMENT_DEVICE=%" PRIu64, (uint64_t) stbuf.st_dev,
                   "DEPLOYMENT_INODE=%" PRIu64, (uint64_t) stbuf.st_ino,
                   NULL);
#endif
  return deploy_path;
}

static int
pivot_root(const char * new_root, const char * put_old)
{
  return syscall(__NR_pivot_root, new_root, put_old);
}

int
main(int argc, char *argv[])
{
  /* If we're pid 1, that means there's no initramfs; in this situation
   * various defaults change:
   *
   * - Assume that the target root is /
   * - Quiet logging as there's no journal
   * etc.
   */
  running_as_pid1 = (getpid () == 1);

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

  struct stat stbuf;
  if (stat ("/proc/cmdline", &stbuf) < 0)
    {
      if (errno != ENOENT)
        err (EXIT_FAILURE, "stat(\"/proc/cmdline\") failed");
      /* We need /proc mounted for /proc/cmdline and realpath (on musl) to
       * work: */
      if (mount ("proc", "/proc", "proc", 0, NULL) < 0)
        err (EXIT_FAILURE, "failed to mount proc on /proc");
      we_mounted_proc = 1;
    }

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

  /* Work-around for a kernel bug: for some reason the kernel
   * refuses switching root if any file systems are mounted
   * MS_SHARED. Hence remount them MS_PRIVATE here as a
   * work-around.
   *
   * https://bugzilla.redhat.com/show_bug.cgi?id=847418 */
  if (mount (NULL, "/", NULL, MS_REC|MS_PRIVATE, NULL) < 0)
    err (EXIT_FAILURE, "failed to make \"/\" private mount");

  /* Make deploy_path a bind mount, so we can move it later */
  if (mount (deploy_path, deploy_path, NULL, MS_BIND, NULL) < 0)
    err (EXIT_FAILURE, "failed to make initial bind mount %s", deploy_path);

  /* chdir to our new root.  We need to do this after bind-mounting it over
   * itself otherwise our cwd is still on the non-bind-mounted filesystem
   * below. */
  if (chdir (deploy_path) < 0)
    err (EXIT_FAILURE, "failed to chdir to deploy_path");

  /* Query the repository configuration - this is an operating system builder
   * choice.  More info: https://github.com/ostreedev/ostree/pull/1767
   */
  const bool sysroot_readonly = sysroot_is_configured_ro (root_arg);
  const bool sysroot_currently_writable = !path_is_on_readonly_fs (root_arg);

#ifdef USE_LIBSYSTEMD
      sd_journal_send ("MESSAGE=sysroot configured read-only: %d, currently writable: %d", 
                      (int)sysroot_readonly, (int)sysroot_currently_writable, NULL);
#endif
  if (sysroot_readonly)
    {
      if (!sysroot_currently_writable)
        errx (EXIT_FAILURE, "sysroot=readonly currently requires writable / in initramfs");
      /* Now, /etc is not normally a bind mount, but if we have a readonly
       * sysroot, we still need a writable /etc.  And to avoid race conditions
       * we ensure it's writable in the initramfs, before we switchroot at all.
       */
      if (mount ("etc", "etc", NULL, MS_BIND, NULL) < 0)
        err (EXIT_FAILURE, "failed to make /etc a bind mount");
      /* Pass on the fact that we discovered a readonly sysroot to ostree-remount.service */
      int fd = open (_OSTREE_SYSROOT_READONLY_STAMP, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
      if (fd < 0)
        err (EXIT_FAILURE, "failed to create %s", _OSTREE_SYSROOT_READONLY_STAMP);
      (void) close (fd);
    }

  /* Default to true, but in the systemd case, default to false because it's handled by
   * ostree-system-generator. */
  bool mount_var = true;
#ifdef HAVE_SYSTEMD_AND_LIBMOUNT
  mount_var = false;
#endif

  /* file in /run can override the default behaviour so that we definitely mount /var */
  if (lstat (INITRAMFS_MOUNT_VAR, &stbuf) == 0)
    mount_var = true;

  /* Link to the deployment's /var */
  if (mount_var && mount ("../../var", "var", NULL, MS_BIND, NULL) < 0)
    err (EXIT_FAILURE, "failed to bind mount ../../var to var");

  char srcpath[PATH_MAX];
  /* If /boot is on the same partition, use a bind mount to make it visible
   * at /boot inside the deployment. */
  snprintf (srcpath, sizeof(srcpath), "%s/boot/loader", root_mountpoint);
  if (lstat (srcpath, &stbuf) == 0 && S_ISLNK (stbuf.st_mode))
    {
      if (lstat ("boot", &stbuf) == 0 && S_ISDIR (stbuf.st_mode))
        {
          snprintf (srcpath, sizeof(srcpath), "%s/boot", root_mountpoint);
          if (mount (srcpath, "boot", NULL, MS_BIND, NULL) < 0)
            err (EXIT_FAILURE, "failed to bind mount %s to boot", srcpath);
        }
    }

  /* Do we have a persistent overlayfs for /usr?  If so, mount it now. */
  if (lstat (".usr-ovl-work", &stbuf) == 0)
    {
      const char usr_ovl_options[] = "lowerdir=usr,upperdir=.usr-ovl-upper,workdir=.usr-ovl-work";

      /* Except overlayfs barfs if we try to mount it on a read-only
       * filesystem.  For this use case I think admins are going to be
       * okay if we remount the rootfs here, rather than waiting until
       * later boot and `systemd-remount-fs.service`.
       */
      if (path_is_on_readonly_fs ("."))
        {
          if (mount (".", ".", NULL, MS_REMOUNT | MS_SILENT, NULL) < 0)
            err (EXIT_FAILURE, "failed to remount rootfs writable (for overlayfs)");
        }

      if (mount ("overlay", "usr", "overlay", 0, usr_ovl_options) < 0)
        err (EXIT_FAILURE, "failed to mount /usr overlayfs");
    }
  else
    {
      /* Otherwise, a read-only bind mount for /usr */
      if (mount ("usr", "usr", NULL, MS_BIND, NULL) < 0)
        err (EXIT_FAILURE, "failed to bind mount (class:readonly) /usr");
      if (mount ("usr", "usr", NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0)
        err (EXIT_FAILURE, "failed to bind mount (class:readonly) /usr");
    }


  /* We only stamp /run now if we're running in an initramfs, i.e. we're
   * not pid 1.  Otherwise it's handled later via ostree-system-generator.
   * https://mail.gnome.org/archives/ostree-list/2018-March/msg00012.html
   * https://github.com/ostreedev/ostree/pull/1675
   */
  if (!running_as_pid1)
    touch_run_ostree ();

  if (strcmp(root_mountpoint, "/") == 0)
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
       * /sysroot/ostree/deploy/.../ (deploy_path) and the real rootfs at
       * /sysroot (root_mountpoint).  We want to end up with our made-up root at
       * /sysroot/ and the real rootfs under /sysroot/sysroot as systemd will be
       * responsible for moving /sysroot to /.
       *
       * We need to do this in 3 moves to avoid trying to move /sysroot under
       * itself:
       *
       * 1. /sysroot/ostree/deploy/... -> /sysroot.tmp
       * 2. /sysroot -> /sysroot.tmp/sysroot
       * 3. /sysroot.tmp -> /sysroot
       */
      if (mkdir ("/sysroot.tmp", 0755) < 0)
        err (EXIT_FAILURE, "couldn't create temporary sysroot /sysroot.tmp");

      if (mount (deploy_path, "/sysroot.tmp", NULL, MS_MOVE, NULL) < 0)
        err (EXIT_FAILURE, "failed to MS_MOVE '%s' to '/sysroot.tmp'", deploy_path);

      if (mount (root_mountpoint, "sysroot", NULL, MS_MOVE, NULL) < 0)
        err (EXIT_FAILURE, "failed to MS_MOVE '%s' to 'sysroot'", root_mountpoint);

      if (mount (".", root_mountpoint, NULL, MS_MOVE, NULL) < 0)
        err (EXIT_FAILURE, "failed to MS_MOVE %s to %s", deploy_path, root_mountpoint);

      if (rmdir ("/sysroot.tmp") < 0)
        err (EXIT_FAILURE, "couldn't remove temporary sysroot /sysroot.tmp");
    }

  /* The /sysroot mount needs to be private to avoid having a mount for e.g. /var/cache
   * also propagate to /sysroot/ostree/deploy/$stateroot/var/cache
   *
   * Now in reality, today this is overridden by systemd: the *actual* way we fix this up
   * is in ostree-remount.c.  But let's do it here to express the semantics we want
   * at the very start (perhaps down the line systemd will have compile/runtime option
   * to say that the initramfs environment did everything right from the start).
   */
  if (mount ("none", "sysroot", NULL, MS_PRIVATE, NULL) < 0)
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
