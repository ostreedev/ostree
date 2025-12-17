/* Copyright (C) 2023 Red Hat, Inc.
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

#include <fcntl.h>
#include <glib-unix.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/xattr.h>

#include "glnx-errors.h"
#include "glnx-fdio.h"
#include "glnx-local-alloc.h"
#include "glnx-shutil.h"
#include "glnx-xattrs.h"
#include "ostree-core.h"
#include "ostree-deployment.h"
#include "ot-admin-builtins.h"

#define OSTREE_STATEOVERLAYS_DIR "/var/ostree/state-overlays"
#define OSTREE_STATEOVERLAY_UPPER_DIR "upper"
#define OSTREE_STATEOVERLAY_WORK_DIR "work"

#define OSTREE_STATEOVERLAY_XATTR_DEPLOYMENT_CSUM "user.ostree.deploymentcsum"

/* https://www.kernel.org/doc/html/latest/filesystems/overlayfs.html */
#define OVERLAYFS_DIR_XATTR_OPAQUE "trusted.overlay.opaque"

static GOptionEntry options[] = { { NULL } };

static gboolean
ensure_overlay_dirs (const char *overlay_dir, int *out_overlay_dfd, GCancellable *cancellable,
                     GError **error)
{
  glnx_autofd int overlay_dfd = -1;
  if (!glnx_shutil_mkdir_p_at_open (AT_FDCWD, overlay_dir, 0700, &overlay_dfd, cancellable, error))
    return FALSE;

  if (!glnx_shutil_mkdir_p_at (overlay_dfd, OSTREE_STATEOVERLAY_WORK_DIR, 0700, cancellable, error))
    return FALSE;
  if (!glnx_shutil_mkdir_p_at (overlay_dfd, OSTREE_STATEOVERLAY_UPPER_DIR, 0700, cancellable,
                               error))
    return FALSE;

  *out_overlay_dfd = glnx_steal_fd (&overlay_dfd);
  return TRUE;
}

/* Based on glnx_lgetxattrat() from libglnx, modified to treat ENODATA
 * (xattr not set) as success with *out_bytes = NULL. We check errno
 * immediately after the lgetxattr syscall, before any GLib calls can
 * clobber it. This avoids depending on GLib's g_io_error_from_errno()
 * mapping, which only maps ENODATA to G_IO_ERROR_NOT_FOUND since GLib 2.74.
 *
 * This implementation handles the TOCTOU race condition where the xattr size
 * may change between the size query and the data read by retrying with ERANGE.
 * It also handles the case where the xattr is deleted between calls (ENODATA
 * on second call). Zero-length xattrs are handled without allocating a buffer.
 *
 * TODO: Upstream to libglnx. */
static gboolean
lgetxattrat_allow_nodata (int dfd, const char *path, const char *attribute, GBytes **out_bytes,
                          GError **error)
{
  char pathbuf[PATH_MAX];
  int n = snprintf (pathbuf, sizeof (pathbuf), "/proc/self/fd/%d/%s", dfd, path);
  if (n < 0 || n >= sizeof (pathbuf))
    return glnx_throw (error, "Path truncated for fd %d, path %s", dfd, path);

  ssize_t bytes_read;
  ssize_t real_size;
  g_autofree guint8 *buf = NULL;

again:
  errno = 0;
  bytes_read = TEMP_FAILURE_RETRY (lgetxattr (pathbuf, attribute, NULL, 0));
  if (bytes_read < 0)
    {
      if (errno == ENODATA)
        {
          *out_bytes = NULL;
          return TRUE; /* xattr not set; that's fine */
        }
      return glnx_throw_errno_prefix (error, "lgetxattr(%s)", attribute);
    }

  if (bytes_read == 0)
    {
      *out_bytes = g_bytes_new_static ("", 0);
      return TRUE;
    }

  buf = g_malloc (bytes_read);
  real_size = TEMP_FAILURE_RETRY (lgetxattr (pathbuf, attribute, buf, bytes_read));
  if (real_size < 0)
    {
      if (errno == ERANGE)
        {
          g_clear_pointer (&buf, g_free);
          goto again;
        }
      if (errno == ENODATA)
        {
          *out_bytes = NULL;
          return TRUE; /* xattr was deleted between calls */
        }
      return glnx_throw_errno_prefix (error, "lgetxattr(%s)", attribute);
    }

  *out_bytes = g_bytes_new_take (g_steal_pointer (&buf), real_size);
  return TRUE;
}

