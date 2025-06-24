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

#include "config.h"

#include "otcore.h"
#include <errno.h>
#include <ostree-core.h>
#include <ostree-repo-private.h>

#ifdef HAVE_COMPOSEFS
#include <libcomposefs/lcfs-mount.h>
#include <libcomposefs/lcfs-writer.h>
#endif

// This key is used by default if present in the initramfs to verify
// the signature on the target commit object.  When composefs is
// in use, the ostree commit metadata will contain the composefs image digest,
// which can be used to fully verify the target filesystem tree.
#define BINDING_KEYPATH "/etc/ostree/initramfs-root-binding.key"
// The kernel argument to configure composefs
#define CMDLINE_KEY_COMPOSEFS "ostree.prepare-root.composefs"

static bool
proc_cmdline_has_key_starting_with (const char *cmdline, const char *key)
{
  for (const char *iter = cmdline; iter;)
    {
      if (g_str_has_prefix (iter, key))
        return true;

      iter = strchr (iter, ' ');
      if (iter == NULL)
        return false;

      iter += strspn (iter, " ");
    }

  return false;
}

// Parse a kernel cmdline to find the provided key.
// TODO: Deduplicate this with the kernel argument code from libostree.so
char *
otcore_find_proc_cmdline_key (const char *cmdline, const char *key)
{
  const size_t key_len = strlen (key);
  for (const char *iter = cmdline; iter;)
    {
      const char *next = strchr (iter, ' ');
      if (strncmp (iter, key, key_len) == 0 && iter[key_len] == '=')
        {
          const char *start = iter + key_len + 1;
          if (next)
            return strndup (start, next - start);

          return strdup (start);
        }

      if (next)
        next += strspn (next, " ");

      iter = next;
    }

  return NULL;
}

// Find the target OSTree root filesystem from parsing the provided kernel commandline.
// If none is found, @out_target will be set to NULL, and the function will return successfully.
//
// If invalid data is found, @error will be set.
gboolean
otcore_get_ostree_target (const char *cmdline, gboolean *is_aboot, char **out_target,
                          GError **error)
{
  g_assert (cmdline);
  g_assert (out_target && *out_target == NULL);
  static const char slot_a[] = "/ostree/root.a";
  static const char slot_b[] = "/ostree/root.b";

  // First, handle the Android boot case
  g_autofree char *slot_suffix = otcore_find_proc_cmdline_key (cmdline, "androidboot.slot_suffix");
  if (is_aboot)
    *is_aboot = false;

  if (slot_suffix)
    {
      if (is_aboot)
        *is_aboot = true;

      if (strcmp (slot_suffix, "_a") == 0)
        {
          *out_target = g_strdup (slot_a);
          return TRUE;
        }
      else if (strcmp (slot_suffix, "_b") == 0)
        {
          *out_target = g_strdup (slot_b);
          return TRUE;
        }
      return glnx_throw (error, "androidboot.slot_suffix invalid: %s", slot_suffix);
    }

  /* Non-A/B androidboot:
   * https://source.android.com/docs/core/ota/nonab
   */
  if (proc_cmdline_has_key_starting_with (cmdline, "androidboot."))
    {
      if (is_aboot)
        *is_aboot = true;
      *out_target = g_strdup (slot_a);
      return TRUE;
    }

  // Otherwise, fall back to the default `ostree=` kernel cmdline
  *out_target = otcore_find_proc_cmdline_key (cmdline, "ostree");
  return TRUE;
}

// Load a config file; if it doesn't exist, we return an empty configuration.
// NULL will be returned if we caught an error.
GKeyFile *
otcore_load_config (int rootfs_fd, const char *filename, GError **error)
{
  // The path to the config file for this binary
  static const char *const config_roots[] = { "usr/lib", "etc" };
  g_autoptr (GKeyFile) ret = g_key_file_new ();

  for (guint i = 0; i < G_N_ELEMENTS (config_roots); i++)
    {
      glnx_autofd int fd = -1;
      g_autofree char *path = g_build_filename (config_roots[i], filename, NULL);
      if (!ot_openat_ignore_enoent (rootfs_fd, path, &fd, error))
        return NULL;
      /* If the config file doesn't exist, that's OK */
      if (fd == -1)
        continue;

      g_autofree char *buf = glnx_fd_readall_utf8 (fd, NULL, NULL, error);
      if (!buf)
        return NULL;
      if (!g_key_file_load_from_data (ret, buf, -1, 0, error))
        return NULL;
    }

  return g_steal_pointer (&ret);
}

void
otcore_free_composefs_config (ComposefsConfig *config)
{
  g_clear_pointer (&config->pubkeys, g_ptr_array_unref);
  g_free (config->signature_pubkey);
  g_free (config);
}

