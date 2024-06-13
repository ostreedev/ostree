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
 * See ostree-prepare-root-static.c for this.
 */

#include "config.h"

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libglnx.h>
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

#include <ostree-core.h>
#include <ostree-repo-private.h>

#include "ot-keyfile-utils.h"
#include "otcore.h"

#define SYSROOT_KEY "sysroot"
#define READONLY_KEY "readonly"

/* This key configures the / mount in the deployment root */
#define ROOT_KEY "root"
#define ETC_KEY "etc"
#define TRANSIENT_KEY "transient"

#define OSTREE_PREPARE_ROOT_DEPLOYMENT_MSG \
  SD_ID128_MAKE (71, 70, 33, 6a, 73, ba, 46, 01, ba, d3, 1a, f8, 88, aa, 0d, f7)

// A temporary mount point
#define TMP_SYSROOT "/sysroot.tmp"

#ifdef HAVE_COMPOSEFS
#include <libcomposefs/lcfs-mount.h>
#include <libcomposefs/lcfs-writer.h>
#endif

#include "ostree-mount-util.h"

static bool
sysroot_is_configured_ro (const char *sysroot)
{
  g_autoptr (GError) local_error = NULL;
  g_autofree char *repo_config_path = g_build_filename (sysroot, "ostree/repo/config", NULL);
  g_autoptr (GKeyFile) repo_config = g_key_file_new ();
  if (!g_key_file_load_from_file (repo_config, repo_config_path, G_KEY_FILE_NONE, &local_error))
    {
      g_printerr ("Failed to load %s: %s", repo_config_path, local_error->message);
      return false;
    }

  return g_key_file_get_boolean (repo_config, SYSROOT_KEY, READONLY_KEY, NULL);
}

static char *
resolve_deploy_path (const char *root_mountpoint)
{
  char destpath[PATH_MAX];
  struct stat stbuf;
  char *deploy_path;
  g_autofree char *kernel_cmdline = read_proc_cmdline ();
  if (!kernel_cmdline)
    errx (EXIT_FAILURE, "Failed to read kernel cmdline");

  g_autoptr (GError) error = NULL;
  g_autofree char *ostree_target = NULL;
  if (!otcore_get_ostree_target (kernel_cmdline, NULL, &ostree_target, &error))
    errx (EXIT_FAILURE, "Failed to determine ostree target: %s", error->message);
  if (!ostree_target)
    errx (EXIT_FAILURE, "No ostree target found");

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
  const char *resolved_path = deploy_path + strlen (root_mountpoint);
  ot_journal_send ("MESSAGE=Resolved OSTree target to: %s", deploy_path,
                   "MESSAGE_ID=" SD_ID128_FORMAT_STR,
                   SD_ID128_FORMAT_VAL (OSTREE_PREPARE_ROOT_DEPLOYMENT_MSG), "DEPLOYMENT_PATH=%s",
                   resolved_path, "DEPLOYMENT_DEVICE=%" PRIu64, (uint64_t)stbuf.st_dev,
                   "DEPLOYMENT_INODE=%" PRIu64, (uint64_t)stbuf.st_ino, NULL);
  return deploy_path;
}

#ifdef HAVE_COMPOSEFS
static GVariant *
load_variant (const char *root_mountpoint, const char *digest, const char *extension,
              const GVariantType *type, GError **error)
{
  g_autofree char *path = g_strdup_printf ("%s/ostree/repo/objects/%.2s/%s.%s", root_mountpoint,
                                           digest, digest + 2, extension);

  char *data = NULL;
  gsize data_size;
  if (!g_file_get_contents (path, &data, &data_size, error))
    return NULL;

  return g_variant_ref_sink (g_variant_new_from_data (type, data, data_size, FALSE, g_free, data));
}

