/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef HAVE_SELINUX
#include <selinux/restorecon.h>
#endif

#include "ostree-mount-util.h"
#include "otcore.h"

/* Relabel the directory $real_path, which is going to be an overlayfs mount,
 * based on the content of an overlayfs upperdirectory that is in use by the mount.
 * The goal is that we relabel in the overlay mount all the files that have been
 * modified (directly or via parent copyup operations) since the overlayfs was
 * mounted. This will be used for the /etc overlayfs mount where no selinux labels
 * are set before selinux policy is loaded.
 */
static void
relabel_dir_for_upper (const char *upper_path, const char *real_path, gboolean is_dir)
{
#ifdef HAVE_SELINUX
  /* Ignore ENOENT, because if there is no file to relabel we can continue,
   * systemd-sysusers runs in parallel and can create temporary files in /etc
   * causing failures like:
   * "Failed to relabel /etc/.#gshadowJzu4Rx: No such file or directory"
   */
  if (selinux_restorecon (real_path, 0))
    {
      if (errno == ENOENT)
        return;

      err (EXIT_FAILURE, "Failed to relabel %s", real_path);
    }

  if (!is_dir)
    return;

  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };

  if (!glnx_dirfd_iterator_init_at (AT_FDCWD, upper_path, FALSE, &dfd_iter, NULL))
    err (EXIT_FAILURE, "Failed to open upper directory %s for relabeling", upper_path);

  while (TRUE)
    {
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, NULL, NULL))
        {
          err (EXIT_FAILURE, "Failed to read upper directory %s for relabelin", upper_path);
          break;
        }

      if (dent == NULL)
        break;

      g_autofree char *upper_child = g_build_filename (upper_path, dent->d_name, NULL);
      g_autofree char *real_child = g_build_filename (real_path, dent->d_name, NULL);
      relabel_dir_for_upper (upper_child, real_child, dent->d_type == DT_DIR);
    }
#endif
}

int
main (int argc, char *argv[])
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ostree_run_metadata_v = NULL;
  {
    glnx_autofd int fd = open (OTCORE_RUN_BOOTED, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      {
        /* We really expect that nowadays that everything is done in the initramfs,
         * but historically we created this file here, so we'll continue to do be
         * sure here it exists.  This code should be removed at some point though.
         */
        if (errno == ENOENT)
          {
            int subfd = open (OTCORE_RUN_BOOTED, O_EXCL | O_CREAT | O_WRONLY | O_NOCTTY | O_CLOEXEC,
                              0640);
            if (subfd != -1)
              (void)close (subfd);
          }
        else
          {
            err (EXIT_FAILURE, "failed to open %s", OTCORE_RUN_BOOTED);
          }
      }
    else
      {
        if (!ot_variant_read_fd (fd, 0, G_VARIANT_TYPE_VARDICT, TRUE, &ostree_run_metadata_v,
                                 &error))
          errx (EXIT_FAILURE, "failed to read %s: %s", OTCORE_RUN_BOOTED, error->message);
      }
  }

  g_autoptr (GVariantDict) ostree_run_metadata = g_variant_dict_new (ostree_run_metadata_v);
  const char *transient_etc = NULL;
  g_variant_dict_lookup (ostree_run_metadata, OTCORE_RUN_BOOTED_KEY_TRANSIENT_ETC, "&s",
                         &transient_etc);
  if (transient_etc)
    {
      /* If the initramfs created any files in /etc (directly or via overlay copy-up) they
       * will be unlabeled, because the selinux policy is not loaded until after the
       * pivot-root. So, for all files in the upper dir, relabel the corresponding overlay
       * file.
       *
       * Also, note that during boot systemd will create a /run/machine-id ->
       * /etc/machine-id bind mount (as /etc is read-only early on). It will then later
       * replace this mount with a real one (in systemd-machine-id-commit.service).
       *
       * We need to label the actual overlayfs file, not the temporary bind-mount. To do
       * this we unmount the covering mount before relabeling, but we do so in a temporary
       * private namespace to avoid affecting other parts of the system.
       */

      glnx_autofd int initial_ns_fd = -1;
      if (g_file_test ("/run/machine-id", G_FILE_TEST_EXISTS)
          && g_file_test ("/etc/machine-id", G_FILE_TEST_EXISTS))
        {
          initial_ns_fd = open ("/proc/self/ns/mnt", O_RDONLY | O_NOCTTY | O_CLOEXEC);
          if (initial_ns_fd < 0)
            err (EXIT_FAILURE, "Failed to open initial namespace");

          if (unshare (CLONE_NEWNS) < 0)
            err (EXIT_FAILURE, "Failed to unshare initial namespace");

          /* Ensure unmount is not propagated */
          if (mount ("none", "/etc", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
            err (EXIT_FAILURE, "warning: While remounting /etc MS_PRIVATE");

          if (umount2 ("/etc/machine-id", MNT_DETACH) < 0)
            err (EXIT_FAILURE, "Failed to unmount machine-id");
        }

      g_autofree char *upper = g_build_filename (transient_etc, "upper", NULL);
      relabel_dir_for_upper (upper, "/etc", TRUE);

      if (initial_ns_fd != -1 && setns (initial_ns_fd, CLONE_NEWNS) < 0)
        err (EXIT_FAILURE, "Failed to join initial namespace");
    }

  exit (EXIT_SUCCESS);
}