// Parse the [composefs] section of the prepare-root.conf.
ComposefsConfig *
otcore_load_composefs_config (const char *cmdline, GKeyFile *config, gboolean load_keys,
                              GError **error)
{
  g_assert (cmdline);
  g_assert (config);

  GLNX_AUTO_PREFIX_ERROR ("Loading composefs config", error);

  g_autoptr (ComposefsConfig) ret = g_new0 (ComposefsConfig, 1);

  g_autofree char *enabled = g_key_file_get_value (config, OTCORE_PREPARE_ROOT_COMPOSEFS_KEY,
                                                   OTCORE_PREPARE_ROOT_ENABLED_KEY, NULL);
  if (g_strcmp0 (enabled, "signed") == 0)
    {
      ret->enabled = OT_TRISTATE_YES;
      ret->require_verity = true;
      ret->is_signed = true;
    }
  else if (g_strcmp0 (enabled, "verity") == 0)
    {
      ret->enabled = OT_TRISTATE_YES;
      ret->require_verity = true;
      ret->is_signed = false;
    }
  else if (!ot_keyfile_get_tristate_with_default (config, OTCORE_PREPARE_ROOT_COMPOSEFS_KEY,
                                                  OTCORE_PREPARE_ROOT_ENABLED_KEY, OT_TRISTATE_NO,
                                                  &ret->enabled, error))
    return NULL;

  // Look for a key - we default to the initramfs binding path.
  if (!ot_keyfile_get_value_with_default (config, OTCORE_PREPARE_ROOT_COMPOSEFS_KEY,
                                          OTCORE_PREPARE_ROOT_KEYPATH_KEY, BINDING_KEYPATH,
                                          &ret->signature_pubkey, error))
    return NULL;

  if (ret->is_signed && load_keys)
    {
      ret->pubkeys = g_ptr_array_new_with_free_func ((GDestroyNotify)g_bytes_unref);

      g_autofree char *pubkeys = NULL;
      gsize pubkeys_size;

      /* Load keys */

      if (!g_file_get_contents (ret->signature_pubkey, &pubkeys, &pubkeys_size, error))
        return glnx_prefix_error_null (error, "Reading public key file '%s'",
                                       ret->signature_pubkey);

      g_auto (GStrv) lines = g_strsplit (pubkeys, "\n", -1);
      for (char **iter = lines; *iter; iter++)
        {
          const char *line = *iter;
          if (!*line)
            continue;

          gsize pubkey_size;
          g_autofree guchar *pubkey = g_base64_decode (line, &pubkey_size);
          g_ptr_array_add (ret->pubkeys, g_bytes_new_take (g_steal_pointer (&pubkey), pubkey_size));
        }

      if (ret->pubkeys->len == 0)
        return glnx_null_throw (error, "public key file specified, but no public keys found");
    }

  g_autofree char *ostree_composefs = otcore_find_proc_cmdline_key (cmdline, CMDLINE_KEY_COMPOSEFS);
  if (ostree_composefs)
    {
      if (g_strcmp0 (ostree_composefs, "signed") == 0)
        {
          ret->enabled = OT_TRISTATE_YES;
          ret->is_signed = true;
          ret->require_verity = true;
        }
      else
        {
          // The other states force off signatures
          ret->is_signed = false;
          if (!_ostree_parse_tristate (ostree_composefs, &ret->enabled, error))
            return glnx_prefix_error (error, "handling karg " CMDLINE_KEY_COMPOSEFS), NULL;
        }
    }

  return g_steal_pointer (&ret);
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
validate_signature (GBytes *data, GVariant *signatures, GPtrArray *pubkeys, GError **error)
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
          g_autoptr (GVariant) child = g_variant_get_child_value (signatures, i);
          g_autoptr (GBytes) signature = g_variant_get_data_as_bytes (child);
          bool valid = false;

          if (!otcore_validate_ed25519_signature (data, pubkey, signature, &valid, error))
            return glnx_prefix_error (error, "signature verification failed");
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

gboolean
otcore_mount_composefs (ComposefsConfig *composefs_config, GVariantBuilder *metadata_builder,
                        gboolean root_transient, const char *root_mountpoint,
                        const char *deploy_path, const char *mount_target,
                        bool *out_using_composefs, GError **error)
{
  bool using_composefs = FALSE;
#ifdef HAVE_COMPOSEFS
  /* We construct the new sysroot in /sysroot.tmp, which is either the composefs
     mount or a bind mount of the deploy-dir */
  if (composefs_config->enabled == OT_TRISTATE_NO)
    return TRUE;

  g_autofree char *sysroot_objects = g_strdup_printf ("%s/ostree/repo/objects", root_mountpoint);
  const char *objdirs[] = { sysroot_objects };
  struct lcfs_mount_options_s cfs_options = {
    objdirs,
    1,
  };

  cfs_options.flags = 0;
  cfs_options.image_mountdir = OSTREE_COMPOSEFS_LOWERMNT;
  if (mkdirat (AT_FDCWD, OSTREE_COMPOSEFS_LOWERMNT, 0700) < 0 && errno != EEXIST)
    return glnx_throw (error, "Failed to create %s", OSTREE_COMPOSEFS_LOWERMNT);

  g_autofree char *expected_digest = NULL;

  // For now we just stick the transient root on the default /run tmpfs;
  // however, see
  // https://github.com/systemd/systemd/blob/604b2001081adcbd64ee1fbe7de7a6d77c5209fe/src/basic/mountpoint-util.h#L36
  // which bumps up these defaults for the rootfs a bit.
  g_autofree char *root_upperdir
      = root_transient ? g_build_filename (OTCORE_RUN_OSTREE_PRIVATE, "root/upper", NULL) : NULL;
  g_autofree char *root_workdir
      = root_transient ? g_build_filename (OTCORE_RUN_OSTREE_PRIVATE, "root/work", NULL) : NULL;

  // Propagate these options for transient root, if provided
  if (root_transient)
    {
      if (!glnx_shutil_mkdir_p_at (AT_FDCWD, root_upperdir, 0755, NULL, error))
        return glnx_prefix_error (error, "Failed to create %s", root_upperdir);
      if (!glnx_shutil_mkdir_p_at (AT_FDCWD, root_workdir, 0700, NULL, error))
        return glnx_prefix_error (error, "Failed to create %s", root_workdir);

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
      g_autoptr (GVariant) commit = NULL;
      g_autoptr (GVariant) commitmeta = NULL;

      if (!load_commit_for_deploy (root_mountpoint, deploy_path, &commit, &commitmeta, error))
        return glnx_prefix_error (error, "Error loading signatures from repo");

      g_autoptr (GVariant) signatures = g_variant_lookup_value (
          commitmeta, OSTREE_SIGN_METADATA_ED25519_KEY, G_VARIANT_TYPE ("aay"));
      if (signatures == NULL)
        return glnx_throw (error, "Signature validation requested, but no signatures in commit");

      g_autoptr (GBytes) commit_data = g_variant_get_data_as_bytes (commit);
      if (!validate_signature (commit_data, signatures, composefs_config->pubkeys, error))
        return glnx_throw (error, "No valid signatures found for public key");

      g_print ("composefs+ostree: Validated commit signature using '%s'\n", composefs_pubkey);
      g_variant_builder_add (metadata_builder, "{sv}", OTCORE_RUN_BOOTED_KEY_COMPOSEFS_SIGNATURE,
                             g_variant_new_string (composefs_pubkey));

      g_autoptr (GVariant) metadata = g_variant_get_child_value (commit, 0);
      g_autoptr (GVariant) cfs_digest_v = g_variant_lookup_value (
          metadata, OSTREE_COMPOSEFS_DIGEST_KEY_V0, G_VARIANT_TYPE_BYTESTRING);
      if (cfs_digest_v == NULL || g_variant_get_size (cfs_digest_v) != OSTREE_SHA256_DIGEST_LEN)
        return glnx_throw (error, "Signature validation requested, but no valid digest in commit");
      const guint8 *cfs_digest_buf = ot_variant_get_data (cfs_digest_v, error);
      if (!cfs_digest_buf)
        return glnx_prefix_error (error, "Failed to query digest");

      expected_digest = g_malloc (OSTREE_SHA256_STRING_LEN + 1);
      ot_bin2hex (expected_digest, cfs_digest_buf, g_variant_get_size (cfs_digest_v));

      g_assert (composefs_config->require_verity);
      cfs_options.flags |= LCFS_MOUNT_FLAGS_REQUIRE_VERITY;
      g_print ("composefs: Verifying digest: %s\n", expected_digest);
      cfs_options.expected_fsverity_digest = expected_digest;
    }
  else if (composefs_config->require_verity)
    {
      cfs_options.flags |= LCFS_MOUNT_FLAGS_REQUIRE_VERITY;
    }

  if (lcfs_mount_image (OSTREE_COMPOSEFS_NAME, mount_target, &cfs_options) == 0)
    {
      using_composefs = true;
      bool using_verity = (cfs_options.flags & LCFS_MOUNT_FLAGS_REQUIRE_VERITY) > 0;
      g_variant_builder_add (metadata_builder, "{sv}", OTCORE_RUN_BOOTED_KEY_COMPOSEFS,
                             g_variant_new_boolean (true));
      g_variant_builder_add (metadata_builder, "{sv}", OTCORE_RUN_BOOTED_KEY_COMPOSEFS_VERITY,
                             g_variant_new_boolean (using_verity));
      g_print ("composefs: mounted successfully (verity=%s)\n", using_verity ? "true" : "false");
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
          return glnx_throw (error, "composefs: failed to mount: %s", errmsg);
        }
    }
#else
  /* if composefs is configured as "maybe", we should continue */
  if (composefs_config->enabled == OT_TRISTATE_YES)
    return glnx_throw (error, "composefs: enabled at runtime, but support is not compiled in");
#endif
  *out_using_composefs = using_composefs;
  return TRUE;
}