// Given a mount point, directly load the .commit object.  At the current time this tool
// doesn't link to libostree.
static gboolean
load_commit_for_deploy (const char *root_mountpoint, const char *deploy_path, GVariant **commit_out,
                        GVariant **commitmeta_out, GError **error)
{
  g_autoptr (GError) local_error = NULL;
  g_autofree char *digest = g_path_get_basename (deploy_path);
  char *dot = strchr (digest, '.');
  if (dot != NULL)
    *dot = 0;

  g_autoptr (GVariant) commit_v
      = load_variant (root_mountpoint, digest, "commit", OSTREE_COMMIT_GVARIANT_FORMAT, error);
  if (commit_v == NULL)
    return FALSE;

  g_autoptr (GVariant) commitmeta_v = load_variant (root_mountpoint, digest, "commitmeta",
                                                    G_VARIANT_TYPE ("a{sv}"), &local_error);
  if (commitmeta_v == NULL)
    {
      if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        glnx_throw (error, "No commitmeta for commit %s", digest);
      else
        g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  *commit_out = g_steal_pointer (&commit_v);
  *commitmeta_out = g_steal_pointer (&commitmeta_v);

  return TRUE;
}

/**
 * validate_signature:
 * @data: The raw data whose signature must be validated
 * @signatures: A variant of type "ay" (byte array) containing signatures
 * @pubkeys: an array of type GBytes*
 *
 * Verify that @data is signed using @signatures and @pubkeys.
 */
static gboolean
validate_signature (GBytes *data, GVariant *signatures, GPtrArray *pubkeys)
{
  g_assert (data);
  g_assert (signatures);
  g_assert (pubkeys);

  for (gsize j = 0; j < pubkeys->len; j++)
    {
      GBytes *pubkey = pubkeys->pdata[j];
      g_assert (pubkey);

      for (gsize i = 0; i < g_variant_n_children (signatures); i++)
        {
          g_autoptr (GError) local_error = NULL;
          g_autoptr (GVariant) child = g_variant_get_child_value (signatures, i);
          g_autoptr (GBytes) signature = g_variant_get_data_as_bytes (child);
          bool valid = false;

          if (!otcore_validate_ed25519_signature (data, pubkey, signature, &valid, &local_error))
            errx (EXIT_FAILURE, "signature verification failed: %s", local_error->message);
          // At least one valid signature is enough.
          if (valid)
            return TRUE;
        }
    }

  return FALSE;
}

// Output a friendly message based on an errno for common cases
static const char *
composefs_error_message (int errsv)
{
  switch (errsv)
    {
    case ENOVERITY:
      return "fsverity not enabled on composefs image";
    case EWRONGVERITY:
      return "Wrong fsverity digest in composefs image";
    case ENOSIGNATURE:
      return "Missing signature for fsverity in composefs image";
    default:
      return strerror (errsv);
    }
}

#endif

int
main (int argc, char *argv[])
{
  char srcpath[PATH_MAX];
  struct stat stbuf;
  g_autoptr (GError) error = NULL;

  if (argc < 2)
    err (EXIT_FAILURE, "usage: ostree-prepare-root SYSROOT");
  const char *root_arg = argv[1];

  // Since several APIs want to operate in terms of file descriptors, let's
  // open the initramfs now.  Currently this is just used for the config parser.
  glnx_autofd int initramfs_rootfs_fd = -1;
  if (!glnx_opendirat (AT_FDCWD, "/", FALSE, &initramfs_rootfs_fd, &error))
    errx (EXIT_FAILURE, "Failed to open /: %s", error->message);

  g_autoptr (GKeyFile) config
      = otcore_load_config (initramfs_rootfs_fd, PREPARE_ROOT_CONFIG_PATH, &error);
  if (!config)
    errx (EXIT_FAILURE, "Failed to parse config: %s", error->message);

  gboolean sysroot_readonly = FALSE;
  gboolean root_transient = FALSE;

  if (!ot_keyfile_get_boolean_with_default (config, ROOT_KEY, TRANSIENT_KEY, FALSE, &root_transient,
                                            &error))
    return FALSE;

  // We always parse the composefs config, because we want to detect and error
  // out if it's enabled, but not supported at compile time.
  g_autoptr (ComposefsConfig) composefs_config
      = otcore_load_composefs_config (config, TRUE, &error);
  if (!composefs_config)
    errx (EXIT_FAILURE, "%s", error->message);

  // If composefs is enabled, that also implies sysroot.readonly=true because it's
  // the new default we want to use (not because it's actually required)
  const bool sysroot_readonly_default = composefs_config->enabled == OT_TRISTATE_YES;
  if (!ot_keyfile_get_boolean_with_default (config, SYSROOT_KEY, READONLY_KEY,
                                            sysroot_readonly_default, &sysroot_readonly, &error))
    errx (EXIT_FAILURE, "Failed to parse sysroot.readonly value: %s", error->message);

  /* This is the final target where we should prepare the rootfs.  The usual
   * case with systemd in the initramfs is that root_mountpoint = "/sysroot".
   * In the fastboot embedded case we're pid1 and will setup / ourself, and
   * then root_mountpoint = "/".
   * */
  const char *root_mountpoint = realpath (root_arg, NULL);
  if (root_mountpoint == NULL)
    err (EXIT_FAILURE, "realpath(\"%s\")", root_arg);
  g_autofree char *deploy_path = resolve_deploy_path (root_mountpoint);
  const char *deploy_directory_name = glnx_basename (deploy_path);
  // Note that realpath() should have stripped any trailing `/` which shouldn't
  // be in the karg to start with, but we assert here to be sure we have a non-empty
  // filename.
  g_assert (deploy_directory_name && *deploy_directory_name);

  /* These are global state directories underneath /run */
  if (mkdirat (AT_FDCWD, OTCORE_RUN_OSTREE, 0755) < 0)
    err (EXIT_FAILURE, "Failed to create %s", OTCORE_RUN_OSTREE);
  if (mkdirat (AT_FDCWD, OTCORE_RUN_OSTREE_PRIVATE, 0) < 0)
    err (EXIT_FAILURE, "Failed to create %s", OTCORE_RUN_OSTREE_PRIVATE);

  /* Fall back to querying the repository configuration in the target disk.
   * This is an operating system builder choice.  More info:
   * https://github.com/ostreedev/ostree/pull/1767
   * However, we only do this if composefs is not enabled, because we don't
   * want to parse the target root filesystem before verifying its integrity.
   */
  if (!sysroot_readonly && composefs_config->enabled != OT_TRISTATE_YES)
    {
      sysroot_readonly = sysroot_is_configured_ro (root_arg);
      // Encourage porting to the new config file
      if (sysroot_readonly)
        g_print ("Found legacy sysroot.readonly flag, not configured in %s\n",
                 PREPARE_ROOT_CONFIG_PATH);
    }
  const bool sysroot_currently_writable = !path_is_on_readonly_fs (root_arg);
  g_print ("sysroot.readonly configuration value: %d (fs writable: %d)\n", (int)sysroot_readonly,
           (int)sysroot_currently_writable);

  /* Remount root MS_PRIVATE here to avoid errors due to the kernel-enforced
   * constraint that disallows MS_SHARED mounts to be moved.
   *
   * Kernel docs: Documentation/filesystems/sharedsubtree.txt
   */
  if (mount (NULL, "/", NULL, MS_REC | MS_PRIVATE | MS_SILENT, NULL) < 0)
    err (EXIT_FAILURE, "failed to make \"/\" private mount");

  if (mkdir (TMP_SYSROOT, 0755) < 0)
    err (EXIT_FAILURE, "couldn't create temporary sysroot %s", TMP_SYSROOT);

  /* Run in the deploy_path dir so we can use relative paths below */
  if (chdir (deploy_path) < 0)
    err (EXIT_FAILURE, "failed to chdir to deploy_path");

  GVariantBuilder metadata_builder;
  g_variant_builder_init (&metadata_builder, G_VARIANT_TYPE ("a{sv}"));

  /* Record the underlying plain deployment directory (device,inode) pair
   * so that it can be later checked by the sysroot code to figure out
   * which deployment was booted.
   */
  if (lstat (".", &stbuf) < 0)
    err (EXIT_FAILURE, "lstat deploy_root");
  g_variant_builder_add (&metadata_builder, "{sv}", OTCORE_RUN_BOOTED_KEY_BACKING_ROOTDEVINO,
                         g_variant_new ("(tt)", (guint64)stbuf.st_dev, (guint64)stbuf.st_ino));

  // Tracks if we did successfully enable it at runtime
  bool using_composefs = false;

#ifdef HAVE_COMPOSEFS
  /* We construct the new sysroot in /sysroot.tmp, which is either the composefs
     mount or a bind mount of the deploy-dir */
  if (composefs_config->enabled != OT_TRISTATE_NO)
    {
      const char *objdirs[] = { "/sysroot/ostree/repo/objects" };
      g_autofree char *cfs_digest = NULL;
      struct lcfs_mount_options_s cfs_options = {
        objdirs,
        1,
      };

      cfs_options.flags = 0;
      cfs_options.image_mountdir = OSTREE_COMPOSEFS_LOWERMNT;
      if (mkdirat (AT_FDCWD, OSTREE_COMPOSEFS_LOWERMNT, 0700) < 0)
        err (EXIT_FAILURE, "Failed to create %s", OSTREE_COMPOSEFS_LOWERMNT);

      g_autofree char *expected_digest = NULL;

      // For now we just stick the transient root on the default /run tmpfs;
      // however, see
      // https://github.com/systemd/systemd/blob/604b2001081adcbd64ee1fbe7de7a6d77c5209fe/src/basic/mountpoint-util.h#L36
      // which bumps up these defaults for the rootfs a bit.
      g_autofree char *root_upperdir
          = root_transient ? g_build_filename (OTCORE_RUN_OSTREE_PRIVATE, "root/upper", NULL)
                           : NULL;
      g_autofree char *root_workdir
          = root_transient ? g_build_filename (OTCORE_RUN_OSTREE_PRIVATE, "root/work", NULL) : NULL;

      // Propagate these options for transient root, if provided
      if (root_transient)
        {
          if (!glnx_shutil_mkdir_p_at (AT_FDCWD, root_upperdir, 0755, NULL, &error))
            errx (EXIT_FAILURE, "Failed to create %s: %s", root_upperdir, error->message);
          if (!glnx_shutil_mkdir_p_at (AT_FDCWD, root_workdir, 0700, NULL, &error))
            errx (EXIT_FAILURE, "Failed to create %s: %s", root_workdir, error->message);

          cfs_options.workdir = root_workdir;
          cfs_options.upperdir = root_upperdir;
        }
      else
        {
          cfs_options.flags = LCFS_MOUNT_FLAGS_READONLY;
        }

      if (composefs_config->is_signed)
        {
          const char *composefs_pubkey = composefs_config->signature_pubkey;
          g_autoptr (GError) local_error = NULL;
          g_autoptr (GVariant) commit = NULL;
          g_autoptr (GVariant) commitmeta = NULL;

          if (!load_commit_for_deploy (root_mountpoint, deploy_path, &commit, &commitmeta,
                                       &local_error))
            errx (EXIT_FAILURE, "Error loading signatures from repo: %s", local_error->message);

          g_autoptr (GVariant) signatures = g_variant_lookup_value (
              commitmeta, OSTREE_SIGN_METADATA_ED25519_KEY, G_VARIANT_TYPE ("aay"));
          if (signatures == NULL)
            errx (EXIT_FAILURE, "Signature validation requested, but no signatures in commit");

          g_autoptr (GBytes) commit_data = g_variant_get_data_as_bytes (commit);
          if (!validate_signature (commit_data, signatures, composefs_config->pubkeys))
            errx (EXIT_FAILURE, "No valid signatures found for public key");

          g_print ("composefs+ostree: Validated commit signature using '%s'\n", composefs_pubkey);
          g_variant_builder_add (&metadata_builder, "{sv}",
                                 OTCORE_RUN_BOOTED_KEY_COMPOSEFS_SIGNATURE,
                                 g_variant_new_string (composefs_pubkey));

          g_autoptr (GVariant) metadata = g_variant_get_child_value (commit, 0);
          g_autoptr (GVariant) cfs_digest_v = g_variant_lookup_value (
              metadata, OSTREE_COMPOSEFS_DIGEST_KEY_V0, G_VARIANT_TYPE_BYTESTRING);
          if (cfs_digest_v == NULL || g_variant_get_size (cfs_digest_v) != OSTREE_SHA256_DIGEST_LEN)
            errx (EXIT_FAILURE, "Signature validation requested, but no valid digest in commit");
          const guint8 *cfs_digest_buf = ot_variant_get_data (cfs_digest_v, &error);
          if (!cfs_digest_buf)
            errx (EXIT_FAILURE, "Failed to query digest: %s", error->message);

          expected_digest = g_malloc (OSTREE_SHA256_STRING_LEN + 1);
          ot_bin2hex (expected_digest, cfs_digest_buf, g_variant_get_size (cfs_digest_v));

          cfs_options.flags |= LCFS_MOUNT_FLAGS_REQUIRE_VERITY;
          g_print ("composefs: Verifying digest: %s\n", expected_digest);
          cfs_options.expected_fsverity_digest = expected_digest;
        }

      if (lcfs_mount_image (OSTREE_COMPOSEFS_NAME, TMP_SYSROOT, &cfs_options) == 0)
        {
          using_composefs = true;
          g_variant_builder_add (&metadata_builder, "{sv}", OTCORE_RUN_BOOTED_KEY_COMPOSEFS,
                                 g_variant_new_boolean (true));
          g_print ("composefs: mounted successfully\n");
        }
      else
        {
          int errsv = errno;
          g_assert (composefs_config->enabled != OT_TRISTATE_NO);
          if (composefs_config->enabled == OT_TRISTATE_MAYBE && errsv == ENOENT)
            {
              g_print ("composefs: No image present\n");
            }
          else
            {
              const char *errmsg = composefs_error_message (errsv);
              errx (EXIT_FAILURE, "composefs: failed to mount: %s", errmsg);
            }
        }
    }
#else
  /* if composefs is configured as "maybe", we should continue */
  if (composefs_config->enabled == OT_TRISTATE_YES)
    errx (EXIT_FAILURE, "composefs: enabled at runtime, but support is not compiled in");
#endif

  if (!using_composefs)
    {
      if (root_transient)
        {
          errx (EXIT_FAILURE, "Must enable composefs with root.transient");
        }
      g_print ("Using legacy ostree bind mount for /\n");
      /* The deploy root starts out bind mounted to sysroot.tmp */
      if (mount (deploy_path, TMP_SYSROOT, NULL, MS_BIND | MS_SILENT, NULL) < 0)
        err (EXIT_FAILURE, "failed to make initial bind mount %s", deploy_path);
    }

  /* Pass on the state  */
  g_variant_builder_add (&metadata_builder, "{sv}", OTCORE_RUN_BOOTED_KEY_ROOT_TRANSIENT,
                         g_variant_new_boolean (root_transient));

  /* This will result in a system with /sysroot read-only. Thus, two additional
   * writable bind-mounts (for /etc and /var) are required later on. */
  if (sysroot_readonly)
    {
      if (!sysroot_currently_writable)
        errx (EXIT_FAILURE, OTCORE_SYSROOT_NOT_WRITEABLE, root_arg);
    }
  /* Pass on the state for use by ostree-prepare-root */
  g_variant_builder_add (&metadata_builder, "{sv}", OTCORE_RUN_BOOTED_KEY_SYSROOT_RO,
                         g_variant_new_boolean (sysroot_readonly));

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
  if (sysroot_readonly || using_composefs || root_transient)
    {
      gboolean etc_transient = FALSE;
      if (!ot_keyfile_get_boolean_with_default (config, ETC_KEY, TRANSIENT_KEY, FALSE,
                                                &etc_transient, &error))
        errx (EXIT_FAILURE, "Failed to parse etc.transient value: %s", error->message);

      static const char *tmp_sysroot_etc = TMP_SYSROOT "/etc";
      if (etc_transient)
        {
          char *ovldir = "/run/ostree/transient-etc";

          g_variant_builder_add (&metadata_builder, "{sv}", OTCORE_RUN_BOOTED_KEY_TRANSIENT_ETC,
                                 g_variant_new_string (ovldir));

          char *lowerdir = "usr/etc";
          if (using_composefs)
            lowerdir = TMP_SYSROOT "/usr/etc";

          g_autofree char *upperdir = g_build_filename (ovldir, "upper", NULL);
          g_autofree char *workdir = g_build_filename (ovldir, "work", NULL);

          struct
          {
            const char *path;
            int mode;
          } subdirs[] = { { ovldir, 0700 }, { upperdir, 0755 }, { workdir, 0755 } };
          for (int i = 0; i < G_N_ELEMENTS (subdirs); i++)
            {
              if (mkdirat (AT_FDCWD, subdirs[i].path, subdirs[i].mode) < 0)
                err (EXIT_FAILURE, "Failed to create dir %s", subdirs[i].path);
            }

          g_autofree char *ovl_options
              = g_strdup_printf ("lowerdir=%s,upperdir=%s,workdir=%s", lowerdir, upperdir, workdir);
          if (mount ("overlay", tmp_sysroot_etc, "overlay", MS_SILENT, ovl_options) < 0)
            err (EXIT_FAILURE, "failed to mount transient etc overlayfs");
        }
      else
        {
          /* Bind-mount /etc (at deploy path), and remount as writable. */
          if (mount ("etc", tmp_sysroot_etc, NULL, MS_BIND | MS_SILENT, NULL) < 0)
            err (EXIT_FAILURE, "failed to prepare /etc bind-mount at /sysroot.tmp/etc");
          if (mount (tmp_sysroot_etc, tmp_sysroot_etc, NULL, MS_BIND | MS_REMOUNT | MS_SILENT,
                     NULL)
              < 0)
            err (EXIT_FAILURE, "failed to make writable /etc bind-mount at /sysroot.tmp/etc");
        }
    }

  /* Prepare /usr.
   * It may be either just a read-only bind-mount, or a persistent overlayfs if set up
   * with ostree admin unlock --hotfix.
   * Note however that root.transient as handled above is effectively a generalization of unlock
   * --hotfix.
   * Also, hotfixes are incompatible with signed composefs use for security reasons.
   */
  if (lstat (OTCORE_HOTFIX_USR_OVL_WORK, &stbuf) == 0
      && !(using_composefs && composefs_config->is_signed))
    {
      /* Do we have a persistent overlayfs for /usr?  If so, mount it now. */
      const char usr_ovl_options[]
          = "lowerdir=" TMP_SYSROOT
            "/usr,upperdir=.usr-ovl-upper,workdir=" OTCORE_HOTFIX_USR_OVL_WORK;

      unsigned long mflags = MS_SILENT;
      // Propagate readonly state
      if (!sysroot_currently_writable)
        mflags |= MS_RDONLY;
      if (mount ("overlay", TMP_SYSROOT "/usr", "overlay", mflags, usr_ovl_options) < 0)
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

  /* This can be used by other things to signal ostree is in use */
  {
    g_autoptr (GVariant) metadata = g_variant_ref_sink (g_variant_builder_end (&metadata_builder));
    const guint8 *buf = g_variant_get_data (metadata) ?: (guint8 *)"";
    if (!glnx_file_replace_contents_at (AT_FDCWD, OTCORE_RUN_BOOTED, buf,
                                        g_variant_get_size (metadata), 0, NULL, &error))
      errx (EXIT_FAILURE, "Writing %s: %s", OTCORE_RUN_BOOTED, error->message);
  }

  if (chdir (TMP_SYSROOT) < 0)
    err (EXIT_FAILURE, "failed to chdir to " TMP_SYSROOT);

  /* Now we have our ready made-up up root at
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

  /* Now that we've set up all the mount points, if configured we remount the physical
   * rootfs as read-only; what is visibly mutable to the OS by default is just /etc and /var.
   * But ostree knows how to mount /boot and /sysroot read-write to perform operations.
   */
  if (sysroot_readonly)
    {
      if (mount ("sysroot", "sysroot", NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_SILENT, NULL)
          < 0)
        err (EXIT_FAILURE, "failed to make /sysroot read-only");
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

  exit (EXIT_SUCCESS);
}
