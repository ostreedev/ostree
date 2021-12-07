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

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <gio/gfiledescriptorbased.h>
#include <gio/gunixinputstream.h>
#include "libglnx.h"
#include "ostree.h"
#include "ostree-core-private.h"
#include "ostree-repo-os.h"
#include "otutil.h"

/**
 * ostree_commit_metadata_for_bootable:
 * @root: Root filesystem to be committed
 * @dict: Dictionary to update
 *
 * Update provided @dict with standard metadata for bootable OSTree commits.
 * Since: 2021.1
 */
_OSTREE_PUBLIC
gboolean
ostree_commit_metadata_for_bootable (GFile *root, GVariantDict *dict, GCancellable *cancellable, GError **error)
{
  g_autoptr(GFile) modules = g_file_resolve_relative_path (root, "usr/lib/modules");
  g_autoptr(GFileEnumerator) dir_enum 
    = g_file_enumerate_children (modules, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!dir_enum)
    return glnx_prefix_error (error, "Opening usr/lib/modules");

  g_autofree char *linux_release = NULL;
  while (TRUE)
    {
      GFileInfo *child_info;
      GFile *child_path;
      if (!g_file_enumerator_iterate (dir_enum, &child_info, &child_path,
                                      cancellable, error))
        return FALSE;
      if (child_info == NULL)
        break;
      if (g_file_info_get_file_type (child_info) != G_FILE_TYPE_DIRECTORY)
        continue;
    
      g_autoptr(GFile) kernel_path = g_file_resolve_relative_path (child_path, "vmlinuz");
      if (!g_file_query_exists (kernel_path, NULL))
        continue;

      if (linux_release != NULL)
        return glnx_throw (error, "Multiple kernels found in /usr/lib/modules");

      linux_release = g_strdup (g_file_info_get_name (child_info));
    }

  if (linux_release)
    {
      g_variant_dict_insert (dict, OSTREE_METADATA_KEY_BOOTABLE, "b", TRUE);
      g_variant_dict_insert (dict, OSTREE_METADATA_KEY_LINUX, "s", linux_release);
      return TRUE;
    }
  return glnx_throw (error, "No kernel found in /usr/lib/modules");
}
