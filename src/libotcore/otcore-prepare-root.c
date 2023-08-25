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
otcore_get_ostree_target (const char *cmdline, char **out_target, GError **error)
{
  g_assert (cmdline);
  g_assert (out_target && *out_target == NULL);
  static const char slot_a[] = "/ostree/root.a";
  static const char slot_b[] = "/ostree/root.b";

  // First, handle the Android boot case
  g_autofree char *slot_suffix = otcore_find_proc_cmdline_key (cmdline, "androidboot.slot_suffix");
  if (slot_suffix)
    {
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

      g_print ("Loading %s\n", path);

      g_autofree char *buf = glnx_fd_readall_utf8 (fd, NULL, NULL, error);
      if (!buf)
        return NULL;
      if (!g_key_file_load_from_data (ret, buf, -1, 0, error))
        return NULL;
    }

  return g_steal_pointer (&ret);
}
