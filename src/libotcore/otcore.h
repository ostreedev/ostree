/*
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

#pragma once

#include "config.h"

#include "otutil.h"
#include <stdbool.h>

#ifdef HAVE_LIBSODIUM
#include <sodium.h>
#define USE_LIBSODIUM
#endif

#if defined(HAVE_OPENSSL)
#include <openssl/evp.h>
#include <openssl/x509.h>
#define USE_OPENSSL
#endif

// Length of a signature in bytes
#define OSTREE_SIGN_ED25519_SIG_SIZE 64U
// Length of a public key in bytes
#define OSTREE_SIGN_ED25519_PUBKEY_SIZE 32U
// This key is stored inside commit metadata.
#define OSTREE_SIGN_METADATA_ED25519_KEY "ostree.sign.ed25519"
// The variant type
#define OSTREE_SIGN_METADATA_ED25519_TYPE "aay"

// This key is stored inside commit metadata.
#define OSTREE_SIGN_METADATA_SPKI_KEY "ostree.sign.spki"
// The variant type
#define OSTREE_SIGN_METADATA_SPKI_TYPE "aay"

// Maximum size of metadata in bytes, in sync with OSTREE_MAX_METADATA_SIZE
#define OSTREE_SIGN_MAX_METADATA_SIZE (128 * 1024 * 1024)

bool otcore_ed25519_init (void);
gboolean otcore_validate_ed25519_signature (GBytes *data, GBytes *pubkey, GBytes *signature,
                                            bool *out_valid, GError **error);

bool otcore_spki_init (void);
gboolean otcore_validate_spki_signature (GBytes *data, GBytes *public_key, GBytes *signature,
                                         bool *out_valid, GError **error);

char *otcore_find_proc_cmdline_key (const char *cmdline, const char *key);
gboolean otcore_get_ostree_target (const char *cmdline, gboolean *is_aboot, char **out_target,
                                   GError **error);

GKeyFile *otcore_load_config (int rootfs, const char *filename, GError **error);

typedef struct
{
  OtTristate composefs_enabled;
  gboolean root_transient;
  gboolean root_transient_ro;
  gboolean require_verity;
  gboolean is_signed;
  char *signature_pubkey;
  GPtrArray *pubkeys;
} RootConfig;
void otcore_free_rootfs_config (RootConfig *config);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (RootConfig, otcore_free_rootfs_config)

RootConfig *otcore_load_rootfs_config (const char *cmdline, GKeyFile *config, gboolean load_keys,
                                       GError **error);

/**
 * otcore_mount_rootfs:
 * @rootfs_config: Configuration for root
 * @metadata_builder: (transfer none): GVariantBuilder to add metadata to.
 * @root_transient: Whether the root filesystem is transient.
 * @root_mountpoint: The mount point of the physical root filesystem.
 * @deploy_path: The path to the deployment.
 * @mount_target: The target path to mount the composefs image.
 * @out_using_composefs: (out): Whether composefs was successfully used.
 * @error: (out): Return location for a GError, or %NULL.
 *
 * If composefs is enabled, it will be mounted at the target. Otherwise, the
 * target directory is left unchanged.
 *
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean otcore_mount_rootfs (RootConfig *rootfs_config, GVariantBuilder *metadata_builder,
                              const char *root_mountpoint, const char *deploy_path,
                              const char *mount_target, bool *out_using_composefs, GError **error);
gboolean otcore_mount_boot (const char *physical_root, const char *deploy_path, GError **error);

gboolean otcore_mount_etc (GKeyFile *config, GVariantBuilder *metadata_builder,
                           const char *mount_target, GError **error);

// Our directory with transient state (eventually /run/ostree-booted should be a link to
// /run/ostree/booted)
#define OTCORE_RUN_OSTREE "/run/ostree"
// This sub-directory is transient state that should not be visible to other processes in general;
// we make it with mode 0 (which requires CAP_DAC_OVERRIDE to pass through).
#define OTCORE_RUN_OSTREE_PRIVATE "/run/ostree/.private"

#define PREPARE_ROOT_CONFIG_PATH "ostree/prepare-root.conf"

// The directory holding extra/backing data for a deployment, such as overlayfs workdirs
#define OSTREE_DEPLOYMENT_BACKING_DIR "backing"
// The directory holding the root overlayfs
#define OSTREE_DEPLOYMENT_ROOT_TRANSIENT_DIR "root-transient"
// The directory holding overlayfs for /usr (ostree admin unlock)
#define OSTREE_DEPLOYMENT_USR_TRANSIENT_DIR "usr-transient"

// Written by ostree admin unlock --hotfix, read by ostree-prepare-root
#define OTCORE_HOTFIX_USR_OVL_WORK ".usr-ovl-work"

// The name of the composefs metadata root
#define OSTREE_COMPOSEFS_NAME ".ostree.cfs"
// The temporary directory used for the EROFS mount; it's in the .private directory
// to help ensure that at least unprivileged code can't transiently see the underlying
// EROFS mount if we somehow leaked it (but it *should* be unmounted always).
#define OSTREE_COMPOSEFS_LOWERMNT OTCORE_RUN_OSTREE_PRIVATE "/cfsroot-lower"

#define OTCORE_PREPARE_ROOT_COMPOSEFS_KEY "composefs"
#define OTCORE_PREPARE_ROOT_ENABLED_KEY "enabled"
#define OTCORE_PREPARE_ROOT_KEYPATH_KEY "keypath"
#define OTCORE_PREPARE_ROOT_TRANSIENT_KEY "transient"
#define OTCORE_PREPARE_ROOT_TRANSIENT_RO_KEY "transient-ro"

// For use with systemd soft reboots
#define OTCORE_RUN_NEXTROOT "/run/nextroot"

// The file written in the initramfs which contains an a{sv} of metadata
// from ostree-prepare-root.
#define OTCORE_RUN_BOOTED "/run/ostree-booted"
// Written by ostree-soft-reboot.c with metadata about /run/nextroot
// that is then processed by ostree-boot-complete.c and turned into
// the canonical /run/ostree-booted.
#define OTCORE_RUN_NEXTROOT_BOOTED "/run/ostree/nextroot-booted"
// This key will be present if composefs was successfully used.
#define OTCORE_RUN_BOOTED_KEY_COMPOSEFS "composefs"
// True if fsverity was required for composefs.
#define OTCORE_RUN_BOOTED_KEY_COMPOSEFS_VERITY "composefs.verity"
// This key if present contains the public key successfully used
// to verify the signature.
#define OTCORE_RUN_BOOTED_KEY_COMPOSEFS_SIGNATURE "composefs.signed"
// This key will be present if the root is transient
#define OTCORE_RUN_BOOTED_KEY_ROOT_TRANSIENT "root.transient"
// This key will be present if the root is transient readonly
#define OTCORE_RUN_BOOTED_KEY_ROOT_TRANSIENT_RO "root.transient-ro"
// This key will be present if the sysroot-ro flag was found
#define OTCORE_RUN_BOOTED_KEY_SYSROOT_RO "sysroot-ro"
// Always holds the (device, inode) pair of the booted deployment
#define OTCORE_RUN_BOOTED_KEY_BACKING_ROOTDEVINO "backing-root-device-inode"

#define OTCORE_RUN_BOOTED_KEY_TRANSIENT_ETC "transient-etc"