static gboolean
is_opaque_dir (int dfd, const char *dname, gboolean *out_is_opaque, GError **error)
{
  g_autoptr (GBytes) data = NULL;
  if (!lgetxattrat_allow_nodata (dfd, dname, OVERLAYFS_DIR_XATTR_OPAQUE, &data, error))
    return FALSE;

  if (!data)
    *out_is_opaque = FALSE;
  else
    {
      gsize size;
      const guint8 *buf = g_bytes_get_data (data, &size);
      *out_is_opaque = (size == 1 && buf[0] == 'y');
    }
  return TRUE;
}

static gboolean
prune_upperdir_recurse (int lower_dfd, int upper_dfd, GCancellable *cancellable, GError **error)
{
  g_auto (GLnxDirFdIterator) dfd_iter = { 0 };
  if (!glnx_dirfd_iterator_init_at (upper_dfd, ".", FALSE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      /* do we have an entry of the same name in the lowerdir? */
      struct stat stbuf;
      if (!glnx_fstatat_allow_noent (lower_dfd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW, error))
        return FALSE;
      if (errno == ENOENT)
        continue; /* state file (i.e. upperdir only); carry on */

      /* ok, it shadows; are they both directories? */
      if (dent->d_type == DT_DIR && S_ISDIR (stbuf.st_mode))
        {
          /* is the directory opaque? */
          gboolean is_opaque = FALSE;
          if (!is_opaque_dir (upper_dfd, dent->d_name, &is_opaque, error))
            return FALSE;

          if (!is_opaque)
            {
              /* recurse */
              glnx_autofd int lower_subdfd = -1;
              if (!glnx_opendirat (lower_dfd, dent->d_name, FALSE, &lower_subdfd, error))
                return FALSE;
              glnx_autofd int upper_subdfd = -1;
              if (!glnx_opendirat (upper_dfd, dent->d_name, FALSE, &upper_subdfd, error))
                return FALSE;
              if (!prune_upperdir_recurse (lower_subdfd, upper_subdfd, cancellable, error))
                return glnx_prefix_error (error, "in %s", dent->d_name);

              continue;
            }

          /* fallthrough; implicitly delete opaque directories */
        }

      /* any other case, we prune (this also implicitly covers whiteouts and opaque dirs) */
      if (dent->d_type == DT_DIR)
        {
          if (!glnx_shutil_rm_rf_at (upper_dfd, dent->d_name, cancellable, error))
            return FALSE;
        }
      else
        {
          /* just unlinkat(); saves one openat() call */
          if (!glnx_unlinkat (upper_dfd, dent->d_name, 0, error))
            return FALSE;
        }
    }

  return TRUE;
}

static gboolean
prune_upperdir (int sysroot_fd, const char *mountpath, int overlay_dfd, GCancellable *cancellable,
                GError **error)
{
  glnx_autofd int lower_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, mountpath, FALSE, &lower_dfd, error))
    return FALSE;

  glnx_autofd int upper_dfd = -1;
  if (!glnx_opendirat (overlay_dfd, OSTREE_STATEOVERLAY_UPPER_DIR, FALSE, &upper_dfd, error))
    return FALSE;

  if (!prune_upperdir_recurse (lower_dfd, upper_dfd, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
mount_overlay (const char *mountpath, const char *name, GError **error)
{
  /* we could use /proc/self/... with overlay_dfd to avoid these allocations,
   * but this gets stringified into the options field in the mount table, and
   * being cryptic is not helpful */
  g_autofree char *upperdir
      = g_build_filename (OSTREE_STATEOVERLAYS_DIR, name, OSTREE_STATEOVERLAY_UPPER_DIR, NULL);
  g_autofree char *workdir
      = g_build_filename (OSTREE_STATEOVERLAYS_DIR, name, OSTREE_STATEOVERLAY_WORK_DIR, NULL);
  g_autofree char *ovl_options
      = g_strdup_printf ("lowerdir=%s,upperdir=%s,workdir=%s", mountpath, upperdir, workdir);
  if (mount ("overlay", mountpath, "overlay", MS_SILENT, ovl_options) < 0)
    return glnx_throw_errno_prefix (error, "mount(%s)", mountpath);

  return TRUE;
}

static gboolean
get_overlay_deployment_checksum (int overlay_dfd, char **out_checksum, GCancellable *cancellable,
                                 GError **error)
{
  g_autoptr (GBytes) bytes = NULL;
  if (!lgetxattrat_allow_nodata (overlay_dfd, OSTREE_STATEOVERLAY_UPPER_DIR,
                                 OSTREE_STATEOVERLAY_XATTR_DEPLOYMENT_CSUM, &bytes, error))
    return FALSE;
  if (!bytes)
    return TRUE; /* probably newly created */

  gsize len;
  const char *data = g_bytes_get_data (bytes, &len);

  if (len != OSTREE_SHA256_STRING_LEN)
    return TRUE; /* invalid; gracefully handle as missing */

  *out_checksum = g_strndup (data, len);
  return TRUE;
}

static gboolean
set_overlay_deployment_checksum (int overlay_dfd, const char *checksum, GCancellable *cancellable,
                                 GError **error)
{
  g_assert_cmpuint (strlen (checksum), ==, OSTREE_SHA256_STRING_LEN);
  /* we could store it in binary of course, but let's make it more accessible for debugging */
  if (!glnx_lsetxattrat (overlay_dfd, OSTREE_STATEOVERLAY_UPPER_DIR,
                         OSTREE_STATEOVERLAY_XATTR_DEPLOYMENT_CSUM, (guint8 *)checksum,
                         OSTREE_SHA256_STRING_LEN, 0, error))
    return FALSE;
  return TRUE;
}

/* Called by ostree-state-overlay@.service. */
gboolean
ot_admin_builtin_state_overlay (int argc, char **argv, OstreeCommandInvocation *invocation,
                                GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("NAME MOUNTPATH");
  g_autoptr (OstreeSysroot) sysroot = NULL;

  /* First parse the args without loading the sysroot to see what options are
   * set. */
  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_SUPERUSER
                                              | OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED,
                                          invocation, &sysroot, cancellable, error))
    return FALSE;

  if (argc < 3)
    return glnx_throw (error, "Missing NAME or MOUNTPATH");

  /* Sanity-check */
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);
  if (booted_deployment == NULL)
    return glnx_throw (error, "Must be booted into an OSTree deployment");

  const char *overlay_name = argv[1];
  const char *mountpath = argv[2];

  glnx_autofd int overlay_dfd = -1;
  g_autofree char *overlay_dir = g_build_filename (OSTREE_STATEOVERLAYS_DIR, overlay_name, NULL);
  if (!ensure_overlay_dirs (overlay_dir, &overlay_dfd, cancellable, error))
    return FALSE;

  g_autofree char *current_checksum = NULL;
  if (!get_overlay_deployment_checksum (overlay_dfd, &current_checksum, cancellable, error))
    return FALSE;
  /* note current_checksum could still be NULL */

  const char *target_checksum = ostree_deployment_get_csum (booted_deployment);
  if (g_strcmp0 (current_checksum, target_checksum) != 0)
    {
      /* the lowerdir was updated; prune the upperdir */
      if (!prune_upperdir (ostree_sysroot_get_fd (sysroot), mountpath, overlay_dfd, cancellable,
                           error))
        return glnx_prefix_error (error, "Pruning upperdir for %s", overlay_name);

      if (!set_overlay_deployment_checksum (overlay_dfd, target_checksum, cancellable, error))
        return FALSE;
    }

  return mount_overlay (mountpath, overlay_name, error);
}
