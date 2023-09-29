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
#elif defined(HAVE_OPENSSL)
#include <openssl/evp.h>
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

bool otcore_ed25519_init (void);
gboolean otcore_validate_ed25519_signature (GBytes *data, GBytes *pubkey, GBytes *signature,
                                            bool *out_valid, GError **error);

char *otcore_find_proc_cmdline_key (const char *cmdline, const char *key);
gboolean otcore_get_ostree_target (const char *cmdline, char **out_target, GError **error);

GKeyFile *otcore_load_config (int rootfs, const char *filename, GError **error);

// Our directory with transient state (eventually /run/ostree-booted should be a link to
// /run/ostree/booted)
#define OTCORE_RUN_OSTREE "/run/ostree"
// This sub-directory is transient state that should not be visible to other processes in general;
// we make it with mode 0 (which requires CAP_DAC_OVERRIDE to pass through).
#define OTCORE_RUN_OSTREE_PRIVATE "/run/ostree/.private"

// The name of the composefs metadata root
#define OSTREE_COMPOSEFS_NAME ".ostree.cfs"
// The temporary directory used for the EROFS mount; it's in the .private directory
// to help ensure that at least unprivileged code can't transiently see the underlying
// EROFS mount if we somehow leaked it (but it *should* be unmounted always).
#define OSTREE_COMPOSEFS_LOWERMNT OTCORE_RUN_OSTREE_PRIVATE "/cfsroot-lower"

// The file written in the initramfs which contains an a{sv} of metadata
// from ostree-prepare-root.
#define OTCORE_RUN_BOOTED "/run/ostree-booted"
// This key will be present if composefs was successfully used.
#define OTCORE_RUN_BOOTED_KEY_COMPOSEFS "composefs"
// This key if present contains the public key successfully used
// to verify the signature.
#define OTCORE_RUN_BOOTED_KEY_COMPOSEFS_SIGNATURE "composefs.signed"
// This key will be present if the sysroot-ro flag was found
#define OTCORE_RUN_BOOTED_KEY_SYSROOT_RO "sysroot-ro"

#define OTCORE_RUN_BOOTED_KEY_TRANSIENT_ETC "transient-etc"
