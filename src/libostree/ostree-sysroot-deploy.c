/*
 * Copyright (C) 2012,2014 Colin Walters <walters@verbum.org>
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

#include <err.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib-unix.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/statvfs.h>

#ifdef HAVE_LIBMOUNT
#include <libmount.h>
#endif
#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-journal.h>
#endif

#include "libglnx.h"
#include "ostree-core-private.h"
#include "ostree-deployment-private.h"
#include "ostree-linuxfsutil.h"
#include "ostree-repo-private.h"
#include "ostree-sepolicy-private.h"
#include "ostree-sysroot-private.h"
#include "ostree.h"
#include "otcore.h"

#ifdef HAVE_LIBSYSTEMD
#define OSTREE_VARRELABEL_ID \
  SD_ID128_MAKE (da, 67, 9b, 08, ac, d3, 45, 04, b7, 89, d9, 6f, 81, 8e, a7, 81)
#define OSTREE_CONFIGMERGE_ID \
  SD_ID128_MAKE (d3, 86, 3b, ae, c1, 3e, 44, 49, ab, 03, 84, 68, 4a, 8a, f3, a7)
#define OSTREE_DEPLOYMENT_COMPLETE_ID \
  SD_ID128_MAKE (dd, 44, 0e, 3e, 54, 90, 83, b6, 3d, 0e, fc, 7d, c1, 52, 55, f1)
#define OSTREE_DEPLOYMENT_FINALIZING_ID \
  SD_ID128_MAKE (e8, 64, 6c, d6, 3d, ff, 46, 25, b7, 79, 09, a8, e7, a4, 09, 94)
#endif

/*
 * Like symlinkat() but overwrites (atomically) an existing
 * symlink.
 */
static gboolean
symlink_at_replace (const char *oldpath, int parent_dfd, const char *newpath,
                    GCancellable *cancellable, GError **error)
{
  /* Possibly in the future generate a temporary random name here,
   * would need to move "generate a temporary name" code into
   * libglnx or glib?
   */
  g_autofree char *temppath = g_strconcat (newpath, ".tmp", NULL);

  /* Clean up any stale temporary links */
  (void)unlinkat (parent_dfd, temppath, 0);

  /* Create the temp link */
  if (TEMP_FAILURE_RETRY (symlinkat (oldpath, parent_dfd, temppath)) < 0)
    return glnx_throw_errno_prefix (error, "symlinkat");

  /* Rename it into place */
  if (!glnx_renameat (parent_dfd, temppath, parent_dfd, newpath, error))
    return FALSE;

  return TRUE;
}

static GLnxFileCopyFlags
sysroot_flags_to_copy_flags (GLnxFileCopyFlags defaults, OstreeSysrootDebugFlags sysrootflags)
{
  if (sysrootflags & OSTREE_SYSROOT_DEBUG_NO_XATTRS)
    defaults |= GLNX_FILE_COPY_NOXATTRS;
  return defaults;
}

/* Try a hardlink if we can, otherwise fall back to copying.  Used
 * right now for kernels/initramfs/device trees in /boot, where we can just
 * hardlink if we're on the same partition.
 */
static gboolean
install_into_boot (OstreeRepo *repo, OstreeSePolicy *sepolicy, int src_dfd, const char *src_subpath,
                   int dest_dfd, const char *dest_subpath, GCancellable *cancellable,
                   GError **error)
{
  if (linkat (src_dfd, src_subpath, dest_dfd, dest_subpath, 0) == 0)
    return TRUE; /* Note early return */
  if (!G_IN_SET (errno, EMLINK, EXDEV))
    return glnx_throw_errno_prefix (error, "linkat(%s)", dest_subpath);

  /* Otherwise, copy */
  struct stat src_stbuf;
  if (!glnx_fstatat (src_dfd, src_subpath, &src_stbuf, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;

  glnx_autofd int src_fd = -1;
  if (!glnx_openat_rdonly (src_dfd, src_subpath, FALSE, &src_fd, error))
    return FALSE;

  /* Be sure we relabel when copying the kernel, as in current
   * e.g. Fedora it might be labeled module_object_t or usr_t,
   * but policy may not allow other processes to read from that
   * like kdump.
   * See also
   * https://github.com/fedora-selinux/selinux-policy/commit/747f4e6775d773ab74efae5aa37f3e5e7f0d4aca
   * This means we also drop xattrs but...I doubt anyone uses
   * non-SELinux xattrs for the kernel anyways aside from perhaps
   * IMA but that's its own story.
   */
  g_auto (OstreeSepolicyFsCreatecon) fscreatecon = {
    0,
  };
  const char *boot_path = glnx_strjoina ("/boot/", glnx_basename (dest_subpath));
  if (!_ostree_sepolicy_preparefscreatecon (&fscreatecon, sepolicy, boot_path, S_IFREG | 0644,
                                            error))
    return FALSE;

  g_auto (GLnxTmpfile) tmp_dest = {
    0,
  };
  if (!glnx_open_tmpfile_linkable_at (dest_dfd, ".", O_WRONLY | O_CLOEXEC, &tmp_dest, error))
    return FALSE;

  if (glnx_regfile_copy_bytes (src_fd, tmp_dest.fd, (off_t)-1) < 0)
    return glnx_throw_errno_prefix (error, "regfile copy");

  /* Kernel data should always be root-owned */
  if (fchown (tmp_dest.fd, src_stbuf.st_uid, src_stbuf.st_gid) != 0)
    return glnx_throw_errno_prefix (error, "fchown");

  if (fchmod (tmp_dest.fd, src_stbuf.st_mode & 07777) != 0)
    return glnx_throw_errno_prefix (error, "fchmod");

  if (fdatasync (tmp_dest.fd) < 0)
    return glnx_throw_errno_prefix (error, "fdatasync");

  /* Today we don't have a config flag to *require* verity on /boot,
   * and at least for Fedora CoreOS we're not likely to do fsverity on
   * /boot soon due to wanting to support mounting it from old Linux
   * kernels.  So change "required" to "maybe".
   */
  _OstreeFeatureSupport boot_verity = _OSTREE_FEATURE_NO;
  if (repo->fs_verity_wanted != _OSTREE_FEATURE_NO)
    boot_verity = _OSTREE_FEATURE_MAYBE;
  if (!_ostree_tmpf_fsverity_core (&tmp_dest, boot_verity, NULL, NULL, error))
    return FALSE;

  if (!glnx_link_tmpfile_at (&tmp_dest, GLNX_LINK_TMPFILE_NOREPLACE, dest_dfd, dest_subpath, error))
    return FALSE;

  return TRUE;
}

/* Copy ownership, mode, and xattrs from source directory to destination */
static gboolean
dirfd_copy_attributes_and_xattrs (int src_parent_dfd, const char *src_name, int src_dfd,
                                  int dest_dfd, OstreeSysrootDebugFlags flags,
                                  GCancellable *cancellable, GError **error)
{
  g_autoptr (GVariant) xattrs = NULL;

  /* Clone all xattrs first, so we get the SELinux security context
   * right.  This will allow other users access if they have ACLs, but
   * oh well.
   */
  if (!(flags & OSTREE_SYSROOT_DEBUG_NO_XATTRS))
    {
      if (!glnx_dfd_name_get_all_xattrs (src_parent_dfd, src_name, &xattrs, cancellable, error))
        return FALSE;
      if (!glnx_fd_set_all_xattrs (dest_dfd, xattrs, cancellable, error))
        return FALSE;
    }

  struct stat src_stbuf;
  if (!glnx_fstat (src_dfd, &src_stbuf, error))
    return FALSE;
  if (fchown (dest_dfd, src_stbuf.st_uid, src_stbuf.st_gid) != 0)
    return glnx_throw_errno_prefix (error, "fchown");
  if (fchmod (dest_dfd, src_stbuf.st_mode) != 0)
    return glnx_throw_errno_prefix (error, "fchmod");

  return TRUE;
}

static gint
str_sort_cb (gconstpointer name_ptr_a, gconstpointer name_ptr_b)
{
  const gchar *name_a = *((const gchar **)name_ptr_a);
  const gchar *name_b = *((const gchar **)name_ptr_b);

  return g_strcmp0 (name_a, name_b);
}

static gboolean
checksum_dir_recurse (int dfd, const char *path, OtChecksum *checksum, GCancellable *cancellable,
                      GError **error)
{
  g_auto (GLnxDirFdIterator) dfditer = {
    0,
  };
  g_autoptr (GPtrArray) d_entries = g_ptr_array_new_with_free_func (g_free);

  if (!glnx_dirfd_iterator_init_at (dfd, path, TRUE, &dfditer, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent (&dfditer, &dent, cancellable, error))
        return FALSE;

      if (dent == NULL)
        break;

      g_ptr_array_add (d_entries, g_strdup (dent->d_name));
    }

  /* File systems do not guarantee dir entry order, make sure this is
   * reproducable
   */
  g_ptr_array_sort (d_entries, str_sort_cb);

  for (gint i = 0; i < d_entries->len; i++)
    {
      const gchar *d_name = (gchar *)g_ptr_array_index (d_entries, i);
      struct stat stbuf;

      if (!glnx_fstatat (dfditer.fd, d_name, &stbuf, AT_SYMLINK_NOFOLLOW, error))
        return FALSE;

      if (S_ISDIR (stbuf.st_mode))
        {
          if (!checksum_dir_recurse (dfditer.fd, d_name, checksum, cancellable, error))
            return FALSE;
        }
      else
        {
          glnx_autofd int fd = -1;

          if (!ot_openat_ignore_enoent (dfditer.fd, d_name, &fd, error))
            return FALSE;
          if (fd != -1)
            {
              g_autoptr (GInputStream) in = g_unix_input_stream_new (g_steal_fd (&fd), TRUE);
              if (!ot_gio_splice_update_checksum (NULL, in, checksum, cancellable, error))
                return FALSE;
            }
        }
    }

  return TRUE;
}

static gboolean
copy_dir_recurse (int src_parent_dfd, int dest_parent_dfd, const char *name,
                  OstreeSysrootDebugFlags flags, GCancellable *cancellable, GError **error)
{
  g_auto (GLnxDirFdIterator) src_dfd_iter = {
    0,
  };
  glnx_autofd int dest_dfd = -1;
  struct dirent *dent;

  if (!glnx_dirfd_iterator_init_at (src_parent_dfd, name, TRUE, &src_dfd_iter, error))
    return FALSE;

  /* Create with mode 0700, we'll fchmod/fchown later */
  if (!glnx_ensure_dir (dest_parent_dfd, name, 0700, error))
    return FALSE;

  if (!glnx_opendirat (dest_parent_dfd, name, TRUE, &dest_dfd, error))
    return FALSE;

  if (!dirfd_copy_attributes_and_xattrs (src_parent_dfd, name, src_dfd_iter.fd, dest_dfd, flags,
                                         cancellable, error))
    return glnx_prefix_error (error, "Copying attributes of %s", name);

  while (TRUE)
    {
      struct stat child_stbuf;

      if (!glnx_dirfd_iterator_next_dent (&src_dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (!glnx_fstatat (src_dfd_iter.fd, dent->d_name, &child_stbuf, AT_SYMLINK_NOFOLLOW, error))
        return FALSE;

      if (S_ISDIR (child_stbuf.st_mode))
        {
          if (!copy_dir_recurse (src_dfd_iter.fd, dest_dfd, dent->d_name, flags, cancellable,
                                 error))
            return FALSE;
        }
      else
        {
          if (!glnx_file_copy_at (src_dfd_iter.fd, dent->d_name, &child_stbuf, dest_dfd,
                                  dent->d_name,
                                  sysroot_flags_to_copy_flags (GLNX_FILE_COPY_OVERWRITE, flags),
                                  cancellable, error))
            return glnx_prefix_error (error, "Copying %s", dent->d_name);
        }
    }

  return TRUE;
}

/* If a chain of directories is added, this function will ensure
 * they're created.
 */
static gboolean
ensure_directory_from_template (int orig_etc_fd, int modified_etc_fd, int new_etc_fd,
                                const char *path, int *out_dfd, OstreeSysrootDebugFlags flags,
                                GCancellable *cancellable, GError **error)
{
  glnx_autofd int src_dfd = -1;
  glnx_autofd int target_dfd = -1;

  g_assert (path != NULL);
  g_assert (*path != '/' && *path != '\0');

  if (!glnx_opendirat (modified_etc_fd, path, TRUE, &src_dfd, error))
    return FALSE;

  /* Create with mode 0700, we'll fchmod/fchown later */
again:
  if (mkdirat (new_etc_fd, path, 0700) != 0)
    {
      if (errno == EEXIST)
        {
          /* Fall through */
        }
      else if (errno == ENOENT)
        {
          g_autofree char *parent_path = g_path_get_dirname (path);

          if (strcmp (parent_path, ".") != 0)
            {
              if (!ensure_directory_from_template (orig_etc_fd, modified_etc_fd, new_etc_fd,
                                                   parent_path, NULL, flags, cancellable, error))
                return FALSE;

              /* Loop */
              goto again;
            }
          else
            {
              /* Fall through...shouldn't happen, but we'll propagate
               * an error from open. */
            }
        }
      else
        return glnx_throw_errno_prefix (error, "mkdirat");
    }

  if (!glnx_opendirat (new_etc_fd, path, TRUE, &target_dfd, error))
    return FALSE;

  if (!dirfd_copy_attributes_and_xattrs (modified_etc_fd, path, src_dfd, target_dfd, flags,
                                         cancellable, error))
    return FALSE;

  if (out_dfd)
    *out_dfd = g_steal_fd (&target_dfd);
  return TRUE;
}

/* Copy (relative) @path from @modified_etc_fd to @new_etc_fd, overwriting any
 * existing file there. The @path may refer to a regular file, a symbolic link,
 * or a directory. Directories will be copied recursively.
 */
static gboolean
copy_modified_config_file (int orig_etc_fd, int modified_etc_fd, int new_etc_fd, const char *path,
                           OstreeSysrootDebugFlags flags, GCancellable *cancellable, GError **error)
{
  struct stat modified_stbuf;
  struct stat new_stbuf;

  if (!glnx_fstatat (modified_etc_fd, path, &modified_stbuf, AT_SYMLINK_NOFOLLOW, error))
    return glnx_prefix_error (error, "Reading modified config file");

  glnx_autofd int dest_parent_dfd = -1;
  if (strchr (path, '/') != NULL)
    {
      g_autofree char *parent = g_path_get_dirname (path);

      if (!ensure_directory_from_template (orig_etc_fd, modified_etc_fd, new_etc_fd, parent,
                                           &dest_parent_dfd, flags, cancellable, error))
        return FALSE;
    }
  else
    {
      dest_parent_dfd = dup (new_etc_fd);
      if (dest_parent_dfd == -1)
        return glnx_throw_errno_prefix (error, "dup");
    }

  g_assert (dest_parent_dfd != -1);

  if (fstatat (new_etc_fd, path, &new_stbuf, AT_SYMLINK_NOFOLLOW) < 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "fstatat");
    }
  else if (S_ISDIR (new_stbuf.st_mode))
    {
      if (!S_ISDIR (modified_stbuf.st_mode))
        {
          return g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              "Modified config file newly defaults to directory '%s', cannot merge",
                              path),
                 FALSE;
        }
      else
        {
          /* Do nothing here - we assume that we've already
           * recursively copied the parent directory.
           */
          return TRUE;
        }
    }
  else
    {
      if (!glnx_unlinkat (new_etc_fd, path, 0, error))
        return FALSE;
    }

  if (S_ISDIR (modified_stbuf.st_mode))
    {
      if (!copy_dir_recurse (modified_etc_fd, new_etc_fd, path, flags, cancellable, error))
        return FALSE;
    }
  else if (S_ISLNK (modified_stbuf.st_mode) || S_ISREG (modified_stbuf.st_mode))
    {
      if (!glnx_file_copy_at (modified_etc_fd, path, &modified_stbuf, new_etc_fd, path,
                              sysroot_flags_to_copy_flags (GLNX_FILE_COPY_OVERWRITE, flags),
                              cancellable, error))
        return glnx_prefix_error (error, "Copying %s", path);
    }
  else
    {
      ot_journal_print (LOG_INFO,
                        "Ignoring non-regular/non-symlink file found during /etc merge: %s", path);
    }

  return TRUE;
}

/*
 * merge_configuration_from:
 * @sysroot: Sysroot
 * @merge_deployment: Source of configuration differences
 * @new_deployment: Target for merge of configuration
 * @new_deployment_dfd: Directory fd for @new_deployment (may *not* be -1)
 * @cancellable: Cancellable
 * @error: Error
 *
 * Compute the difference between @merge_deployment's `/usr/etc` and `/etc`, and
 * apply that to @new_deployment's `/etc`.
 *
 * The algorithm for computing the difference is pretty simple; it's
 * approximately equivalent to "diff -unR orig_etc modified_etc",
 * except that rather than attempting a 3-way merge if a file is also
 * changed in @new_etc, the modified version always wins.
 */
static gboolean
merge_configuration_from (OstreeSysroot *sysroot, OstreeDeployment *merge_deployment,
                          OstreeDeployment *new_deployment, int new_deployment_dfd,
                          GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("During /etc merge", error);
  const OstreeSysrootDebugFlags flags = sysroot->debug_flags;

  g_assert (merge_deployment != NULL && new_deployment != NULL);
  g_assert (new_deployment_dfd != -1);

  g_autofree char *merge_deployment_path
      = ostree_sysroot_get_deployment_dirpath (sysroot, merge_deployment);
  glnx_autofd int merge_deployment_dfd = -1;
  if (!glnx_opendirat (sysroot->sysroot_fd, merge_deployment_path, FALSE, &merge_deployment_dfd,
                       error))
    return FALSE;

  /* TODO: get rid of GFile usage here */
  g_autoptr (GFile) orig_etc = ot_fdrel_to_gfile (merge_deployment_dfd, "usr/etc");
  g_autoptr (GFile) modified_etc = ot_fdrel_to_gfile (merge_deployment_dfd, "etc");
  /* Return values for below */
  g_autoptr (GPtrArray) modified
      = g_ptr_array_new_with_free_func ((GDestroyNotify)ostree_diff_item_unref);
  g_autoptr (GPtrArray) removed = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  g_autoptr (GPtrArray) added = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  /* For now, ignore changes to xattrs; the problem is that
   * security.selinux will be different between the /usr/etc labels
   * and the ones in the real /etc, so they all show up as different.
   *
   * This means that if you want to change the security context of a
   * file, to have that change persist across upgrades, you must also
   * modify the content of the file.
   */
  if (!ostree_diff_dirs (OSTREE_DIFF_FLAGS_IGNORE_XATTRS, orig_etc, modified_etc, modified, removed,
                         added, cancellable, error))
    return glnx_prefix_error (error, "While computing configuration diff");

  {
    g_autofree char *msg
        = g_strdup_printf ("Copying /etc changes: %u modified, %u removed, %u added", modified->len,
                           removed->len, added->len);
    ot_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL (OSTREE_CONFIGMERGE_ID),
                     "MESSAGE=%s", msg, "ETC_N_MODIFIED=%u", modified->len, "ETC_N_REMOVED=%u",
                     removed->len, "ETC_N_ADDED=%u", added->len, NULL);
    _ostree_sysroot_emit_journal_msg (sysroot, msg);
  }

  glnx_autofd int orig_etc_fd = -1;
  if (!glnx_opendirat (merge_deployment_dfd, "usr/etc", TRUE, &orig_etc_fd, error))
    return FALSE;
  glnx_autofd int modified_etc_fd = -1;
  if (!glnx_opendirat (merge_deployment_dfd, "etc", TRUE, &modified_etc_fd, error))
    return FALSE;
  glnx_autofd int new_etc_fd = -1;
  if (!glnx_opendirat (new_deployment_dfd, "etc", TRUE, &new_etc_fd, error))
    return FALSE;

  for (guint i = 0; i < removed->len; i++)
    {
      GFile *file = removed->pdata[i];
      g_autofree char *path = NULL;

      path = g_file_get_relative_path (orig_etc, file);
      g_assert (path);

      if (!glnx_shutil_rm_rf_at (new_etc_fd, path, cancellable, error))
        return FALSE;
    }

  for (guint i = 0; i < modified->len; i++)
    {
      OstreeDiffItem *diff = modified->pdata[i];
      g_autofree char *path = g_file_get_relative_path (modified_etc, diff->target);

      g_assert (path);

      if (!copy_modified_config_file (orig_etc_fd, modified_etc_fd, new_etc_fd, path, flags,
                                      cancellable, error))
        return FALSE;
    }
  for (guint i = 0; i < added->len; i++)
    {
      GFile *file = added->pdata[i];
      g_autofree char *path = g_file_get_relative_path (modified_etc, file);

      g_assert (path);

      if (!copy_modified_config_file (orig_etc_fd, modified_etc_fd, new_etc_fd, path, flags,
                                      cancellable, error))
        return FALSE;
    }

  return TRUE;
}

#ifdef HAVE_COMPOSEFS
static gboolean
compare_verity_digests (GVariant *metadata_composefs, const guchar *fsverity_digest, GError **error)
{
  const guchar *expected_digest;

  if (metadata_composefs == NULL)
    return TRUE;

  if (g_variant_n_children (metadata_composefs) != OSTREE_SHA256_DIGEST_LEN)
    return glnx_throw (error, "Expected composefs fs-verity in metadata has the wrong size");

  expected_digest = g_variant_get_data (metadata_composefs);
  if (memcmp (fsverity_digest, expected_digest, OSTREE_SHA256_DIGEST_LEN) != 0)
    {
      char actual_checksum[OSTREE_SHA256_STRING_LEN + 1];
      char expected_checksum[OSTREE_SHA256_STRING_LEN + 1];

      ostree_checksum_inplace_from_bytes (fsverity_digest, actual_checksum);
      ostree_checksum_inplace_from_bytes (expected_digest, expected_checksum);

      return glnx_throw (error,
                         "Generated composefs image digest (%s) doesn't match expected digest (%s)",
                         actual_checksum, expected_checksum);
    }

  return TRUE;
}

#endif

/* Look up @revision in the repository, and check it out in
 * /ostree/deploy/OS/deploy/${treecsum}.${deployserial}.
 * A dfd for the result is returned in @out_deployment_dfd.
 */
static gboolean
checkout_deployment_tree (OstreeSysroot *sysroot, OstreeRepo *repo, OstreeDeployment *deployment,
                          const char *revision, int *out_deployment_dfd, GCancellable *cancellable,
                          GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Checking out deployment tree", error);
  /* Find the directory with deployments for this stateroot */
  g_autofree char *osdeploy_path
      = g_strconcat ("ostree/deploy/", ostree_deployment_get_osname (deployment), "/deploy", NULL);
  if (!glnx_shutil_mkdir_p_at (sysroot->sysroot_fd, osdeploy_path, 0775, cancellable, error))
    return FALSE;

  glnx_autofd int osdeploy_dfd = -1;
  if (!glnx_opendirat (sysroot->sysroot_fd, osdeploy_path, TRUE, &osdeploy_dfd, error))
    return FALSE;

  /* Clean up anything that was there before, from e.g. an interrupted checkout */
  const char *csum = ostree_deployment_get_csum (deployment);
  g_autofree char *checkout_target_name
      = g_strdup_printf ("%s.%d", csum, ostree_deployment_get_deployserial (deployment));
  if (!glnx_shutil_rm_rf_at (osdeploy_dfd, checkout_target_name, cancellable, error))
    return FALSE;

  /* Generate hardlink farm, then opendir it */
  OstreeRepoCheckoutAtOptions checkout_opts = { .process_passthrough_whiteouts = TRUE };
  if (!ostree_repo_checkout_at (repo, &checkout_opts, osdeploy_dfd, checkout_target_name, csum,
                                cancellable, error))
    return FALSE;

#ifdef HAVE_COMPOSEFS
  if (repo->composefs_wanted != OT_TRISTATE_NO)
    {
      g_autofree guchar *fsverity_digest = NULL;
      g_auto (GLnxTmpfile) tmpf = {
        0,
      };
      g_autoptr (GVariant) commit_variant = NULL;

      if (!ostree_repo_load_commit (repo, revision, &commit_variant, NULL, error))
        return FALSE;

      g_autoptr (GVariant) metadata = g_variant_get_child_value (commit_variant, 0);
      g_autoptr (GVariant) metadata_composefs = g_variant_lookup_value (
          metadata, OSTREE_COMPOSEFS_DIGEST_KEY_V0, G_VARIANT_TYPE_BYTESTRING);

      /* Create a composefs image and put in deploy dir */
      g_autoptr (OstreeComposefsTarget) target = ostree_composefs_target_new ();

      g_autoptr (GFile) commit_root = NULL;
      if (!ostree_repo_read_commit (repo, csum, &commit_root, NULL, cancellable, error))
        return FALSE;

      if (!ostree_repo_checkout_composefs (repo, target, (OstreeRepoFile *)commit_root, cancellable,
                                           error))
        return FALSE;

      g_autofree char *composefs_cfs_path
          = g_strdup_printf ("%s/" OSTREE_COMPOSEFS_NAME, checkout_target_name);

      if (!glnx_open_tmpfile_linkable_at (osdeploy_dfd, checkout_target_name, O_WRONLY | O_CLOEXEC,
                                          &tmpf, error))
        return FALSE;

      if (!ostree_composefs_target_write (target, tmpf.fd, &fsverity_digest, cancellable, error))
        return FALSE;

      /* If the commit specified a composefs digest, verify it */
      if (!compare_verity_digests (metadata_composefs, fsverity_digest, error))
        return FALSE;

      if (!glnx_fchmod (tmpf.fd, 0644, error))
        return FALSE;

      if (!_ostree_tmpf_fsverity (repo, &tmpf, NULL, error))
        return FALSE;

      if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_REPLACE, osdeploy_dfd, composefs_cfs_path,
                                 error))
        return FALSE;
    }
#endif

  return glnx_opendirat (osdeploy_dfd, checkout_target_name, TRUE, out_deployment_dfd, error);
}

static char *
ptrarray_path_join (GPtrArray *path)
{
  GString *path_buf;

  path_buf = g_string_new ("");

  if (path->len == 0)
    g_string_append_c (path_buf, '/');
  else
    {
      guint i;
      for (i = 0; i < path->len; i++)
        {
          const char *elt = path->pdata[i];

          g_string_append_c (path_buf, '/');
          g_string_append (path_buf, elt);
        }
    }

  return g_string_free (path_buf, FALSE);
}

static gboolean
relabel_one_path (OstreeSysroot *sysroot, OstreeSePolicy *sepolicy, GFile *path, GFileInfo *info,
                  GPtrArray *path_parts, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autofree char *relpath = NULL;

  relpath = ptrarray_path_join (path_parts);
  if (!ostree_sepolicy_restorecon (sepolicy, relpath, info, path,
                                   OSTREE_SEPOLICY_RESTORECON_FLAGS_ALLOW_NOLABEL, NULL,
                                   cancellable, error))
    goto out;

  ret = TRUE;
out:
  return ret;
}

static gboolean
relabel_recursively (OstreeSysroot *sysroot, OstreeSePolicy *sepolicy, GFile *dir,
                     GFileInfo *dir_info, GPtrArray *path_parts, GCancellable *cancellable,
                     GError **error)
{
  gboolean ret = FALSE;
  g_autoptr (GFileEnumerator) direnum = NULL;

  if (!relabel_one_path (sysroot, sepolicy, dir, dir_info, path_parts, cancellable, error))
    goto out;

  direnum = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO,
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
  if (!direnum)
    goto out;

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *child;
      GFileType ftype;

      if (!g_file_enumerator_iterate (direnum, &file_info, &child, cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      g_ptr_array_add (path_parts, (char *)g_file_info_get_name (file_info));

      ftype = g_file_info_get_file_type (file_info);
      if (ftype == G_FILE_TYPE_DIRECTORY)
        {
          if (!relabel_recursively (sysroot, sepolicy, child, file_info, path_parts, cancellable,
                                    error))
            goto out;
        }
      else
        {
          if (!relabel_one_path (sysroot, sepolicy, child, file_info, path_parts, cancellable,
                                 error))
            goto out;
        }

      g_ptr_array_remove_index (path_parts, path_parts->len - 1);
    }

  ret = TRUE;
out:
  return ret;
}

static gboolean
selinux_relabel_dir (OstreeSysroot *sysroot, OstreeSePolicy *sepolicy, GFile *dir,
                     const char *prefix, GCancellable *cancellable, GError **error)
{

  g_autoptr (GFileInfo) root_info = g_file_query_info (
      dir, OSTREE_GIO_FAST_QUERYINFO, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
  if (!root_info)
    return FALSE;

  g_autoptr (GPtrArray) path_parts = g_ptr_array_new ();
  g_ptr_array_add (path_parts, (char *)prefix);
  if (!relabel_recursively (sysroot, sepolicy, dir, root_info, path_parts, cancellable, error))
    return glnx_prefix_error (error, "Relabeling /%s", prefix);

  return TRUE;
}

/* Handles SELinux labeling for /var; this is slated to be deleted.  See
 * https://github.com/ostreedev/ostree/pull/872
 */
static gboolean
selinux_relabel_var_if_needed (OstreeSysroot *sysroot, OstreeSePolicy *sepolicy, int os_deploy_dfd,
                               GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Relabeling /var", error);
  /* This is a bit of a hack; we should change the code at some
   * point in the distant future to only create (and label) /var
   * when doing a deployment.
   */
  const char selabeled[] = "var/.ostree-selabeled";
  struct stat stbuf;
  if (!glnx_fstatat_allow_noent (os_deploy_dfd, selabeled, &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  if (errno == ENOENT)
    {
      {
        g_autofree char *msg
            = g_strdup_printf ("Relabeling /var (no stamp file '%s' found)", selabeled);
        ot_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR,
                         SD_ID128_FORMAT_VAL (OSTREE_VARRELABEL_ID), "MESSAGE=%s", msg, NULL);
        _ostree_sysroot_emit_journal_msg (sysroot, msg);
      }

      g_autoptr (GFile) deployment_var_path = ot_fdrel_to_gfile (os_deploy_dfd, "var");
      if (!selinux_relabel_dir (sysroot, sepolicy, deployment_var_path, "var", cancellable, error))
        {
          g_prefix_error (error, "Relabeling /var: ");
          return FALSE;
        }

      {
        g_auto (OstreeSepolicyFsCreatecon) con = {
          0,
        };
        const char *selabeled_abspath = glnx_strjoina ("/", selabeled);

        if (!_ostree_sepolicy_preparefscreatecon (&con, sepolicy, selabeled_abspath, 0644, error))
          return FALSE;

        if (!glnx_file_replace_contents_at (os_deploy_dfd, selabeled, (guint8 *)"", 0,
                                            GLNX_FILE_REPLACE_DATASYNC_NEW, cancellable, error))
          return FALSE;
      }
    }

  return TRUE;
}

/* Handle initial creation of /etc in the deployment. See also
 * merge_configuration_from().
 */
static gboolean
prepare_deployment_etc (OstreeSysroot *sysroot, OstreeRepo *repo, OstreeDeployment *deployment,
                        int deployment_dfd, GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Preparing /etc", error);

  enum DirectoryState
  {
    DIRSTATE_NONEXISTENT,
    DIRSTATE_EMPTY,
    DIRSTATE_POPULATED,
  };

  enum DirectoryState etc_state;
  {
    gboolean exists = FALSE;
    g_auto (GLnxDirFdIterator) dfd_iter = {
      0,
    };
    if (!ot_dfd_iter_init_allow_noent (deployment_dfd, "etc", &dfd_iter, &exists, error))
      return glnx_prefix_error (error, "Failed to stat etc in deployment");
    if (!exists)
      {
        etc_state = DIRSTATE_NONEXISTENT;
      }
    else
      {
        struct dirent *dent;
        if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, NULL, error))
          return FALSE;
        if (dent)
          etc_state = DIRSTATE_POPULATED;
        else
          etc_state = DIRSTATE_EMPTY;
      }
  }
  struct stat stbuf;
  if (!glnx_fstatat_allow_noent (deployment_dfd, "usr/etc", &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  gboolean usretc_exists = (errno == 0);

  switch (etc_state)
    {
    case DIRSTATE_NONEXISTENT:
      break;
    case DIRSTATE_EMPTY:
      {
        if (usretc_exists)
          {
            /* For now it's actually simpler to just remove the empty directory
             * and have a symmetrical code path.
             */
            if (unlinkat (deployment_dfd, "etc", AT_REMOVEDIR) < 0)
              return glnx_throw_errno_prefix (error, "Failed to remove empty etc");
            etc_state = DIRSTATE_NONEXISTENT;
          }
        /* Otherwise, there's no /etc or /usr/etc, we'll assume they know what they're doing... */
      }
      break;
    case DIRSTATE_POPULATED:
      {
        if (usretc_exists)
          {
            return glnx_throw (error, "Tree contains both /etc and /usr/etc");
          }
        else
          {
            /* Compatibility hack */
            if (!glnx_renameat (deployment_dfd, "etc", deployment_dfd, "usr/etc", error))
              return FALSE;
            etc_state = DIRSTATE_NONEXISTENT;
            usretc_exists = TRUE;
          }
      }
      break;
    }

  if (usretc_exists)
    {
      g_assert (etc_state == DIRSTATE_NONEXISTENT);
      /* We need copies of /etc from /usr/etc (so admins can use vi), and if
       * SELinux is enabled, we need to relabel.
       */
      OstreeRepoCheckoutAtOptions etc_co_opts
          = { .force_copy = TRUE, .subpath = "/usr/etc", .sepolicy_prefix = "/etc" };

      /* Here, we initialize SELinux policy from the /usr/etc inside
       * the root - this is before we've finalized the configuration
       * merge into /etc. */
      g_autoptr (OstreeSePolicy) sepolicy
          = ostree_sepolicy_new_at (deployment_dfd, cancellable, error);
      if (!sepolicy)
        return FALSE;
      if (ostree_sepolicy_get_name (sepolicy) != NULL)
        etc_co_opts.sepolicy = sepolicy;

      /* Copy usr/etc → etc */
      if (!ostree_repo_checkout_at (repo, &etc_co_opts, deployment_dfd, "etc",
                                    ostree_deployment_get_csum (deployment), cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/* Write the origin file for a deployment; this does not bump the mtime, under
 * the assumption the caller may be writing multiple.
 */
static gboolean
write_origin_file_internal (OstreeSysroot *sysroot, OstreeSePolicy *sepolicy,
                            OstreeDeployment *deployment, GKeyFile *new_origin,
                            GLnxFileReplaceFlags flags, GCancellable *cancellable, GError **error)
{
  if (!_ostree_sysroot_ensure_writable (sysroot, error))
    return FALSE;

  GLNX_AUTO_PREFIX_ERROR ("Writing out origin file", error);
  GKeyFile *origin = new_origin ? new_origin : ostree_deployment_get_origin (deployment);

  if (origin)
    {
      g_auto (OstreeSepolicyFsCreatecon) con = {
        0,
      };
      if (!_ostree_sepolicy_preparefscreatecon (&con, sepolicy, "/etc/ostree/remotes.d/dummy.conf",
                                                0644, error))
        return FALSE;

      g_autofree char *origin_path = g_strdup_printf (
          "ostree/deploy/%s/deploy/%s.%d.origin", ostree_deployment_get_osname (deployment),
          ostree_deployment_get_csum (deployment), ostree_deployment_get_deployserial (deployment));

      gsize len;
      g_autofree char *contents = g_key_file_to_data (origin, &len, error);
      if (!contents)
        return FALSE;

      if (!glnx_file_replace_contents_at (sysroot->sysroot_fd, origin_path, (guint8 *)contents, len,
                                          flags, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/**
 * ostree_sysroot_write_origin_file:
 * @sysroot: System root
 * @deployment: Deployment
 * @new_origin: (allow-none): Origin content
 * @cancellable: Cancellable
 * @error: Error
 *
 * Immediately replace the origin file of the referenced @deployment
 * with the contents of @new_origin.  If @new_origin is %NULL,
 * this function will write the current origin of @deployment.
 */
gboolean
ostree_sysroot_write_origin_file (OstreeSysroot *sysroot, OstreeDeployment *deployment,
                                  GKeyFile *new_origin, GCancellable *cancellable, GError **error)
{
  g_autoptr (GFile) rootfs = g_file_new_for_path ("/");
  g_autoptr (OstreeSePolicy) sepolicy = ostree_sepolicy_new (rootfs, cancellable, error);
  if (!sepolicy)
    return FALSE;

  if (!write_origin_file_internal (sysroot, sepolicy, deployment, new_origin,
                                   GLNX_FILE_REPLACE_DATASYNC_NEW, cancellable, error))
    return FALSE;

  if (!_ostree_sysroot_bump_mtime (sysroot, error))
    return FALSE;

  return TRUE;
}

typedef struct
{
  int boot_dfd;
  char *kernel_srcpath;
  char *kernel_namever;
  char *kernel_hmac_srcpath;
  char *kernel_hmac_namever;
  char *initramfs_srcpath;
  char *initramfs_namever;
  char *devicetree_srcpath;
  char *devicetree_namever;
  char *aboot_srcpath;
  char *aboot_namever;
  char *bootcsum;
} OstreeKernelLayout;
static void
_ostree_kernel_layout_free (OstreeKernelLayout *layout)
{
  glnx_close_fd (&layout->boot_dfd);
  g_free (layout->kernel_srcpath);
  g_free (layout->kernel_namever);
  g_free (layout->kernel_hmac_srcpath);
  g_free (layout->kernel_hmac_namever);
  g_free (layout->initramfs_srcpath);
  g_free (layout->initramfs_namever);
  g_free (layout->devicetree_srcpath);
  g_free (layout->devicetree_namever);
  g_free (layout->aboot_srcpath);
  g_free (layout->aboot_namever);
  g_free (layout->bootcsum);
  g_free (layout);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeKernelLayout, _ostree_kernel_layout_free);

static OstreeKernelLayout *
_ostree_kernel_layout_new (void)
{
  OstreeKernelLayout *ret = g_new0 (OstreeKernelLayout, 1);
  ret->boot_dfd = -1;
  return ret;
}

/* See get_kernel_from_tree() below */
static gboolean
get_kernel_from_tree_usrlib_modules (OstreeSysroot *sysroot, int deployment_dfd,
                                     OstreeKernelLayout **out_layout, GCancellable *cancellable,
                                     GError **error)
{
  g_autofree char *kver = NULL;
  /* Look in usr/lib/modules */
  g_auto (GLnxDirFdIterator) mod_dfditer = {
    0,
  };
  gboolean exists;
  if (!ot_dfd_iter_init_allow_noent (deployment_dfd, "usr/lib/modules", &mod_dfditer, &exists,
                                     error))
    return FALSE;
  if (!exists)
    {
      /* No usr/lib/modules?  We're done */
      *out_layout = NULL;
      return TRUE;
    }

  g_autoptr (OstreeKernelLayout) ret_layout = _ostree_kernel_layout_new ();

  /* Reusable buffer for path string */
  g_autoptr (GString) pathbuf = g_string_new ("");
  /* Loop until we find something that looks like a valid /usr/lib/modules/$kver */
  while (ret_layout->boot_dfd == -1)
    {
      struct dirent *dent;
      struct stat stbuf;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&mod_dfditer, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;
      if (dent->d_type != DT_DIR)
        continue;

      /* It's a directory, look for /vmlinuz as a regular file */
      g_string_truncate (pathbuf, 0);
      g_string_append_printf (pathbuf, "%s/vmlinuz", dent->d_name);
      if (fstatat (mod_dfditer.fd, pathbuf->str, &stbuf, 0) < 0)
        {
          if (errno != ENOENT)
            return glnx_throw_errno_prefix (error, "fstatat(%s)", pathbuf->str);
          else
            continue;
        }
      else
        {
          /* Not a regular file? Loop again */
          if (!S_ISREG (stbuf.st_mode))
            continue;
        }

      /* Looks valid, this should exit the loop */
      if (!glnx_opendirat (mod_dfditer.fd, dent->d_name, FALSE, &ret_layout->boot_dfd, error))
        return FALSE;
      kver = g_strdup (dent->d_name);
      ret_layout->kernel_srcpath = g_strdup ("vmlinuz");
      ret_layout->kernel_namever = g_strdup_printf ("vmlinuz-%s", kver);
    }

  if (ret_layout->boot_dfd == -1)
    {
      *out_layout = NULL;
      /* No kernel found?  We're done. */
      return TRUE;
    }

  /* We found a module directory, compute the checksum */
  g_auto (OtChecksum) checksum = {
    0,
  };
  ot_checksum_init (&checksum);
  glnx_autofd int fd = -1;
  /* Checksum the kernel */
  if (!glnx_openat_rdonly (ret_layout->boot_dfd, "vmlinuz", TRUE, &fd, error))
    return FALSE;
  g_autoptr (GInputStream) in = g_unix_input_stream_new (fd, FALSE);
  if (!ot_gio_splice_update_checksum (NULL, in, &checksum, cancellable, error))
    return FALSE;
  g_clear_object (&in);
  glnx_close_fd (&fd);

  /* Look for an initramfs, but it's optional; since there wasn't any precedent
   * for this, let's be a bit conservative and support both `initramfs.img` and
   * `initramfs`.
   */
  const char *initramfs_paths[] = { "initramfs.img", "initramfs" };
  const char *initramfs_path = NULL;
  for (guint i = 0; i < G_N_ELEMENTS (initramfs_paths); i++)
    {
      initramfs_path = initramfs_paths[i];
      if (!ot_openat_ignore_enoent (ret_layout->boot_dfd, initramfs_path, &fd, error))
        return FALSE;
      if (fd != -1)
        break;
      else
        initramfs_path = NULL;
    }
  if (fd != -1)
    {
      g_assert (initramfs_path);
      ret_layout->initramfs_srcpath = g_strdup (initramfs_path);
      ret_layout->initramfs_namever = g_strdup_printf ("initramfs-%s.img", kver);
      in = g_unix_input_stream_new (fd, FALSE);
      if (!ot_gio_splice_update_checksum (NULL, in, &checksum, cancellable, error))
        return FALSE;
    }
  g_clear_object (&in);
  glnx_close_fd (&fd);

  /* look for a aboot.img file. */
  if (!ot_openat_ignore_enoent (ret_layout->boot_dfd, "aboot.img", &fd, error))
    return FALSE;

  if (fd != -1)
    {
      ret_layout->aboot_srcpath = g_strdup ("aboot.img");
      ret_layout->aboot_namever = g_strdup_printf ("aboot-%s.img", kver);
    }
  glnx_close_fd (&fd);

  /* look for a aboot.cfg file. */
  if (!ot_openat_ignore_enoent (ret_layout->boot_dfd, "aboot.cfg", &fd, error))
    return FALSE;

  /* Testing aid for https://github.com/ostreedev/ostree/issues/2154 */
  const gboolean no_dtb = (sysroot->debug_flags & OSTREE_SYSROOT_DEBUG_TEST_NO_DTB) > 0;
  if (!no_dtb)
    {
      /* Check for /usr/lib/modules/$kver/devicetree first, if it does not
       * exist check for /usr/lib/modules/$kver/dtb/ directory.
       */
      if (!ot_openat_ignore_enoent (ret_layout->boot_dfd, "devicetree", &fd, error))
        return FALSE;
      if (fd != -1)
        {
          ret_layout->devicetree_srcpath = g_strdup ("devicetree");
          ret_layout->devicetree_namever = g_strdup_printf ("devicetree-%s", kver);
          in = g_unix_input_stream_new (fd, FALSE);
          if (!ot_gio_splice_update_checksum (NULL, in, &checksum, cancellable, error))
            return FALSE;
        }
      else
        {
          struct stat stbuf;
          /* Check for dtb directory */
          if (!glnx_fstatat_allow_noent (ret_layout->boot_dfd, "dtb", &stbuf, 0, error))
            return FALSE;

          if (errno == 0 && S_ISDIR (stbuf.st_mode))
            {
              /* devicetree_namever set to NULL indicates a complete directory */
              ret_layout->devicetree_srcpath = g_strdup ("dtb");
              ret_layout->devicetree_namever = NULL;

              if (!checksum_dir_recurse (ret_layout->boot_dfd, "dtb", &checksum, cancellable,
                                         error))
                return FALSE;
            }
        }
    }
  g_clear_object (&in);
  glnx_close_fd (&fd);

  /* And finally, look for any HMAC file. This is needed for FIPS mode on some distros. */
  if (!glnx_fstatat_allow_noent (ret_layout->boot_dfd, ".vmlinuz.hmac", NULL, 0, error))
    return FALSE;
  if (errno == 0)
    {
      ret_layout->kernel_hmac_srcpath = g_strdup (".vmlinuz.hmac");
      /* Name it as dracut expects it:
       * https://github.com/dracutdevs/dracut/blob/225e4b94cbdb702cf512490dcd2ad9ca5f5b22c1/modules.d/01fips/fips.sh#L129
       */
      ret_layout->kernel_hmac_namever = g_strdup_printf (".%s.hmac", ret_layout->kernel_namever);
    }

  char hexdigest[OSTREE_SHA256_STRING_LEN + 1];
  ot_checksum_get_hexdigest (&checksum, hexdigest, sizeof (hexdigest));
  ret_layout->bootcsum = g_strdup (hexdigest);

  *out_layout = g_steal_pointer (&ret_layout);
  return TRUE;
}

/* See get_kernel_from_tree() below */
static gboolean
get_kernel_from_tree_legacy_layouts (int deployment_dfd, OstreeKernelLayout **out_layout,
                                     GCancellable *cancellable, GError **error)
{
  const char *legacy_paths[] = { "usr/lib/ostree-boot", "boot" };
  g_autofree char *kernel_checksum = NULL;
  g_autofree char *initramfs_checksum = NULL;
  g_autofree char *devicetree_checksum = NULL;
  g_autoptr (OstreeKernelLayout) ret_layout = _ostree_kernel_layout_new ();

  for (guint i = 0; i < G_N_ELEMENTS (legacy_paths); i++)
    {
      const char *path = legacy_paths[i];
      ret_layout->boot_dfd = glnx_opendirat_with_errno (deployment_dfd, path, TRUE);
      if (ret_layout->boot_dfd == -1)
        {
          if (errno != ENOENT)
            return glnx_throw_errno_prefix (error, "openat(%s)", path);
        }
      else
        break;
    }

  if (ret_layout->boot_dfd == -1)
    {
      /* No legacy found?  We're done */
      *out_layout = NULL;
      return TRUE;
    }

  /* ret_layout->boot_dfd will point to either /usr/lib/ostree-boot or /boot, let's
   * inspect it.
   */
  g_auto (GLnxDirFdIterator) dfditer = {
    0,
  };
  if (!glnx_dirfd_iterator_init_at (ret_layout->boot_dfd, ".", FALSE, &dfditer, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent (&dfditer, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      const char *name = dent->d_name;
      /* See if this is the kernel */
      if (ret_layout->kernel_srcpath == NULL && g_str_has_prefix (name, "vmlinuz-"))
        {
          const char *dash = strrchr (name, '-');
          g_assert (dash);
          /* In this version, we require that the tree builder generated a
           * sha256 of the kernel+initramfs and appended it to the file names.
           */
          if (ostree_validate_structureof_checksum_string (dash + 1, NULL))
            {
              kernel_checksum = g_strdup (dash + 1);
              ret_layout->kernel_srcpath = g_strdup (name);
              ret_layout->kernel_namever = g_strndup (name, dash - name);
            }
        }
      /* See if this is the initramfs  */
      else if (ret_layout->initramfs_srcpath == NULL && g_str_has_prefix (name, "initramfs-"))
        {
          const char *dash = strrchr (name, '-');
          g_assert (dash);
          if (ostree_validate_structureof_checksum_string (dash + 1, NULL))
            {
              initramfs_checksum = g_strdup (dash + 1);
              ret_layout->initramfs_srcpath = g_strdup (name);
              ret_layout->initramfs_namever = g_strndup (name, dash - name);
            }
        }
      /* See if this is the devicetree  */
      else if (ret_layout->devicetree_srcpath == NULL && g_str_has_prefix (name, "devicetree-"))
        {
          const char *dash = strrchr (name, '-');
          g_assert (dash);
          if (ostree_validate_structureof_checksum_string (dash + 1, NULL))
            {
              devicetree_checksum = g_strdup (dash + 1);
              ret_layout->devicetree_srcpath = g_strdup (name);
              ret_layout->devicetree_namever = g_strndup (name, dash - name);
            }
        }

      /* If we found a kernel, an initramfs and a devicetree, break out of the loop */
      if (ret_layout->kernel_srcpath != NULL && ret_layout->initramfs_srcpath != NULL
          && ret_layout->devicetree_srcpath != NULL)
        break;
    }

  /* No kernel found?  We're done */
  if (ret_layout->kernel_srcpath == NULL)
    {
      *out_layout = NULL;
      return TRUE;
    }

  /* The kernel/initramfs checksums must be the same */
  if (ret_layout->initramfs_srcpath != NULL)
    {
      g_assert (kernel_checksum != NULL);
      g_assert (initramfs_checksum != NULL);
      if (strcmp (kernel_checksum, initramfs_checksum) != 0)
        return glnx_throw (error, "Mismatched kernel checksum vs initrd");
    }

  /* The kernel/devicetree checksums must be the same */
  if (ret_layout->devicetree_srcpath != NULL)
    {
      g_assert (kernel_checksum != NULL);
      g_assert (devicetree_checksum != NULL);
      if (strcmp (kernel_checksum, devicetree_checksum) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "Mismatched kernel checksum vs device tree in tree");
          return FALSE;
        }
    }

  ret_layout->bootcsum = g_steal_pointer (&kernel_checksum);

  *out_layout = g_steal_pointer (&ret_layout);
  return TRUE;
}

/* Locate kernel/initramfs in the tree; the current standard is to look in
 * /usr/lib/modules/$kver/vmlinuz first.
 *
 * Originally OSTree defined kernels to be found underneath /boot
 * in the tree.  But that means when mounting /boot at runtime
 * we end up masking the content underneath, triggering a warning.
 *
 * For that reason, and also consistency with the "/usr defines the OS" model we
 * later switched to defining the in-tree kernels to be found under
 * /usr/lib/ostree-boot. But since then, Fedora at least switched to storing the
 * kernel in /usr/lib/modules, which makes sense and isn't ostree-specific, so
 * we prefer that now. However, the default Fedora layout doesn't put the
 * initramfs there, so we need to look in /usr/lib/ostree-boot first.
 */
static gboolean
get_kernel_from_tree (OstreeSysroot *sysroot, int deployment_dfd, OstreeKernelLayout **out_layout,
                      GCancellable *cancellable, GError **error)
{
  g_autoptr (OstreeKernelLayout) usrlib_modules_layout = NULL;
  g_autoptr (OstreeKernelLayout) legacy_layout = NULL;

  /* First, gather from usr/lib/modules/$kver if it exists */
  if (!get_kernel_from_tree_usrlib_modules (sysroot, deployment_dfd, &usrlib_modules_layout,
                                            cancellable, error))
    return FALSE;

  /* Gather the legacy layout */
  if (!get_kernel_from_tree_legacy_layouts (deployment_dfd, &legacy_layout, cancellable, error))
    return FALSE;

  /* Evaluate the state of both layouts.  If there's no legacy layout
   * If a legacy layout exists, and it has
   * an initramfs but the module layout doesn't, the legacy layout wins. This is
   * what happens with rpm-ostree with Fedora today, until rpm-ostree learns the
   * new layout.
   */
  if (legacy_layout == NULL)
    {
      /* No legacy layout, let's see if we have a module layout...*/
      if (usrlib_modules_layout == NULL)
        {
          /* Both layouts are not found?  Throw. */
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "Failed to find kernel in /usr/lib/modules, /usr/lib/ostree-boot or /boot");
          return FALSE;
        }
      else
        {
          /* No legacy, just usr/lib/modules?  We're done */
          *out_layout = g_steal_pointer (&usrlib_modules_layout);
          return TRUE;
        }
    }
  else if (usrlib_modules_layout != NULL && usrlib_modules_layout->initramfs_srcpath == NULL
           && legacy_layout->initramfs_srcpath != NULL)
    {
      /* Does the module path not have an initramfs, but the legacy does?  Prefer
       * the latter then, to make rpm-ostree work as is today.
       */
      *out_layout = g_steal_pointer (&legacy_layout);
      return TRUE;
    }
  /* Prefer module layout */
  else if (usrlib_modules_layout != NULL)
    {
      *out_layout = g_steal_pointer (&usrlib_modules_layout);
      return TRUE;
    }
  else
    {
      /* And finally fall back to legacy; we know one exists since we
       * checked first above.
       */
      g_assert (legacy_layout->kernel_srcpath);
      *out_layout = g_steal_pointer (&legacy_layout);
      return TRUE;
    }
}

/* We used to syncfs(), but that doesn't flush the journal on XFS,
 * and since GRUB2 can't read the XFS journal, the system
 * could fail to boot.
 *
 * http://marc.info/?l=linux-fsdevel&m=149520244919284&w=2
 * https://github.com/ostreedev/ostree/pull/1049
 */
static gboolean
fsfreeze_thaw_cycle (OstreeSysroot *self, int rootfs_dfd, GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("During fsfreeze-thaw", error);

  int sockpair[2];
  if (socketpair (AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockpair) < 0)
    return glnx_throw_errno_prefix (error, "socketpair");
  glnx_autofd int sock_parent = sockpair[0];
  glnx_autofd int sock_watchdog = sockpair[1];

  pid_t pid = fork ();
  if (pid < 0)
    return glnx_throw_errno_prefix (error, "fork");

  const gboolean debug_fifreeze = (self->debug_flags & OSTREE_SYSROOT_DEBUG_TEST_FIFREEZE) > 0;
  char c = '!';
  if (pid == 0) /* Child watchdog/unfreezer process. */
    {
      glnx_close_fd (&sock_parent);
      /* Daemonize, and mask SIGINT/SIGTERM, so we're likely to survive e.g.
       * someone doing a `systemctl restart rpm-ostreed` or a Ctrl-C of
       * `ostree admin upgrade`.  We don't daemonize though if testing so
       * that we can waitpid().
       */
      if (!debug_fifreeze)
        {
          if (daemon (0, 0) < 0)
            err (1, "daemon");
        }
      int sigs[] = { SIGINT, SIGTERM };
      for (guint i = 0; i < G_N_ELEMENTS (sigs); i++)
        {
          if (signal (sigs[i], SIG_IGN) == SIG_ERR)
            err (1, "signal");
        }
      /* Tell the parent we're ready */
      if (write (sock_watchdog, &c, sizeof (c)) != 1)
        err (1, "write");
      /* Wait for the parent to say it's going to freeze. */
      ssize_t bytes_read = TEMP_FAILURE_RETRY (read (sock_watchdog, &c, sizeof (c)));
      if (bytes_read < 0)
        err (1, "read");
      if (bytes_read != 1)
        errx (1, "failed to read from parent");
      /* Now we wait for the second message from the parent saying the freeze is
       * complete. We have a 30 second timeout; if somehow the parent hasn't
       * signaled completion, go ahead and unfreeze. But for debugging, just 1
       * second to avoid exessively lengthining the test suite.
       */
      const int timeout_ms = debug_fifreeze ? 1000 : 30000;
      struct pollfd pfds[1];
      pfds[0].fd = sock_watchdog;
      pfds[0].events = POLLIN | POLLHUP;
      int r = TEMP_FAILURE_RETRY (poll (pfds, 1, timeout_ms));
      /* Do a thaw if we hit an error, or if the poll timed out */
      if (r <= 0)
        {
          /* Ignore errors:
           * EINVAL: Not frozen
           * EPERM: For running the test suite as non-root
           * EOPNOTSUPP: If the filesystem doesn't support it
           */
          int saved_errno = errno;
          _ostree_linuxfs_filesystem_thaw (rootfs_dfd);
          errno = saved_errno;
          /* But if we got an error from poll, let's log it */
          if (r < 0)
            err (1, "poll");
        }
      if (debug_fifreeze)
        g_printerr ("fifreeze watchdog was run\n");
      /* We use _exit() rather than exit() to avoid tripping over any shared
       * libraries in process that aren't fork() safe; for example gjs/spidermonkey:
       * https://github.com/ostreedev/ostree/issues/1262
       * This doesn't help for the err()/errx() calls above, but eh...
       */
      _exit (EXIT_SUCCESS);
    }
  else /* Parent process. */
    {
      glnx_close_fd (&sock_watchdog);
      /* Wait for the watchdog to say it's set up; mainly that it's
       * masked SIGTERM successfully.
       */
      ssize_t bytes_read = TEMP_FAILURE_RETRY (read (sock_parent, &c, sizeof (c)));
      if (bytes_read < 0)
        return glnx_throw_errno_prefix (error, "read(watchdog init)");
      if (bytes_read != 1)
        return glnx_throw (error, "read(watchdog init)");
      /* And tell the watchdog that we're ready to start */
      if (write (sock_parent, &c, sizeof (c)) != sizeof (c))
        return glnx_throw_errno_prefix (error, "write(watchdog start)");
      /* Testing infrastructure */
      if (debug_fifreeze)
        {
          int wstatus;
          /* Ensure the child has written its data */
          if (TEMP_FAILURE_RETRY (waitpid (pid, &wstatus, 0)) < 0)
            return glnx_throw_errno_prefix (error, "waitpid(test-fifreeze)");
          if (!g_spawn_check_exit_status (wstatus, error))
            return glnx_prefix_error (error, "test-fifreeze: ");
          return glnx_throw (error, "aborting due to test-fifreeze");
        }
      /* Do a freeze/thaw cycle; TODO add a FIFREEZETHAW ioctl */
      if (_ostree_linuxfs_filesystem_freeze (rootfs_dfd) != 0)
        {
          /* Not supported, we're running in the unit tests (as non-root), or
           * the filesystem is already frozen (EBUSY).
           * OK, let's just do a syncfs.
           */
          if (G_IN_SET (errno, EOPNOTSUPP, ENOSYS, EPERM, EBUSY))
            {
              /* Warn if the filesystem was already frozen */
              if (errno == EBUSY)
                g_debug ("Filesystem already frozen, falling back to syncfs");
              if (TEMP_FAILURE_RETRY (syncfs (rootfs_dfd)) != 0)
                return glnx_throw_errno_prefix (error, "syncfs");
              /* Write the completion, and return */
              if (write (sock_parent, &c, sizeof (c)) != sizeof (c))
                return glnx_throw_errno_prefix (error, "write(watchdog syncfs complete)");
              return TRUE;
            }
          else
            return glnx_throw_errno_prefix (error, "ioctl(FIFREEZE)");
        }
      /* And finally thaw, then signal our completion to the watchdog */
      if (_ostree_linuxfs_filesystem_thaw (rootfs_dfd) != 0)
        {
          /* Warn but don't error if the filesystem was already thawed */
          if (errno == EINVAL)
            g_debug ("Filesystem already thawed");
          else
            return glnx_throw_errno_prefix (error, "ioctl(FITHAW)");
        }
      if (write (sock_parent, &c, sizeof (c)) != sizeof (c))
        return glnx_throw_errno_prefix (error, "write(watchdog FITHAW complete)");
    }
  return TRUE;
}

typedef struct
{
  guint64 root_syncfs_msec;
  guint64 boot_syncfs_msec;
} SyncStats;

/* First, sync the root directory as well as /var and /boot which may
 * be separate mount points.  Then *in addition*, do a global
 * `sync()`.
 */
static gboolean
full_system_sync (OstreeSysroot *self, SyncStats *out_stats, GCancellable *cancellable,
                  GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Full sync", error);
  ot_journal_print (LOG_INFO, "Starting syncfs() for system root");
  guint64 start_msec = g_get_monotonic_time () / 1000;
  if (syncfs (self->sysroot_fd) != 0)
    return glnx_throw_errno_prefix (error, "syncfs(sysroot)");
  guint64 end_msec = g_get_monotonic_time () / 1000;
  ot_journal_print (LOG_INFO, "Completed syncfs() for system root in %" G_GUINT64_FORMAT " ms",
                    end_msec - start_msec);

  out_stats->root_syncfs_msec = (end_msec - start_msec);

  if (!_ostree_sysroot_ensure_boot_fd (self, error))
    return FALSE;

  g_assert_cmpint (self->boot_fd, !=, -1);
  ot_journal_print (LOG_INFO, "Starting freeze/thaw cycle for system root");
  start_msec = g_get_monotonic_time () / 1000;
  if (!fsfreeze_thaw_cycle (self, self->boot_fd, cancellable, error))
    return FALSE;
  end_msec = g_get_monotonic_time () / 1000;
  ot_journal_print (LOG_INFO,
                    "Completed freeze/thaw cycle for system root in %" G_GUINT64_FORMAT " ms",
                    end_msec - start_msec);
  out_stats->boot_syncfs_msec = (end_msec - start_msec);

  return TRUE;
}

/* Write out the "bootlinks", which are symlinks pointing to deployments.
 * We might be generating a new bootversion (i.e. updating the bootloader config),
 * or we might just be generating a "sub-bootversion".
 *
 * These new links are made active by swap_bootlinks().
 */
static gboolean
create_new_bootlinks (OstreeSysroot *self, int bootversion, GPtrArray *new_deployments,
                      GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Creating new current bootlinks", error);
  glnx_autofd int ostree_dfd = -1;
  if (!glnx_opendirat (self->sysroot_fd, "ostree", TRUE, &ostree_dfd, error))
    return FALSE;

  int old_subbootversion;
  if (bootversion != self->bootversion)
    {
      if (!_ostree_sysroot_read_current_subbootversion (self, bootversion, &old_subbootversion,
                                                        cancellable, error))
        return FALSE;
    }
  else
    old_subbootversion = self->subbootversion;

  int new_subbootversion = old_subbootversion == 0 ? 1 : 0;

  /* Create the "subbootdir", which is a directory holding a symlink farm pointing to
   * deployments per-osname.
   */
  g_autofree char *ostree_subbootdir_name
      = g_strdup_printf ("boot.%d.%d", bootversion, new_subbootversion);
  if (!glnx_shutil_rm_rf_at (ostree_dfd, ostree_subbootdir_name, cancellable, error))
    return FALSE;
  if (!glnx_shutil_mkdir_p_at (ostree_dfd, ostree_subbootdir_name, 0755, cancellable, error))
    return FALSE;

  glnx_autofd int ostree_subbootdir_dfd = -1;
  if (!glnx_opendirat (ostree_dfd, ostree_subbootdir_name, FALSE, &ostree_subbootdir_dfd, error))
    return FALSE;

  for (guint i = 0; i < new_deployments->len; i++)
    {
      OstreeDeployment *deployment = new_deployments->pdata[i];
      g_autofree char *bootlink_parent
          = g_strconcat (ostree_deployment_get_osname (deployment), "/",
                         ostree_deployment_get_bootcsum (deployment), NULL);
      g_autofree char *bootlink_pathname = g_strdup_printf (
          "%s/%d", bootlink_parent, ostree_deployment_get_bootserial (deployment));
      g_autofree char *bootlink_target = g_strdup_printf (
          "../../../deploy/%s/deploy/%s.%d", ostree_deployment_get_osname (deployment),
          ostree_deployment_get_csum (deployment), ostree_deployment_get_deployserial (deployment));

      if (!glnx_shutil_mkdir_p_at (ostree_subbootdir_dfd, bootlink_parent, 0755, cancellable,
                                   error))
        return FALSE;

      if (!symlink_at_replace (bootlink_target, ostree_subbootdir_dfd, bootlink_pathname,
                               cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/* Rename into place symlinks created via create_new_bootlinks().
 */
static gboolean
swap_bootlinks (OstreeSysroot *self, int bootversion, GPtrArray *new_deployments,
                char **out_subbootdir, GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Swapping new version bootlinks", error);
  glnx_autofd int ostree_dfd = -1;
  if (!glnx_opendirat (self->sysroot_fd, "ostree", TRUE, &ostree_dfd, error))
    return FALSE;

  int old_subbootversion;
  if (bootversion != self->bootversion)
    {
      if (!_ostree_sysroot_read_current_subbootversion (self, bootversion, &old_subbootversion,
                                                        cancellable, error))
        return FALSE;
    }
  else
    old_subbootversion = self->subbootversion;

  int new_subbootversion = old_subbootversion == 0 ? 1 : 0;
  g_autofree char *ostree_bootdir_name = g_strdup_printf ("boot.%d", bootversion);
  g_autofree char *ostree_subbootdir_name
      = g_strdup_printf ("boot.%d.%d", bootversion, new_subbootversion);
  if (!symlink_at_replace (ostree_subbootdir_name, ostree_dfd, ostree_bootdir_name, cancellable,
                           error))
    return FALSE;
  if (out_subbootdir)
    *out_subbootdir = g_steal_pointer (&ostree_subbootdir_name);
  return TRUE;
}

static GHashTable *
parse_os_release (const char *contents, const char *split)
{
  g_autofree char **lines = g_strsplit (contents, split, -1);
  GHashTable *ret = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  for (char **iter = lines; *iter; iter++)
    {
      g_autofree char *line = *iter;

      if (g_str_has_prefix (line, "#"))
        continue;

      char *eq = strchr (line, '=');
      if (!eq)
        continue;

      *eq = '\0';
      const char *quotedval = eq + 1;
      char *val = g_shell_unquote (quotedval, NULL);
      if (!val)
        continue;

      g_hash_table_insert (ret, g_steal_pointer (&line), val);
    }

  return ret;
}

/* Given @deployment, prepare it to be booted; basically copying its
 * kernel/initramfs into /boot/ostree (if needed) and writing out an entry in
 * /boot/loader/entries.
 */
static gboolean
install_deployment_kernel (OstreeSysroot *sysroot, int new_bootversion,
                           OstreeDeployment *deployment, guint n_deployments, gboolean show_osname,
                           GCancellable *cancellable, GError **error)

{
  GLNX_AUTO_PREFIX_ERROR ("Installing kernel", error);
  OstreeBootconfigParser *bootconfig = ostree_deployment_get_bootconfig (deployment);
  g_autofree char *deployment_dirpath = ostree_sysroot_get_deployment_dirpath (sysroot, deployment);
  glnx_autofd int deployment_dfd = -1;
  if (!glnx_opendirat (sysroot->sysroot_fd, deployment_dirpath, FALSE, &deployment_dfd, error))
    return FALSE;

  /* We need to label the kernels */
  g_autoptr (OstreeSePolicy) sepolicy = ostree_sepolicy_new_at (deployment_dfd, cancellable, error);
  if (!sepolicy)
    return FALSE;

  /* Find the kernel/initramfs/devicetree in the tree */
  g_autoptr (OstreeKernelLayout) kernel_layout = NULL;
  if (!get_kernel_from_tree (sysroot, deployment_dfd, &kernel_layout, cancellable, error))
    return FALSE;

  if (!_ostree_sysroot_ensure_boot_fd (sysroot, error))
    return FALSE;

  const char *osname = ostree_deployment_get_osname (deployment);
  const char *bootcsum = ostree_deployment_get_bootcsum (deployment);
  g_autofree char *bootcsumdir = g_strdup_printf ("ostree/%s-%s", osname, bootcsum);
  g_autofree char *bootconfdir = g_strdup_printf ("loader.%d/entries", new_bootversion);
  g_autofree char *bootconf_name = NULL;
  guint index = n_deployments - ostree_deployment_get_index (deployment);
  // Allow opt-in to dropping the stateroot, because grub2 parses the *filename* and ignores
  // the version field.  xref https://github.com/ostreedev/ostree/issues/2961
  bool use_new_naming = (sysroot->opt_flags & OSTREE_SYSROOT_GLOBAL_OPT_BOOTLOADER_NAMING_2) > 0;
  if (use_new_naming)
    bootconf_name = g_strdup_printf ("ostree-%d.conf", index);
  else
    bootconf_name = g_strdup_printf ("ostree-%d-%s.conf", index, osname);
  if (!glnx_shutil_mkdir_p_at (sysroot->boot_fd, bootcsumdir, 0775, cancellable, error))
    return FALSE;

  glnx_autofd int bootcsum_dfd = -1;
  if (!glnx_opendirat (sysroot->boot_fd, bootcsumdir, TRUE, &bootcsum_dfd, error))
    return FALSE;

  if (!glnx_shutil_mkdir_p_at (sysroot->boot_fd, bootconfdir, 0775, cancellable, error))
    return FALSE;

  OstreeRepo *repo = ostree_sysroot_repo (sysroot);

  const char *bootprefix = repo->enable_bootprefix ? "/boot/" : "/";

  /* Install (hardlink/copy) the kernel into /boot/ostree/osname-${bootcsum} if
   * it doesn't exist already.
   */
  struct stat stbuf;
  if (!glnx_fstatat_allow_noent (bootcsum_dfd, kernel_layout->kernel_namever, &stbuf, 0, error))
    return FALSE;
  if (errno == ENOENT)
    {
      if (!install_into_boot (repo, sepolicy, kernel_layout->boot_dfd,
                              kernel_layout->kernel_srcpath, bootcsum_dfd,
                              kernel_layout->kernel_namever, cancellable, error))
        return FALSE;
    }

  /* If we have an initramfs, then install it into
   * /boot/ostree/osname-${bootcsum} if it doesn't exist already.
   */
  if (kernel_layout->initramfs_srcpath)
    {
      g_assert (kernel_layout->initramfs_namever);
      if (!glnx_fstatat_allow_noent (bootcsum_dfd, kernel_layout->initramfs_namever, &stbuf, 0,
                                     error))
        return FALSE;
      if (errno == ENOENT)
        {
          if (!install_into_boot (repo, sepolicy, kernel_layout->boot_dfd,
                                  kernel_layout->initramfs_srcpath, bootcsum_dfd,
                                  kernel_layout->initramfs_namever, cancellable, error))
            return FALSE;
        }
    }

  if (kernel_layout->devicetree_srcpath)
    {
      /* If devicetree_namever is set a single device tree is deployed */
      if (kernel_layout->devicetree_namever)
        {
          if (!glnx_fstatat_allow_noent (bootcsum_dfd, kernel_layout->devicetree_namever, &stbuf, 0,
                                         error))
            return FALSE;
          if (errno == ENOENT)
            {
              if (!install_into_boot (repo, sepolicy, kernel_layout->boot_dfd,
                                      kernel_layout->devicetree_srcpath, bootcsum_dfd,
                                      kernel_layout->devicetree_namever, cancellable, error))
                return FALSE;
            }
        }
      else
        {
          if (!copy_dir_recurse (kernel_layout->boot_dfd, bootcsum_dfd,
                                 kernel_layout->devicetree_srcpath, sysroot->debug_flags,
                                 cancellable, error))
            return FALSE;
        }
    }

  if (kernel_layout->kernel_hmac_srcpath)
    {
      if (!glnx_fstatat_allow_noent (bootcsum_dfd, kernel_layout->kernel_hmac_namever, &stbuf, 0,
                                     error))
        return FALSE;
      if (errno == ENOENT)
        {
          if (!install_into_boot (repo, sepolicy, kernel_layout->boot_dfd,
                                  kernel_layout->kernel_hmac_srcpath, bootcsum_dfd,
                                  kernel_layout->kernel_hmac_namever, cancellable, error))
            return FALSE;
        }
    }

  if (kernel_layout->aboot_srcpath)
    {
      g_assert (kernel_layout->aboot_namever);
      if (!glnx_fstatat_allow_noent (bootcsum_dfd, kernel_layout->aboot_namever, &stbuf, 0, error))
        return FALSE;

      if (errno == ENOENT)
        {
          if (!install_into_boot (repo, sepolicy, kernel_layout->boot_dfd,
                                  kernel_layout->aboot_srcpath, bootcsum_dfd,
                                  kernel_layout->aboot_namever, cancellable, error))
            return FALSE;
        }
    }

  /* NOTE: if adding more things in bootcsum_dfd, also update get_kernel_layout_size() */

  g_autoptr (GPtrArray) overlay_initrds = NULL;
  for (char **it = _ostree_deployment_get_overlay_initrds (deployment); it && *it; it++)
    {
      char *checksum = *it;

      /* Overlay initrds are not part of the bootcsum dir; they're not part of the tree
       * proper. Instead they're in /boot/ostree/initramfs-overlays/ named by their csum.
       * Doing it this way allows sharing the same bootcsum dir for multiple deployments
       * with the only change being in overlay initrds (or conversely, the same overlay
       * across different boocsums). Eventually, it'd be nice to have an OSTree repo in
       * /boot itself and drop the boocsum dir concept entirely. */

      g_autofree char *destpath = g_strdup_printf (
          "%s%s/%s.img", bootprefix, _OSTREE_SYSROOT_BOOT_INITRAMFS_OVERLAYS, checksum);
      const char *rel_destpath = destpath + 1;

      /* lazily allocate array and create dir so we don't pollute /boot if not needed */
      if (overlay_initrds == NULL)
        {
          overlay_initrds = g_ptr_array_new_with_free_func (g_free);

          if (!glnx_shutil_mkdir_p_at (sysroot->boot_fd, _OSTREE_SYSROOT_BOOT_INITRAMFS_OVERLAYS,
                                       0755, cancellable, error))
            return FALSE;
        }

      if (!glnx_fstatat_allow_noent (sysroot->boot_fd, rel_destpath, NULL, 0, error))
        return FALSE;
      if (errno == ENOENT)
        {
          g_autofree char *srcpath
              = g_strdup_printf (_OSTREE_SYSROOT_RUNSTATE_STAGED_INITRDS_DIR "/%s", checksum);
          if (!install_into_boot (repo, sepolicy, AT_FDCWD, srcpath, sysroot->boot_fd, rel_destpath,
                                  cancellable, error))
            return FALSE;
        }

      /* these are used lower down to populate the bootconfig */
      g_ptr_array_add (overlay_initrds, g_steal_pointer (&destpath));
    }

  g_autofree char *contents = NULL;
  if (!glnx_fstatat_allow_noent (deployment_dfd, "usr/lib/os-release", &stbuf, 0, error))
    return FALSE;
  if (errno == ENOENT)
    {
      contents = glnx_file_get_contents_utf8_at (deployment_dfd, "etc/os-release", NULL,
                                                 cancellable, error);
      if (!contents)
        return glnx_prefix_error (error, "Reading /etc/os-release");
    }
  else
    {
      contents = glnx_file_get_contents_utf8_at (deployment_dfd, "usr/lib/os-release", NULL,
                                                 cancellable, error);
      if (!contents)
        return glnx_prefix_error (error, "Reading /usr/lib/os-release");
    }

  g_autoptr (GHashTable) osrelease_values = parse_os_release (contents, "\n");
  /* title */
  const char *val = g_hash_table_lookup (osrelease_values, "PRETTY_NAME");
  if (val == NULL)
    val = g_hash_table_lookup (osrelease_values, "ID");
  if (val == NULL)
    return glnx_throw (error, "No PRETTY_NAME or ID in /etc/os-release");

  g_autofree char *deployment_version = NULL;
  if (repo)
    {
      /* Try extracting a version for this deployment. */
      const char *csum = ostree_deployment_get_csum (deployment);
      g_autoptr (GVariant) variant = NULL;
      g_autoptr (GVariant) metadata = NULL;

      /* XXX Copying ot_admin_checksum_version() + bits from
       *     ot-admin-builtin-status.c.  Maybe this should be
       *     public API in libostree? */
      if (ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, csum, &variant, NULL))
        {
          metadata = g_variant_get_child_value (variant, 0);
          (void)g_variant_lookup (metadata, OSTREE_COMMIT_META_KEY_VERSION, "s",
                                  &deployment_version);
        }
    }

  /* XXX The SYSLINUX bootloader backend actually parses the title string
   *     (specifically, it looks for the substring "(ostree"), so further
   *     changes to the title format may require updating that backend. */
  g_autoptr (GString) title_key = g_string_new (val);
  if (deployment_version && *deployment_version && !strstr (val, deployment_version))
    {
      g_string_append_c (title_key, ' ');
      g_string_append (title_key, deployment_version);
    }
  g_string_append (title_key, " (ostree");
  if (show_osname)
    {
      g_string_append_c (title_key, ':');
      g_string_append (title_key, osname);
    }

  g_string_append_printf (title_key, ":%d", ostree_deployment_get_index (deployment));

  g_string_append_c (title_key, ')');
  ostree_bootconfig_parser_set (bootconfig, "title", title_key->str);

  g_autofree char *version_key
      = g_strdup_printf ("%d", n_deployments - ostree_deployment_get_index (deployment));
  ostree_bootconfig_parser_set (bootconfig, OSTREE_COMMIT_META_KEY_VERSION, version_key);
  g_autofree char *boot_relpath
      = g_strconcat (bootprefix, bootcsumdir, "/", kernel_layout->kernel_namever, NULL);
  ostree_bootconfig_parser_set (bootconfig, "linux", boot_relpath);

  val = ostree_bootconfig_parser_get (bootconfig, "options");
  g_autoptr (OstreeKernelArgs) kargs = ostree_kernel_args_from_string (val);

  if (kernel_layout->initramfs_namever)
    {
      g_autofree char *initrd_boot_relpath
          = g_strconcat (bootprefix, bootcsumdir, "/", kernel_layout->initramfs_namever, NULL);
      ostree_bootconfig_parser_set (bootconfig, "initrd", initrd_boot_relpath);

      if (overlay_initrds)
        {
          g_ptr_array_add (overlay_initrds, NULL);
          ostree_bootconfig_parser_set_overlay_initrds (bootconfig,
                                                        (char **)overlay_initrds->pdata);
        }
    }
  else
    {
      g_autofree char *prepare_root_arg = NULL;
      prepare_root_arg = g_strdup_printf (
          "init=/ostree/boot.%d/%s/%s/%d/usr/lib/ostree/ostree-prepare-root", new_bootversion,
          osname, bootcsum, ostree_deployment_get_bootserial (deployment));
      ostree_kernel_args_replace_take (kargs, g_steal_pointer (&prepare_root_arg));
    }

  const char *aboot_fn = NULL;
  if (kernel_layout->aboot_namever)
    {
      aboot_fn = kernel_layout->aboot_namever;
    }
  else if (kernel_layout->aboot_srcpath)
    {
      aboot_fn = kernel_layout->aboot_srcpath;
    }

  if (aboot_fn)
    {
      g_autofree char *aboot_relpath = g_strconcat ("/", bootcsumdir, "/", aboot_fn, NULL);
      ostree_bootconfig_parser_set (bootconfig, "aboot", aboot_relpath);
    }
  else
    {
      g_autofree char *aboot_relpath
          = g_strconcat ("/", deployment_dirpath, "/usr/lib/ostree-boot/aboot.img", NULL);
      ostree_bootconfig_parser_set (bootconfig, "aboot", aboot_relpath);
    }

  g_autofree char *abootcfg_relpath
      = g_strconcat ("/", deployment_dirpath, "/usr/lib/ostree-boot/aboot.cfg", NULL);
  ostree_bootconfig_parser_set (bootconfig, "abootcfg", abootcfg_relpath);

  if (kernel_layout->devicetree_namever)
    {
      g_autofree char *dt_boot_relpath
          = g_strconcat (bootprefix, bootcsumdir, "/", kernel_layout->devicetree_namever, NULL);
      ostree_bootconfig_parser_set (bootconfig, "devicetree", dt_boot_relpath);
    }
  else if (kernel_layout->devicetree_srcpath)
    {
      /* If devicetree_srcpath is set but devicetree_namever is NULL, then we
       * want to point to a whole directory of device trees.
       * See: https://github.com/ostreedev/ostree/issues/1900
       */
      g_autofree char *dt_boot_relpath
          = g_strconcat (bootprefix, bootcsumdir, "/", kernel_layout->devicetree_srcpath, NULL);
      ostree_bootconfig_parser_set (bootconfig, "fdtdir", dt_boot_relpath);
    }

  /* Note this is parsed in ostree-impl-system-generator.c */
  g_autofree char *ostree_kernel_arg
      = g_strdup_printf ("ostree=/ostree/boot.%d/%s/%s/%d", new_bootversion, osname, bootcsum,
                         ostree_deployment_get_bootserial (deployment));
  ostree_kernel_args_replace_take (kargs, g_steal_pointer (&ostree_kernel_arg));

  g_autofree char *options_key = ostree_kernel_args_to_string (kargs);
  ostree_bootconfig_parser_set (bootconfig, "options", options_key);

  /* Only append to this BLS config if:
   * - this is not the default deployment
   */
  /* If deployment was prepended, it is the new default */
  gboolean is_new_default = (ostree_deployment_get_index (deployment) == 0);
  gboolean allow_append = !is_new_default;
  if (allow_append)
    {
      /* get all key value pairs in bls-append */
      GLNX_HASH_TABLE_FOREACH_KV (repo->bls_append_values, const char *, key, const char *, value)
        ostree_bootconfig_parser_set (bootconfig, key, value);
    }

  glnx_autofd int bootconf_dfd = -1;
  if (!glnx_opendirat (sysroot->boot_fd, bootconfdir, TRUE, &bootconf_dfd, error))
    return FALSE;

  if (!ostree_bootconfig_parser_write_at (ostree_deployment_get_bootconfig (deployment),
                                          bootconf_dfd, bootconf_name, cancellable, error))
    return FALSE;

  return TRUE;
}

/* We generate the symlink on disk, then potentially do a syncfs() to ensure
 * that it (and everything else we wrote) has hit disk. Only after that do we
 * rename it into place.
 */
static gboolean
prepare_new_bootloader_link (OstreeSysroot *sysroot, int current_bootversion, int new_bootversion,
                             GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Preparing final bootloader swap", error);
  g_assert ((current_bootversion == 0 && new_bootversion == 1)
            || (current_bootversion == 1 && new_bootversion == 0));

  /* This allows us to support both /boot on a seperate filesystem to / as well
   * as on the same filesystem. */
  if (TEMP_FAILURE_RETRY (symlinkat (".", sysroot->sysroot_fd, "boot/boot")) < 0)
    if (errno != EEXIST)
      return glnx_throw_errno_prefix (error, "symlinkat");

  g_autofree char *new_target = g_strdup_printf ("loader.%d", new_bootversion);

  /* We shouldn't actually need to replace but it's easier to reuse
     that code */
  if (!symlink_at_replace (new_target, sysroot->sysroot_fd, "boot/loader.tmp", cancellable, error))
    return FALSE;

  return TRUE;
}

/* Update the /boot/loader symlink to point to /boot/loader.$new_bootversion */
static gboolean
swap_bootloader (OstreeSysroot *sysroot, OstreeBootloader *bootloader, int current_bootversion,
                 int new_bootversion, GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Final bootloader swap", error);

  g_assert ((current_bootversion == 0 && new_bootversion == 1)
            || (current_bootversion == 1 && new_bootversion == 0));

  if (!_ostree_sysroot_ensure_boot_fd (sysroot, error))
    return FALSE;

  /* The symlink was already written, and we used syncfs() to ensure
   * its data is in place.  Renaming now should give us atomic semantics;
   * see https://bugzilla.gnome.org/show_bug.cgi?id=755595
   */
  if (!glnx_renameat (sysroot->boot_fd, "loader.tmp", sysroot->boot_fd, "loader", error))
    return FALSE;

  /* Now we explicitly fsync this directory, even though it
   * isn't required for atomicity, for two reasons:
   *  - It should be very cheap as we're just syncing whatever
   *    data was written since the last sync which was hopefully
   *    less than a second ago.
   *  - It should be sync'd before shutdown as that could crash
   *    for whatever reason, and we wouldn't want to confuse the
   *    admin by going back to the previous session.
   */
  if (fsync (sysroot->boot_fd) != 0)
    return glnx_throw_errno_prefix (error, "fsync(boot)");

  /* TODO: In the future also execute this automatically via a systemd unit
   * if we detect it's necessary.
   **/
  if (bootloader)
    {
      if (!_ostree_bootloader_post_bls_sync (bootloader, new_bootversion, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/* Deployments may share boot checksums; the bootserial indexes them
 * per-bootchecksum. It's used by the symbolic links after the bootloader.
 */
static void
assign_bootserials (GPtrArray *deployments)
{
  g_autoptr (GHashTable) serials = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);

  for (guint i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      const char *bootcsum = ostree_deployment_get_bootcsum (deployment);
      /* Note that not-found maps to NULL which converts to zero */
      guint count = GPOINTER_TO_UINT (g_hash_table_lookup (serials, bootcsum));
      g_hash_table_replace (serials, (char *)bootcsum, GUINT_TO_POINTER (count + 1));

      ostree_deployment_set_bootserial (deployment, count);
    }
}

static char *
get_deployment_nonostree_kargs (OstreeDeployment *deployment)
{
  /* pick up kernel arguments but filter out ostree= */
  OstreeBootconfigParser *bootconfig = ostree_deployment_get_bootconfig (deployment);
  const char *boot_options = ostree_bootconfig_parser_get (bootconfig, "options");
  g_autoptr (OstreeKernelArgs) kargs = ostree_kernel_args_from_string (boot_options);
  ostree_kernel_args_replace (kargs, "ostree");
  return ostree_kernel_args_to_string (kargs);
}

static char *
get_deployment_ostree_version (OstreeRepo *repo, OstreeDeployment *deployment)
{
  const char *csum = ostree_deployment_get_csum (deployment);

  g_autofree char *version = NULL;
  g_autoptr (GVariant) variant = NULL;
  if (ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, csum, &variant, NULL))
    {
      g_autoptr (GVariant) metadata = g_variant_get_child_value (variant, 0);
      (void)g_variant_lookup (metadata, OSTREE_COMMIT_META_KEY_VERSION, "s", &version);
    }

  return g_steal_pointer (&version);
}

/* OSTree implements a special optimization where we want to avoid touching
 * the bootloader configuration if the kernel layout hasn't changed.  This is
 * handled by the ostree= kernel argument referring to a "bootlink".  But
 * we *do* need to update the bootloader configuration if the kernel arguments
 * change.
 *
 * Hence, this function determines if @a and @b are fully compatible from a
 * bootloader perspective.
 */
static gboolean
deployment_bootconfigs_equal (OstreeRepo *repo, OstreeDeployment *a, OstreeDeployment *b)
{
  /* same kernel & initramfs? */
  const char *a_bootcsum = ostree_deployment_get_bootcsum (a);
  const char *b_bootcsum = ostree_deployment_get_bootcsum (b);
  if (strcmp (a_bootcsum, b_bootcsum) != 0)
    return FALSE;

  /* same initrd overlays? */
  if (g_strcmp0 (a->overlay_initrds_id, b->overlay_initrds_id) != 0)
    return FALSE;

  /* same kargs? */
  g_autofree char *a_boot_options_without_ostree = get_deployment_nonostree_kargs (a);
  g_autofree char *b_boot_options_without_ostree = get_deployment_nonostree_kargs (b);
  if (strcmp (a_boot_options_without_ostree, b_boot_options_without_ostree) != 0)
    return FALSE;

  /* same ostree version? this is just for the menutitle, we won't have to cp the kernel */
  g_autofree char *a_version = get_deployment_ostree_version (repo, a);
  g_autofree char *b_version = get_deployment_ostree_version (repo, b);
  if (g_strcmp0 (a_version, b_version) != 0)
    return FALSE;

  return TRUE;
}

/* This used to be a temporary hack to create "current" symbolic link
 * that's easy to follow inside the gnome-ostree build scripts (now
 * gnome-continuous).  It wasn't atomic, and nowadays people can use
 * the OSTree API to find deployments.
 */
static gboolean
cleanup_legacy_current_symlinks (OstreeSysroot *self, GCancellable *cancellable, GError **error)
{
  g_autoptr (GString) buf = g_string_new ("");

  for (guint i = 0; i < self->deployments->len; i++)
    {
      OstreeDeployment *deployment = self->deployments->pdata[i];
      const char *osname = ostree_deployment_get_osname (deployment);

      g_string_truncate (buf, 0);
      g_string_append_printf (buf, "ostree/deploy/%s/current", osname);

      if (!ot_ensure_unlinked_at (self->sysroot_fd, buf->str, error))
        return FALSE;
    }

  return TRUE;
}

/**
 * ostree_sysroot_write_deployments:
 * @self: Sysroot
 * @new_deployments: (element-type OstreeDeployment): List of new deployments
 * @cancellable: Cancellable
 * @error: Error
 *
 * Older version of ostree_sysroot_write_deployments_with_options(). This
 * version will perform post-deployment cleanup by default.
 */
gboolean
ostree_sysroot_write_deployments (OstreeSysroot *self, GPtrArray *new_deployments,
                                  GCancellable *cancellable, GError **error)
{
  OstreeSysrootWriteDeploymentsOpts opts = { .do_postclean = TRUE };
  return ostree_sysroot_write_deployments_with_options (self, new_deployments, &opts, cancellable,
                                                        error);
}

/* Handle writing out a new bootloader config. One reason this needs to be a
 * helper function is to handle wrapping it with temporarily remounting /boot
 * rw.
 */
static gboolean
write_deployments_bootswap (OstreeSysroot *self, GPtrArray *new_deployments,
                            OstreeSysrootWriteDeploymentsOpts *opts, OstreeBootloader *bootloader,
                            SyncStats *out_syncstats, char **out_subbootdir,
                            GCancellable *cancellable, GError **error)
{
  const int new_bootversion = self->bootversion ? 0 : 1;

  g_autofree char *new_loader_entries_dir
      = g_strdup_printf ("boot/loader.%d/entries", new_bootversion);
  if (!glnx_shutil_rm_rf_at (self->sysroot_fd, new_loader_entries_dir, cancellable, error))
    return FALSE;
  if (!glnx_shutil_mkdir_p_at (self->sysroot_fd, new_loader_entries_dir, 0755, cancellable, error))
    return FALSE;

  /* Only show the osname in bootloader titles if there are multiple
   * osname's among the new deployments.  Check for that here. */
  gboolean show_osname = FALSE;
  for (guint i = 1; i < new_deployments->len; i++)
    {
      const char *osname_0 = ostree_deployment_get_osname (new_deployments->pdata[0]);
      const char *osname_i = ostree_deployment_get_osname (new_deployments->pdata[i]);
      if (!g_str_equal (osname_0, osname_i))
        {
          show_osname = TRUE;
          break;
        }
    }

  for (guint i = 0; i < new_deployments->len; i++)
    {
      OstreeDeployment *deployment = new_deployments->pdata[i];
      if (!install_deployment_kernel (self, new_bootversion, deployment, new_deployments->len,
                                      show_osname, cancellable, error))
        return FALSE;
    }

  /* Create and swap bootlinks for *new* version */
  if (!create_new_bootlinks (self, new_bootversion, new_deployments, cancellable, error))
    return FALSE;
  g_autofree char *new_subbootdir = NULL;
  if (!swap_bootlinks (self, new_bootversion, new_deployments, &new_subbootdir, cancellable, error))
    return FALSE;

  g_debug ("Using bootloader: %s",
           bootloader ? g_type_name (G_TYPE_FROM_INSTANCE (bootloader)) : "(none)");

  if (bootloader)
    {
      if (!_ostree_bootloader_write_config (bootloader, new_bootversion, new_deployments,
                                            cancellable, error))
        return glnx_prefix_error (error, "Bootloader write config");
    }

  if (!prepare_new_bootloader_link (self, self->bootversion, new_bootversion, cancellable, error))
    return FALSE;

  if (!full_system_sync (self, out_syncstats, cancellable, error))
    return FALSE;

  if (!swap_bootloader (self, bootloader, self->bootversion, new_bootversion, cancellable, error))
    return FALSE;

  if (out_subbootdir)
    *out_subbootdir = g_steal_pointer (&new_subbootdir);
  return TRUE;
}

/* Actions taken after writing deployments is complete */
static gboolean
write_deployments_finish (OstreeSysroot *self, GCancellable *cancellable, GError **error)
{
  if (!_ostree_sysroot_bump_mtime (self, error))
    return FALSE;

  /* Now reload from disk */
  if (!ostree_sysroot_load (self, cancellable, error))
    return glnx_prefix_error (error, "Reloading deployments after commit");

  if (!cleanup_legacy_current_symlinks (self, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
add_file_size_if_nonnull (int dfd, const char *path, guint64 *inout_size, GError **error)
{
  if (path == NULL)
    return TRUE;

  struct stat stbuf;
  if (!glnx_fstatat (dfd, path, &stbuf, 0, error))
    return FALSE;

  *inout_size += stbuf.st_size;
  return TRUE;
}

/* calculates the total size of the bootcsum dir in /boot after we would copy
 * it. This reflects the logic in  install_deployment_kernel(). */
static gboolean
get_kernel_layout_size (OstreeSysroot *self, OstreeDeployment *deployment, guint64 *out_size,
                        GCancellable *cancellable, GError **error)
{
  g_autofree char *deployment_dirpath = ostree_sysroot_get_deployment_dirpath (self, deployment);
  glnx_autofd int deployment_dfd = -1;
  if (!glnx_opendirat (self->sysroot_fd, deployment_dirpath, FALSE, &deployment_dfd, error))
    return FALSE;

  g_autoptr (OstreeKernelLayout) kernel_layout = NULL;
  if (!get_kernel_from_tree (self, deployment_dfd, &kernel_layout, cancellable, error))
    return FALSE;

  guint64 bootdir_size = 0;
  if (!add_file_size_if_nonnull (kernel_layout->boot_dfd, kernel_layout->kernel_srcpath,
                                 &bootdir_size, error))
    return FALSE;
  if (!add_file_size_if_nonnull (kernel_layout->boot_dfd, kernel_layout->initramfs_srcpath,
                                 &bootdir_size, error))
    return FALSE;
  if (kernel_layout->devicetree_srcpath)
    {
      /* These conditionals mirror the logic in install_deployment_kernel(). */
      if (kernel_layout->devicetree_namever)
        {
          if (!add_file_size_if_nonnull (kernel_layout->boot_dfd, kernel_layout->devicetree_srcpath,
                                         &bootdir_size, error))
            return FALSE;
        }
      else
        {
          guint64 dirsize = 0;
          if (!ot_get_dir_size (kernel_layout->boot_dfd, kernel_layout->devicetree_srcpath,
                                &dirsize, cancellable, error))
            return FALSE;
          bootdir_size += dirsize;
        }
    }
  if (!add_file_size_if_nonnull (kernel_layout->boot_dfd, kernel_layout->kernel_hmac_srcpath,
                                 &bootdir_size, error))
    return FALSE;
  if (!add_file_size_if_nonnull (kernel_layout->boot_dfd, kernel_layout->aboot_srcpath,
                                 &bootdir_size, error))
    return FALSE;

  *out_size = bootdir_size;
  return TRUE;
}

/* This is a roundabout but more trustworthy way of doing a space check than
 * relying on statvfs's f_bfree when you know the size of the objects. */
static gboolean
dfd_fallocate_check (int dfd, off_t len, gboolean *out_passed, GError **error)
{
  /* If the requested size is 0 then return early. Passing a 0 len to
   * fallocate results in EINVAL */
  if (len == 0)
    {
      *out_passed = TRUE;
      return TRUE;
    }

  g_auto (GLnxTmpfile) tmpf = {
    0,
  };
  if (!glnx_open_tmpfile_linkable_at (dfd, ".", O_WRONLY | O_CLOEXEC, &tmpf, error))
    return FALSE;

  *out_passed = TRUE;
  /* There's glnx_try_fallocate, but not with the same error semantics. */
  if (TEMP_FAILURE_RETRY (fallocate (tmpf.fd, 0, 0, len)) < 0)
    {
      if (G_IN_SET (errno, ENOSYS, EOPNOTSUPP))
        return TRUE;
      else if (errno != ENOSPC)
        return glnx_throw_errno_prefix (error, "fallocate");
      *out_passed = FALSE;
    }
  return TRUE;
}

/* Analyze /boot and figure out if the new deployments won't fit in the
 * remaining space. If they won't, check if deleting the deployments that are
 * getting rotated out (e.g. the current rollback) would free up sufficient
 * space. If so, call ostree_sysroot_write_deployments() to delete them. */
static gboolean
auto_early_prune_old_deployments (OstreeSysroot *self, GPtrArray *new_deployments,
                                  GCancellable *cancellable, GError **error)
{
  /* If we're not booted into a deployment, then this is some kind of e.g. disk
   * creation/provisioning. The situation isn't as dire, so let's not resort to
   * auto-pruning and instead let possible ENOSPC errors naturally bubble. */
  if (self->booted_deployment == NULL)
    return TRUE;

  {
    struct stat stbuf;
    if (!glnx_fstatat (self->boot_fd, ".", &stbuf, 0, error))
      return FALSE;

    /* if /boot is on the same filesystem as the sysroot (which must be where
     * the sysroot repo is), don't do anything */
    if (stbuf.st_dev == self->repo->device)
      return TRUE;
  }

  /* pre-emptive cleanup of any cruft in /boot to free up any wasted space */
  if (!_ostree_sysroot_cleanup_bootfs (self, cancellable, error))
    return FALSE;

  /* tracks all the bootcsums currently in /boot */
  g_autoptr (GHashTable) current_bootcsums
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* tracks all the bootcsums of new_deployments */
  g_autoptr (GHashTable) new_bootcsums
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  g_auto (GStrv) bootdirs = NULL;
  if (!_ostree_sysroot_list_all_boot_directories (self, &bootdirs, cancellable, error))
    return glnx_prefix_error (error, "listing bootcsum directories in bootfs");

  for (char **it = bootdirs; it && *it; it++)
    {
      const char *bootdir = *it;

      g_autofree char *bootcsum = NULL;
      if (!_ostree_sysroot_parse_bootdir_name (bootdir, NULL, &bootcsum))
        g_assert_not_reached (); /* checked in _ostree_sysroot_list_all_boot_directories() */

      guint64 bootdir_size;
      g_autofree char *ostree_bootdir = g_build_filename ("ostree", bootdir, NULL);
      if (!ot_get_dir_size (self->boot_fd, ostree_bootdir, &bootdir_size, cancellable, error))
        return FALSE;

      /* for our purposes of sizing bootcsums, it's highly unlikely we need a
       * guint64; cast it down to guint so we can more easily store it */
      if (bootdir_size > G_MAXUINT)
        {
          /* If it somehow happens, don't make it fatal. this is all an
           * optimization anyway, so let the deployment continue. But log it so
           * that users report it and we tweak this code to handle this.
           *
           * An alternative is working with the block size instead, which would
           * be easier to handle. But ideally, `ot_get_dir_size` would be block
           * size aware too for better accuracy, which is awkward since the
           * function itself is generic over directories and doesn't consider
           * e.g. mount points from different filesystems. */
          g_printerr ("bootcsum %s size exceeds %u; disabling auto-prune optimization\n", bootdir,
                      G_MAXUINT);
          return TRUE;
        }

      g_assert_cmpuint (bootdir_size, >, 0);
      g_hash_table_insert (current_bootcsums, g_steal_pointer (&bootcsum),
                           GUINT_TO_POINTER (bootdir_size));
    }

  /* total size of all bootcsums dirs that aren't already in /boot */
  guint64 net_new_bootcsum_dirs_total_size = 0;

  /* now gather all the bootcsums of the new deployments */
  for (guint i = 0; i < new_deployments->len; i++)
    {
      OstreeDeployment *deployment = new_deployments->pdata[i];

      const char *bootcsum = ostree_deployment_get_bootcsum (deployment);
      gpointer bootdir_sizep = g_hash_table_lookup (current_bootcsums, bootcsum);
      if (bootdir_sizep != 0)
        {
          g_hash_table_insert (new_bootcsums, g_strdup (bootcsum), bootdir_sizep);
          continue;
        }

      guint64 bootdir_size = 0;
      if (!get_kernel_layout_size (self, deployment, &bootdir_size, cancellable, error))
        return FALSE;

      /* see similar logic in previous loop */
      if (bootdir_size > G_MAXUINT)
        {
          g_printerr (
              "deployment %s kernel layout size exceeds %u; disabling auto-prune optimization\n",
              ostree_deployment_get_csum (deployment), G_MAXUINT);
          return TRUE;
        }

      g_hash_table_insert (new_bootcsums, g_strdup (bootcsum), GUINT_TO_POINTER (bootdir_size));

      /* it wasn't in current_bootcsums; add */
      net_new_bootcsum_dirs_total_size += bootdir_size;
    }

  {
    gboolean bootfs_has_space = FALSE;
    if (!dfd_fallocate_check (self->boot_fd, net_new_bootcsum_dirs_total_size, &bootfs_has_space,
                              error))
      return glnx_prefix_error (error, "Checking if bootfs has sufficient space");

    /* does the bootfs have enough free space for temporarily holding both the new
     * and old bootdirs? */
    if (bootfs_has_space)
      return TRUE; /* nothing to do! */
  }

  /* OK, we would fail if we tried to write the new bootdirs. Is it salvageable?
   * First, calculate how much space we could save with the bootcsums scheduled
   * for removal. */
  guint64 bootcsum_dirs_to_remove_total_size = 0;
  GLNX_HASH_TABLE_FOREACH_KV (current_bootcsums, const char *, bootcsum, gpointer, sizep)
    {
      if (!g_hash_table_contains (new_bootcsums, bootcsum))
        bootcsum_dirs_to_remove_total_size += GPOINTER_TO_UINT (sizep);
    }

  if (net_new_bootcsum_dirs_total_size > bootcsum_dirs_to_remove_total_size)
    {
      /* Check whether if we did early prune, we'd have enough space to write
       * the new bootcsum dirs. */
      gboolean bootfs_has_space = FALSE;
      if (!dfd_fallocate_check (
              self->boot_fd, net_new_bootcsum_dirs_total_size - bootcsum_dirs_to_remove_total_size,
              &bootfs_has_space, error))
        return glnx_prefix_error (error, "Checking if prune would give bootfs sufficient space");

      if (!bootfs_has_space)
        {
          /* Even if we auto-pruned, the new bootdirs wouldn't fit. Just let the
           * code continue and let it hit ENOSPC. */
          g_printerr ("Disabling auto-prune optimization; insufficient space left in bootfs\n");
          return TRUE;
        }
    }

  g_printerr ("Insufficient space left in bootfs; updating bootloader in two steps\n");

  /* Auto-pruning can salvage the situation. Calculate the set of deployments in common. */
  g_autoptr (GPtrArray) common_deployments = g_ptr_array_new ();
  for (guint i = 0; i < self->deployments->len; i++)
    {
      OstreeDeployment *deployment = self->deployments->pdata[i];
      const char *bootcsum = ostree_deployment_get_bootcsum (deployment);
      if (g_hash_table_contains (new_bootcsums, bootcsum))
        {
          g_ptr_array_add (common_deployments, deployment);
        }
      else
        {
          /* we always keep the booted deployment */
          g_assert (deployment != self->booted_deployment);
        }
    }

  /* if we're here, it means that removing some deployments is possible to gain space */
  g_assert_cmpuint (common_deployments->len, <, self->deployments->len);

  /* Do an initial write out where we do a pure deployment pruning, keeping
   * common deployments. To be safe, disable auto-pruning to make recursion
   * impossible (though the logic in this function shouldn't kick in anyway in
   * that recursive call). Disable cleaning since it's an intermediate stage. */
  OstreeSysrootWriteDeploymentsOpts opts
      = { .do_postclean = FALSE, .disable_auto_early_prune = TRUE };
  if (!ostree_sysroot_write_deployments_with_options (self, common_deployments, &opts, cancellable,
                                                      error))
    return FALSE;

  /* clean up /boot */
  if (!_ostree_sysroot_cleanup_bootfs (self, cancellable, error))
    return FALSE;

  return TRUE;
}

/**
 * ostree_sysroot_write_deployments_with_options:
 * @self: Sysroot
 * @new_deployments: (element-type OstreeDeployment): List of new deployments
 * @opts: Options
 * @cancellable: Cancellable
 * @error: Error
 *
 * Assuming @new_deployments have already been deployed in place on disk via
 * ostree_sysroot_deploy_tree(), atomically update bootloader configuration. By
 * default, no post-transaction cleanup will be performed. You should invoke
 * ostree_sysroot_cleanup() at some point after the transaction, or specify
 * `do_postclean` in @opts.  Skipping the post-transaction cleanup is useful
 * if for example you want to control pruning of the repository.
 *
 * Since: 2017.4
 */
gboolean
ostree_sysroot_write_deployments_with_options (OstreeSysroot *self, GPtrArray *new_deployments,
                                               OstreeSysrootWriteDeploymentsOpts *opts,
                                               GCancellable *cancellable, GError **error)
{
  g_assert (self->loadstate == OSTREE_SYSROOT_LOAD_STATE_LOADED);

  if (!_ostree_sysroot_ensure_writable (self, error))
    return FALSE;

  const bool skip_early_prune = (self->opt_flags & OSTREE_SYSROOT_GLOBAL_OPT_NO_EARLY_PRUNE) > 0;
  if (!skip_early_prune && !opts->disable_auto_early_prune
      && !auto_early_prune_old_deployments (self, new_deployments, cancellable, error))
    return FALSE;

  /* Dealing with the staged deployment is quite tricky here. This function is
   * primarily concerned with writing out "finalized" deployments which have
   * bootloader entries. Originally, we simply dropped the staged deployment
   * here unconditionally. Now, the high level strategy is to retain it, but
   * *only* if it's the first item in the new deployment list - otherwise, it's
   * silently dropped.
   */

  g_autoptr (GPtrArray) new_deployments_copy = g_ptr_array_new ();
  gboolean removed_staged = (self->staged_deployment != NULL);
  if (new_deployments->len > 0)
    {
      OstreeDeployment *first = new_deployments->pdata[0];
      /* If the first deployment is the staged, we filter it out for now */
      g_assert (first);
      if (first == self->staged_deployment)
        {
          g_assert (ostree_deployment_is_staged (first));

          /* In this case note staged was retained */
          removed_staged = FALSE;
        }

      /* Create a copy without any staged deployments */
      for (guint i = 0; i < new_deployments->len; i++)
        {
          OstreeDeployment *deployment = new_deployments->pdata[i];
          if (!ostree_deployment_is_staged (deployment))
            g_ptr_array_add (new_deployments_copy, deployment);
        }
      new_deployments = new_deployments_copy;
    }

  /* Take care of removing the staged deployment's on-disk state if we should */
  if (removed_staged)
    {
      g_assert (self->staged_deployment);
      g_assert (self->staged_deployment == self->deployments->pdata[0]);

      if (!glnx_unlinkat (AT_FDCWD, _OSTREE_SYSROOT_RUNSTATE_STAGED, 0, error))
        return FALSE;

      if (!_ostree_sysroot_rmrf_deployment (self, self->staged_deployment, cancellable, error))
        return FALSE;

      /* Delete the lock if there was any. */
      if (!ot_ensure_unlinked_at (AT_FDCWD, _OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED, error))
        return FALSE;

      /* Clear it out of the *current* deployments list to maintain invariants */
      self->staged_deployment = NULL;
      g_ptr_array_remove_index (self->deployments, 0);
    }
  const guint nonstaged_current_len = self->deployments->len - (self->staged_deployment ? 1 : 0);

  /* Assign a bootserial to each new deployment.
   */
  assign_bootserials (new_deployments);

  /* Determine whether or not we need to touch the bootloader
   * configuration.  If we have an equal number of deployments with
   * matching bootloader configuration, then we can just swap the
   * subbootversion bootlinks.
   */
  gboolean requires_new_bootversion = FALSE;

  if (new_deployments->len != nonstaged_current_len)
    requires_new_bootversion = TRUE;
  else
    {
      gboolean is_noop = TRUE;
      OstreeRepo *repo = ostree_sysroot_repo (self);
      for (guint i = 0; i < new_deployments->len; i++)
        {
          OstreeDeployment *cur_deploy = self->deployments->pdata[i];
          if (ostree_deployment_is_staged (cur_deploy))
            continue;
          OstreeDeployment *new_deploy = new_deployments->pdata[i];
          if (!deployment_bootconfigs_equal (repo, cur_deploy, new_deploy))
            {
              requires_new_bootversion = TRUE;
              is_noop = FALSE;
              break;
            }
          if (cur_deploy != new_deploy)
            is_noop = FALSE;
        }

      /* If we're passed the same set of deployments, we don't need
       * to drop into the rest of this function which deals with
       * changing the bootloader config.
       */
      if (is_noop)
        {
          g_assert (!requires_new_bootversion);
          /* However, if we dropped the staged deployment, we still
           * need to do finalization steps such as regenerating
           * the refs and bumping the mtime.
           */
          if (removed_staged)
            {
              if (!write_deployments_finish (self, cancellable, error))
                return FALSE;
            }
          return TRUE;
        }
    }

  gboolean found_booted_deployment = FALSE;
  for (guint i = 0; i < new_deployments->len; i++)
    {
      OstreeDeployment *deployment = new_deployments->pdata[i];
      g_assert (!ostree_deployment_is_staged (deployment));

      if (ostree_deployment_equal (deployment, self->booted_deployment))
        found_booted_deployment = TRUE;

      g_autoptr (GFile) deployment_root
          = ostree_sysroot_get_deployment_directory (self, deployment);
      if (!g_file_query_exists (deployment_root, NULL))
        return glnx_throw (error, "Unable to find expected deployment root: %s",
                           gs_file_get_path_cached (deployment_root));

      ostree_deployment_set_index (deployment, i);
    }

  if (self->booted_deployment && !found_booted_deployment)
    return glnx_throw (error, "Attempting to remove booted deployment");

  gboolean bootloader_is_atomic = FALSE;
  SyncStats syncstats = {
    0,
  };
  g_autoptr (OstreeBootloader) bootloader = NULL;
  g_autofree char *new_subbootdir = NULL;
  if (!requires_new_bootversion)
    {
      if (!create_new_bootlinks (self, self->bootversion, new_deployments, cancellable, error))
        return FALSE;

      if (!full_system_sync (self, &syncstats, cancellable, error))
        return FALSE;

      if (!swap_bootlinks (self, self->bootversion, new_deployments, &new_subbootdir, cancellable,
                           error))
        return FALSE;

      bootloader_is_atomic = TRUE;
    }
  else
    {
      if (!_ostree_sysroot_query_bootloader (self, &bootloader, cancellable, error))
        return FALSE;

      bootloader_is_atomic = bootloader != NULL && _ostree_bootloader_is_atomic (bootloader);

      if (!write_deployments_bootswap (self, new_deployments, opts, bootloader, &syncstats,
                                       &new_subbootdir, cancellable, error))
        return FALSE;
    }

  {
    g_autofree char *msg
        = g_strdup_printf ("%s; bootconfig swap: %s; bootversion: %s, deployment count change: %i",
                           (bootloader_is_atomic ? "Transaction complete" : "Bootloader updated"),
                           requires_new_bootversion ? "yes" : "no", new_subbootdir,
                           new_deployments->len - self->deployments->len);
    const gchar *bootloader_config = ostree_repo_get_bootloader (ostree_sysroot_repo (self));
    ot_journal_send (
        "MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL (OSTREE_DEPLOYMENT_COMPLETE_ID),
        "MESSAGE=%s", msg, "OSTREE_BOOTLOADER=%s",
        bootloader ? _ostree_bootloader_get_name (bootloader) : "none",
        "OSTREE_BOOTLOADER_CONFIG=%s", bootloader_config, "OSTREE_BOOTLOADER_ATOMIC=%s",
        bootloader_is_atomic ? "yes" : "no", "OSTREE_DID_BOOTSWAP=%s",
        requires_new_bootversion ? "yes" : "no", "OSTREE_N_DEPLOYMENTS=%u", new_deployments->len,
        "OSTREE_SYNCFS_ROOT_MSEC=%" G_GUINT64_FORMAT, syncstats.root_syncfs_msec,
        "OSTREE_SYNCFS_BOOT_MSEC=%" G_GUINT64_FORMAT, syncstats.boot_syncfs_msec, NULL);
    _ostree_sysroot_emit_journal_msg (self, msg);
  }

  if (!write_deployments_finish (self, cancellable, error))
    return FALSE;

  /* And finally, cleanup of any leftover data.
   */
  if (opts->do_postclean)
    {
      if (!ostree_sysroot_cleanup (self, cancellable, error))
        return glnx_prefix_error (error, "Performing final cleanup");
    }

  return TRUE;
}

static gboolean
allocate_deployserial (OstreeSysroot *self, const char *osname, const char *revision,
                       int *out_deployserial, GCancellable *cancellable, GError **error)
{
  int new_deployserial = 0;
  g_autoptr (GPtrArray) tmp_current_deployments = g_ptr_array_new_with_free_func (g_object_unref);

  glnx_autofd int deploy_dfd = -1;
  if (!glnx_opendirat (self->sysroot_fd, "ostree/deploy", TRUE, &deploy_dfd, error))
    return FALSE;

  if (!_ostree_sysroot_list_deployment_dirs_for_os (deploy_dfd, osname, tmp_current_deployments,
                                                    cancellable, error))
    return FALSE;

  for (guint i = 0; i < tmp_current_deployments->len; i++)
    {
      OstreeDeployment *deployment = tmp_current_deployments->pdata[i];

      if (strcmp (ostree_deployment_get_csum (deployment), revision) != 0)
        continue;

      new_deployserial
          = MAX (new_deployserial, ostree_deployment_get_deployserial (deployment) + 1);
    }

  *out_deployserial = new_deployserial;
  return TRUE;
}

void
_ostree_deployment_set_bootconfig_from_kargs (OstreeDeployment *deployment,
                                              char **override_kernel_argv)
{
  /* Create an empty boot configuration; we will merge things into
   * it as we go.
   */
  g_autoptr (OstreeBootconfigParser) bootconfig = ostree_bootconfig_parser_new ();
  ostree_deployment_set_bootconfig (deployment, bootconfig);

  /* After this, install_deployment_kernel() will set the other boot
   * options and write it out to disk.
   */
  if (override_kernel_argv)
    {
      g_autoptr (OstreeKernelArgs) kargs = ostree_kernel_args_new ();
      ostree_kernel_args_append_argv (kargs, override_kernel_argv);
      g_autofree char *new_options = ostree_kernel_args_to_string (kargs);
      ostree_bootconfig_parser_set (bootconfig, "options", new_options);
    }
}

// Perform some basic static analysis and emit warnings for things
// that are likely to fail later.  This function only returns
// a hard error if something unexpected (e.g. I/O error) occurs.
static gboolean
lint_deployment_fs (OstreeSysroot *self, OstreeDeployment *deployment, int deployment_dfd,
                    GCancellable *cancellable, GError **error)
{
  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };
  gboolean exists;

  if (!ot_dfd_iter_init_allow_noent (deployment_dfd, "var", &dfd_iter, &exists, error))
    return FALSE;
  while (exists)
    {
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      fprintf (stderr,
               "note: Deploying commit %s which contains content in /var/%s that should be in "
               "/usr/share/factory/var\n",
               ostree_deployment_get_csum (deployment), dent->d_name);
    }

  return TRUE;
}

static gboolean
require_stateroot (OstreeSysroot *self, const char *stateroot, GError **error)
{
  const char *osdeploypath = glnx_strjoina ("ostree/deploy/", stateroot);
  if (!glnx_fstatat_allow_noent (self->sysroot_fd, osdeploypath, NULL, 0, error))
    return FALSE;
  if (errno == ENOENT)
    return glnx_throw (error, "No such stateroot: %s", stateroot);
  return TRUE;
}

/* The first part of writing a deployment. This primarily means doing the
 * hardlink farm checkout, but we also compute some initial state.
 */
static gboolean
sysroot_initialize_deployment (OstreeSysroot *self, const char *osname, const char *revision,
                               GKeyFile *origin, OstreeSysrootDeployTreeOpts *opts,
                               OstreeDeployment **out_new_deployment, GCancellable *cancellable,
                               GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Initializing deployment", error);

  g_assert (osname != NULL || self->booted_deployment != NULL);

  if (osname == NULL)
    osname = ostree_deployment_get_osname (self->booted_deployment);

  if (!require_stateroot (self, osname, error))
    return FALSE;

  OstreeRepo *repo = ostree_sysroot_repo (self);

  gint new_deployserial;
  if (!allocate_deployserial (self, osname, revision, &new_deployserial, cancellable, error))
    return FALSE;

  g_autoptr (OstreeDeployment) new_deployment
      = ostree_deployment_new (0, osname, revision, new_deployserial, NULL, -1);
  ostree_deployment_set_origin (new_deployment, origin);

  /* Check out the userspace tree onto the filesystem */
  glnx_autofd int deployment_dfd = -1;
  if (!checkout_deployment_tree (self, repo, new_deployment, revision, &deployment_dfd, cancellable,
                                 error))
    return FALSE;

  g_autoptr (OstreeKernelLayout) kernel_layout = NULL;
  if (!get_kernel_from_tree (self, deployment_dfd, &kernel_layout, cancellable, error))
    return FALSE;

  _ostree_deployment_set_bootcsum (new_deployment, kernel_layout->bootcsum);
  _ostree_deployment_set_bootconfig_from_kargs (new_deployment,
                                                opts ? opts->override_kernel_argv : NULL);
  _ostree_deployment_set_overlay_initrds (new_deployment, opts ? opts->overlay_initrds : NULL);

  if (!prepare_deployment_etc (self, repo, new_deployment, deployment_dfd, cancellable, error))
    return FALSE;

  if (!lint_deployment_fs (self, new_deployment, deployment_dfd, cancellable, error))
    return FALSE;

  ot_transfer_out_value (out_new_deployment, &new_deployment);
  return TRUE;
}

/* Get a directory fd for the /var of @deployment.
 * Before we supported having /var be a separate mount point,
 * this was easy. However, as https://github.com/ostreedev/ostree/issues/1729
 * raised, in the primary case where we're
 * doing a new deployment for the booted stateroot,
 * we need to use /var/.  This code doesn't correctly
 * handle the case of `ostree admin --sysroot upgrade`,
 * nor (relatedly) the case of upgrading a separate stateroot.
 */
static gboolean
get_var_dfd (OstreeSysroot *self, int osdeploy_dfd, OstreeDeployment *deployment, int *ret_fd,
             GError **error)
{
  const char *booted_stateroot
      = self->booted_deployment ? ostree_deployment_get_osname (self->booted_deployment) : NULL;

  int base_dfd;
  const char *base_path;
  /* The common case is when we're doing a new deployment for the same stateroot (osname).
   * If we have a separate mounted /var, then we need to use it - the /var in the
   * stateroot will probably just be an empty directory.
   *
   * If the stateroot doesn't match, just fall back to /var in the target's stateroot.
   */
  if (g_strcmp0 (booted_stateroot, ostree_deployment_get_osname (deployment)) == 0)
    {
      base_dfd = AT_FDCWD;
      base_path = "/var";
    }
  else
    {
      base_dfd = osdeploy_dfd;
      base_path = "var";
    }

  return glnx_opendirat (base_dfd, base_path, TRUE, ret_fd, error);
}

static void
child_setup_fchdir (gpointer data)
{
  int fd = (int)(uintptr_t)data;
  int rc __attribute__ ((unused));

  rc = fchdir (fd);
}

/*
 * Derived from rpm-ostree's rust/src/bwrap.rs
 */
gboolean
_ostree_sysroot_run_in_deployment (int deployment_dfd, const char *const *bwrap_argv,
                                   const gchar *const *child_argv, gint *exit_status,
                                   gchar **stdout, GError **error)
{
  static const gchar *const COMMON_ARGV[] = { "/usr/bin/bwrap",
                                              "--dev",
                                              "/dev",
                                              "--proc",
                                              "/proc",
                                              "--dir",
                                              "/run",
                                              "--dir",
                                              "/tmp",
                                              "--chdir",
                                              "/",
                                              "--die-with-parent",
                                              "--unshare-pid",
                                              "--unshare-uts",
                                              "--unshare-ipc",
                                              "--unshare-cgroup-try",
                                              "--ro-bind",
                                              "/sys/block",
                                              "/sys/block",
                                              "--ro-bind",
                                              "/sys/bus",
                                              "/sys/bus",
                                              "--ro-bind",
                                              "/sys/class",
                                              "/sys/class",
                                              "--ro-bind",
                                              "/sys/dev",
                                              "/sys/dev",
                                              "--ro-bind",
                                              "/sys/devices",
                                              "/sys/devices",
                                              "--bind",
                                              "usr",
                                              "/usr",
                                              "--bind",
                                              "etc",
                                              "/etc",
                                              "--bind",
                                              "var",
                                              "/var",
                                              "--symlink",
                                              "/usr/lib",
                                              "/lib",
                                              "--symlink",
                                              "/usr/lib32",
                                              "/lib32",
                                              "--symlink",
                                              "/usr/lib64",
                                              "/lib64",
                                              "--symlink",
                                              "/usr/bin",
                                              "/bin",
                                              "--symlink",
                                              "/usr/sbin",
                                              "/sbin",
                                              NULL };

  g_autoptr (GPtrArray) args = g_ptr_array_new ();

  for (char **it = (char **)COMMON_ARGV; it && *it; it++)
    g_ptr_array_add (args, *it);
  for (char **it = (char **)bwrap_argv; it && *it; it++)
    g_ptr_array_add (args, *it);

  // Separate bwrap args from child args
  g_ptr_array_add (args, "--");

  for (char **it = (char **)child_argv; it && *it; it++)
    g_ptr_array_add (args, *it);

  g_ptr_array_add (args, NULL);

  return g_spawn_sync (NULL, (char **)args->pdata, NULL, 0, &child_setup_fchdir,
                       (gpointer)(uintptr_t)deployment_dfd, stdout, NULL, exit_status, error);
}

#ifdef HAVE_SELINUX
/*
 * Run semodule to check if the module content changed after merging /etc
 * and rebuild the policy if needed.
 */
static gboolean
sysroot_finalize_selinux_policy (int deployment_dfd, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Finalizing SELinux policy", error);
  struct stat stbuf;
  gint exit_status;
  g_autofree gchar *stdout = NULL;

  if (!glnx_fstatat_allow_noent (deployment_dfd, "etc/selinux/config", &stbuf, AT_SYMLINK_NOFOLLOW,
                                 error))
    return FALSE;

  /* Skip the SELinux policy refresh if /etc/selinux/config doesn't exist. */
  if (errno != 0)
    return TRUE;

  /*
   * Skip the SELinux policy refresh if the --refresh
   * flag is not supported by semodule.
   */
  static const gchar *const SEMODULE_HELP_ARGV[] = { "semodule", "--help", NULL };
  if (!_ostree_sysroot_run_in_deployment (deployment_dfd, NULL, SEMODULE_HELP_ARGV, &exit_status,
                                          &stdout, error))
    return FALSE;
  if (!g_spawn_check_exit_status (exit_status, error))
    return glnx_prefix_error (error, "failed to run semodule");
  if (!strstr (stdout, "--refresh"))
    {
      ot_journal_print (LOG_INFO, "semodule does not have --refresh");
      return TRUE;
    }

  static const gchar *const SEMODULE_REBUILD_ARGV[] = { "semodule", "-N", "--refresh", NULL };

  ot_journal_print (LOG_INFO, "Refreshing SELinux policy");
  guint64 start_msec = g_get_monotonic_time () / 1000;
  if (!_ostree_sysroot_run_in_deployment (deployment_dfd, NULL, SEMODULE_REBUILD_ARGV, &exit_status,
                                          NULL, error))
    return FALSE;
  guint64 end_msec = g_get_monotonic_time () / 1000;
  ot_journal_print (LOG_INFO, "Refreshed SELinux policy in %" G_GUINT64_FORMAT " ms",
                    end_msec - start_msec);
  return g_spawn_check_exit_status (exit_status, error);
}
#endif /* HAVE_SELINUX */

static gboolean
sysroot_finalize_deployment (OstreeSysroot *self, OstreeDeployment *deployment,
                             OstreeDeployment *merge_deployment, GCancellable *cancellable,
                             GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Finalizing deployment", error);
  g_autofree char *deployment_path = ostree_sysroot_get_deployment_dirpath (self, deployment);
  glnx_autofd int deployment_dfd = -1;
  if (!glnx_opendirat (self->sysroot_fd, deployment_path, TRUE, &deployment_dfd, error))
    return FALSE;

  OstreeBootconfigParser *bootconfig = ostree_deployment_get_bootconfig (deployment);

  /* If the kargs weren't set yet, then just pick it up from the merge deployment. In the
   * deploy path, overrides are set as part of sysroot_initialize_deployment(). In the
   * finalize-staged path, they're set by OstreeSysroot when reading the staged GVariant. */
  if (merge_deployment && ostree_bootconfig_parser_get (bootconfig, "options") == NULL)
    {
      OstreeBootconfigParser *merge_bootconfig
          = ostree_deployment_get_bootconfig (merge_deployment);
      if (merge_bootconfig)
        {
          const char *kargs = ostree_bootconfig_parser_get (merge_bootconfig, "options");
          ostree_bootconfig_parser_set (bootconfig, "options", kargs);
        }
    }

  if (merge_deployment)
    {
      /* And do the /etc merge */
      if (!merge_configuration_from (self, merge_deployment, deployment, deployment_dfd,
                                     cancellable, error))
        return FALSE;

#ifdef HAVE_SELINUX
      if (!sysroot_finalize_selinux_policy (deployment_dfd, error))
        return FALSE;
#endif /* HAVE_SELINUX */
    }

  const char *osdeploypath
      = glnx_strjoina ("ostree/deploy/", ostree_deployment_get_osname (deployment));
  glnx_autofd int os_deploy_dfd = -1;
  if (!glnx_opendirat (self->sysroot_fd, osdeploypath, TRUE, &os_deploy_dfd, error))
    return FALSE;
  glnx_autofd int var_dfd = -1;
  if (!get_var_dfd (self, os_deploy_dfd, deployment, &var_dfd, error))
    return FALSE;

  /* Ensure that the new deployment does not have /etc/.updated or
   * /var/.updated so that systemd ConditionNeedsUpdate=/etc|/var services run
   * after rebooting.
   */
  if (!ot_ensure_unlinked_at (deployment_dfd, "etc/.updated", error))
    return FALSE;
  if (!ot_ensure_unlinked_at (var_dfd, ".updated", error))
    return FALSE;

  g_autoptr (OstreeSePolicy) sepolicy = ostree_sepolicy_new_at (deployment_dfd, cancellable, error);
  if (!sepolicy)
    return FALSE;

  if (!selinux_relabel_var_if_needed (self, sepolicy, os_deploy_dfd, cancellable, error))
    return FALSE;

  /* Rewrite the origin using the final merged selinux config, just to be
   * conservative about getting the right labels.
   */
  if (!write_origin_file_internal (self, sepolicy, deployment,
                                   ostree_deployment_get_origin (deployment),
                                   GLNX_FILE_REPLACE_NODATASYNC, cancellable, error))
    return FALSE;

  /* Seal it */
  if (!(self->debug_flags & OSTREE_SYSROOT_DEBUG_MUTABLE_DEPLOYMENTS))
    {
      if (!ostree_sysroot_deployment_set_mutable (self, deployment, FALSE, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/**
 * ostree_sysroot_deploy_tree_with_options:
 * @self: Sysroot
 * @osname: (nullable): osname to use for merge deployment
 * @revision: Checksum to add
 * @origin: (nullable): Origin to use for upgrades
 * @provided_merge_deployment: (nullable): Use this deployment for merge path
 * @opts: (nullable): Options
 * @out_new_deployment: (out) (transfer full): The new deployment path
 * @cancellable: Cancellable
 * @error: Error
 *
 * Check out deployment tree with revision @revision, performing a 3
 * way merge with @provided_merge_deployment for configuration.
 *
 * When booted into the sysroot, you should use the
 * ostree_sysroot_stage_tree() API instead.
 *
 * Since: 2020.7
 */
gboolean
ostree_sysroot_deploy_tree_with_options (OstreeSysroot *self, const char *osname,
                                         const char *revision, GKeyFile *origin,
                                         OstreeDeployment *provided_merge_deployment,
                                         OstreeSysrootDeployTreeOpts *opts,
                                         OstreeDeployment **out_new_deployment,
                                         GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Deploying tree", error);

  if (!_ostree_sysroot_ensure_writable (self, error))
    return FALSE;

  g_autoptr (OstreeDeployment) deployment = NULL;
  if (!sysroot_initialize_deployment (self, osname, revision, origin, opts, &deployment,
                                      cancellable, error))
    return FALSE;

  if (!sysroot_finalize_deployment (self, deployment, provided_merge_deployment, cancellable,
                                    error))
    return FALSE;

  *out_new_deployment = g_steal_pointer (&deployment);
  return TRUE;
}

/**
 * ostree_sysroot_deploy_tree:
 * @self: Sysroot
 * @osname: (nullable): osname to use for merge deployment
 * @revision: Checksum to add
 * @origin: (nullable): Origin to use for upgrades
 * @provided_merge_deployment: (nullable): Use this deployment for merge path
 * @override_kernel_argv: (nullable) (array zero-terminated=1) (element-type utf8): Use these as
 * kernel arguments; if %NULL, inherit options from provided_merge_deployment
 * @out_new_deployment: (out): The new deployment path
 * @cancellable: Cancellable
 * @error: Error
 *
 * Older version of ostree_sysroot_stage_tree_with_options().
 *
 * Since: 2018.5
 */
gboolean
ostree_sysroot_deploy_tree (OstreeSysroot *self, const char *osname, const char *revision,
                            GKeyFile *origin, OstreeDeployment *provided_merge_deployment,
                            char **override_kernel_argv, OstreeDeployment **out_new_deployment,
                            GCancellable *cancellable, GError **error)
{
  OstreeSysrootDeployTreeOpts opts = { .override_kernel_argv = override_kernel_argv };
  return ostree_sysroot_deploy_tree_with_options (self, osname, revision, origin,
                                                  provided_merge_deployment, &opts,
                                                  out_new_deployment, cancellable, error);
}

/* Serialize information about a deployment to a variant, used by the staging
 * code.
 */
static GVariant *
serialize_deployment_to_variant (OstreeDeployment *deployment)
{
  g_auto (GVariantBuilder) builder = OT_VARIANT_BUILDER_INITIALIZER;
  g_variant_builder_init (&builder, (GVariantType *)"a{sv}");
  g_autofree char *name = g_strdup_printf ("%s.%d", ostree_deployment_get_csum (deployment),
                                           ostree_deployment_get_deployserial (deployment));
  g_variant_builder_add (&builder, "{sv}", "name", g_variant_new_string (name));
  g_variant_builder_add (&builder, "{sv}", "osname",
                         g_variant_new_string (ostree_deployment_get_osname (deployment)));
  g_variant_builder_add (&builder, "{sv}", "bootcsum",
                         g_variant_new_string (ostree_deployment_get_bootcsum (deployment)));

  return g_variant_builder_end (&builder);
}

static gboolean
require_str_key (GVariantDict *dict, const char *name, const char **ret, GError **error)
{
  if (!g_variant_dict_lookup (dict, name, "&s", ret))
    return glnx_throw (error, "Missing key: %s", name);
  return TRUE;
}

/* Reverse of the above; convert a variant to a deployment. Note that the
 * deployment may not actually be present; this should be verified by
 * higher level code.
 */
OstreeDeployment *
_ostree_sysroot_deserialize_deployment_from_variant (GVariant *v, GError **error)
{
  g_autoptr (GVariantDict) dict = g_variant_dict_new (v);
  const char *name = NULL;
  if (!require_str_key (dict, "name", &name, error))
    return FALSE;
  const char *bootcsum = NULL;
  if (!require_str_key (dict, "bootcsum", &bootcsum, error))
    return FALSE;
  const char *osname = NULL;
  if (!require_str_key (dict, "osname", &osname, error))
    return FALSE;
  g_autofree char *checksum = NULL;
  gint deployserial;
  if (!_ostree_sysroot_parse_deploy_path_name (name, &checksum, &deployserial, error))
    return NULL;
  return ostree_deployment_new (-1, osname, checksum, deployserial, bootcsum, -1);
}

/**
 * ostree_sysroot_stage_overlay_initrd:
 * @self: Sysroot
 * @fd: File descriptor to overlay initrd
 * @out_checksum: (out) (transfer full): Overlay initrd checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Stage an overlay initrd to be used in an upcoming deployment. Returns a checksum which
 * can be passed to ostree_sysroot_deploy_tree_with_options() or
 * ostree_sysroot_stage_tree_with_options() via the `overlay_initrds` array option.
 *
 * Since: 2020.7
 */
gboolean
ostree_sysroot_stage_overlay_initrd (OstreeSysroot *self, int fd, char **out_checksum,
                                     GCancellable *cancellable, GError **error)
{
  g_assert (fd != -1);
  g_assert (out_checksum != NULL);

  if (!glnx_shutil_mkdir_p_at (AT_FDCWD, _OSTREE_SYSROOT_RUNSTATE_STAGED_INITRDS_DIR, 0755,
                               cancellable, error))
    return FALSE;

  glnx_autofd int staged_initrds_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, _OSTREE_SYSROOT_RUNSTATE_STAGED_INITRDS_DIR, FALSE,
                       &staged_initrds_dfd, error))
    return FALSE;

  g_auto (GLnxTmpfile) overlay_initrd = {
    0,
  };
  if (!glnx_open_tmpfile_linkable_at (staged_initrds_dfd, ".", O_WRONLY | O_CLOEXEC,
                                      &overlay_initrd, error))
    return FALSE;

  char checksum[_OSTREE_SHA256_STRING_LEN + 1];
  {
    g_autoptr (GOutputStream) output = g_unix_output_stream_new (overlay_initrd.fd, FALSE);
    g_autoptr (GInputStream) input = g_unix_input_stream_new (fd, FALSE);
    g_autofree guchar *digest = NULL;
    if (!ot_gio_splice_get_checksum (output, input, &digest, cancellable, error))
      return FALSE;
    ot_bin2hex (checksum, (guint8 *)digest, _OSTREE_SHA256_DIGEST_LEN);
  }

  if (!glnx_link_tmpfile_at (&overlay_initrd, GLNX_LINK_TMPFILE_REPLACE, staged_initrds_dfd,
                             checksum, error))
    return FALSE;

  *out_checksum = g_strdup (checksum);
  return TRUE;
}

/**
 * ostree_sysroot_stage_tree:
 * @self: Sysroot
 * @osname: (allow-none): osname to use for merge deployment
 * @revision: Checksum to add
 * @origin: (allow-none): Origin to use for upgrades
 * @merge_deployment: (allow-none): Use this deployment for merge path
 * @override_kernel_argv: (allow-none) (array zero-terminated=1) (element-type utf8): Use these as
 * kernel arguments; if %NULL, inherit options from provided_merge_deployment
 * @out_new_deployment: (out): The new deployment path
 * @cancellable: Cancellable
 * @error: Error
 *
 * Older version of ostree_sysroot_stage_tree_with_options().
 *
 * Since: 2018.5
 */
gboolean
ostree_sysroot_stage_tree (OstreeSysroot *self, const char *osname, const char *revision,
                           GKeyFile *origin, OstreeDeployment *merge_deployment,
                           char **override_kernel_argv, OstreeDeployment **out_new_deployment,
                           GCancellable *cancellable, GError **error)
{
  OstreeSysrootDeployTreeOpts opts = { .override_kernel_argv = override_kernel_argv };
  return ostree_sysroot_stage_tree_with_options (self, osname, revision, origin, merge_deployment,
                                                 &opts, out_new_deployment, cancellable, error);
}

/**
 * ostree_sysroot_stage_tree_with_options:
 * @self: Sysroot
 * @osname: (allow-none): osname to use for merge deployment
 * @revision: Checksum to add
 * @origin: (allow-none): Origin to use for upgrades
 * @merge_deployment: (allow-none): Use this deployment for merge path
 * @opts: Options
 * @out_new_deployment: (out): The new deployment path
 * @cancellable: Cancellable
 * @error: Error
 *
 * Like ostree_sysroot_deploy_tree(), but "finalization" only occurs at OS
 * shutdown time.
 *
 * Since: 2020.7
 */
gboolean
ostree_sysroot_stage_tree_with_options (OstreeSysroot *self, const char *osname,
                                        const char *revision, GKeyFile *origin,
                                        OstreeDeployment *merge_deployment,
                                        OstreeSysrootDeployTreeOpts *opts,
                                        OstreeDeployment **out_new_deployment,
                                        GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Staging deployment", error);

  if (!_ostree_sysroot_ensure_writable (self, error))
    return FALSE;

  OstreeDeployment *booted_deployment = ostree_sysroot_require_booted_deployment (self, error);
  if (booted_deployment == NULL)
    return glnx_prefix_error (error, "Cannot stage deployment");

  g_autoptr (OstreeDeployment) deployment = NULL;
  if (!sysroot_initialize_deployment (self, osname, revision, origin, opts, &deployment,
                                      cancellable, error))
    return FALSE;

  /* Write out the origin file using the sepolicy from the non-merged root for
   * now (i.e. using /usr/etc policy, not /etc); in practice we don't really
   * expect people to customize the label for it.
   */
  {
    g_autofree char *deployment_path = ostree_sysroot_get_deployment_dirpath (self, deployment);
    glnx_autofd int deployment_dfd = -1;
    if (!glnx_opendirat (self->sysroot_fd, deployment_path, FALSE, &deployment_dfd, error))
      return FALSE;
    g_autoptr (OstreeSePolicy) sepolicy
        = ostree_sepolicy_new_at (deployment_dfd, cancellable, error);
    if (!sepolicy)
      return FALSE;
    if (!write_origin_file_internal (self, sepolicy, deployment,
                                     ostree_deployment_get_origin (deployment),
                                     GLNX_FILE_REPLACE_NODATASYNC, cancellable, error))
      return FALSE;
  }

  /* After here we defer action until shutdown. The remaining arguments (merge
   * deployment, kargs) are serialized to a state file in /run.
   */

  /* "target" is the staged deployment */
  g_autoptr (GVariantBuilder) builder = g_variant_builder_new ((GVariantType *)"a{sv}");
  g_variant_builder_add (builder, "{sv}", "target", serialize_deployment_to_variant (deployment));

  if (opts->locked)
    g_variant_builder_add (builder, "{sv}", _OSTREE_SYSROOT_STAGED_KEY_LOCKED,
                           g_variant_new_boolean (TRUE));

  if (merge_deployment)
    g_variant_builder_add (builder, "{sv}", "merge-deployment",
                           serialize_deployment_to_variant (merge_deployment));

  if (opts && opts->override_kernel_argv)
    g_variant_builder_add (
        builder, "{sv}", "kargs",
        g_variant_new_strv ((const char *const *)opts->override_kernel_argv, -1));
  if (opts && opts->overlay_initrds)
    g_variant_builder_add (builder, "{sv}", "overlay-initrds",
                           g_variant_new_strv ((const char *const *)opts->overlay_initrds, -1));

  const char *parent = dirname (strdupa (_OSTREE_SYSROOT_RUNSTATE_STAGED));
  if (!glnx_shutil_mkdir_p_at (AT_FDCWD, parent, 0755, cancellable, error))
    return FALSE;

  g_autoptr (GVariant) state = g_variant_ref_sink (g_variant_builder_end (builder));
  if (!glnx_file_replace_contents_at (AT_FDCWD, _OSTREE_SYSROOT_RUNSTATE_STAGED,
                                      g_variant_get_data (state), g_variant_get_size (state),
                                      GLNX_FILE_REPLACE_NODATASYNC, cancellable, error))
    return FALSE;

  /* If we have a previous one, clean it up */
  if (self->staged_deployment)
    {
      if (!_ostree_sysroot_rmrf_deployment (self, self->staged_deployment, cancellable, error))
        return FALSE;
      // Also remove the lock; xref https://github.com/ostreedev/ostree/issues/3025
      if (!ot_ensure_unlinked_at (AT_FDCWD, _OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED, error))
        return FALSE;
    }

  /* Bump mtime so external processes know something changed, and then reload. */
  if (!_ostree_sysroot_bump_mtime (self, error))
    return FALSE;
  if (!ostree_sysroot_load (self, cancellable, error))
    return FALSE;
  /* Like deploy, we do a prepare cleanup; among other things, this ensures
   * that a ref will be written for the staged tree.  See also
   * https://github.com/ostreedev/ostree/pull/1566 though which
   * adds an ostree_sysroot_cleanup_prune() API.
   */
  if (!ostree_sysroot_prepare_cleanup (self, cancellable, error))
    return FALSE;

  ot_transfer_out_value (out_new_deployment, &deployment);
  return TRUE;
}

/**
 * ostree_sysroot_change_finalization:
 * @self: Sysroot
 * @deployment: Deployment which must be staged
 * @error: Error
 *
 * Given the target deployment (which must be the staged deployment) this API
 * will toggle its "finalization locking" state.  If it is currently locked,
 * it will be unlocked (and hence queued to apply on shutdown).
 *
 * Since: 2023.8
 */
_OSTREE_PUBLIC
gboolean
ostree_sysroot_change_finalization (OstreeSysroot *self, OstreeDeployment *deployment,
                                    GError **error)
{
  GCancellable *cancellable = NULL;
  g_assert (ostree_deployment_is_staged (deployment));

  gboolean new_locked_state = !ostree_deployment_is_finalization_locked (deployment);

  /* Read the staged state from disk */
  glnx_autofd int fd = -1;
  if (!glnx_openat_rdonly (AT_FDCWD, _OSTREE_SYSROOT_RUNSTATE_STAGED, TRUE, &fd, error))
    return FALSE;

  g_autoptr (GBytes) contents = ot_fd_readall_or_mmap (fd, 0, error);
  if (!contents)
    return FALSE;
  g_autoptr (GVariant) staged_deployment_data
      = g_variant_new_from_bytes ((GVariantType *)"a{sv}", contents, TRUE);
  g_autoptr (GVariantDict) staged_deployment_dict = g_variant_dict_new (staged_deployment_data);

  g_variant_dict_insert (staged_deployment_dict, _OSTREE_SYSROOT_STAGED_KEY_LOCKED, "b",
                         new_locked_state);
  g_autoptr (GVariant) new_staged_deployment_data = g_variant_dict_end (staged_deployment_dict);

  if (!glnx_file_replace_contents_at (fd, _OSTREE_SYSROOT_RUNSTATE_STAGED,
                                      g_variant_get_data (new_staged_deployment_data),
                                      g_variant_get_size (new_staged_deployment_data),
                                      GLNX_FILE_REPLACE_NODATASYNC, cancellable, error))
    return FALSE;

  if (!new_locked_state)
    {
      /* Delete the legacy lock if there was any. */
      if (!ot_ensure_unlinked_at (AT_FDCWD, _OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED, error))
        return FALSE;
    }
  else
    {
      /* Create the legacy lockfile; see also the code in ot-admin-builtin-deploy.c */
      if (!glnx_shutil_mkdir_p_at (AT_FDCWD,
                                   dirname (strdupa (_OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED)), 0755,
                                   cancellable, error))
        return FALSE;

      glnx_autofd int lockfd = open (_OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED,
                                     O_CREAT | O_WRONLY | O_NOCTTY | O_CLOEXEC, 0640);
      if (lockfd == -1)
        return glnx_throw_errno_prefix (error, "touch(%s)", _OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED);
    }

  return TRUE;
}

/* Invoked at shutdown time by ostree-finalize-staged.service */
static gboolean
_ostree_sysroot_finalize_staged_inner (OstreeSysroot *self, GCancellable *cancellable,
                                       GError **error)
{
  /* It's totally fine if there's no staged deployment; perhaps down the line
   * though we could teach the ostree cmdline to tell systemd to activate the
   * service when a staged deployment is created.
   */
  if (!self->staged_deployment)
    {
      ot_journal_print (LOG_INFO, "No deployment staged for finalization");
      return TRUE;
    }

  /* Check if finalization is locked. */
  gboolean locked = false;
  (void)g_variant_lookup (self->staged_deployment_data, _OSTREE_SYSROOT_STAGED_KEY_LOCKED, "b",
                          &locked);
  if (locked)
    g_debug ("staged is locked via metadata");
  else
    {
      if (!glnx_fstatat_allow_noent (AT_FDCWD, _OSTREE_SYSROOT_RUNSTATE_STAGED_LOCKED, NULL, 0,
                                     error))
        return FALSE;
      if (errno == 0)
        locked = TRUE;
    }
  if (locked)
    {
      ot_journal_print (LOG_INFO, "Not finalizing; deployment is locked for finalization");
      return TRUE;
    }

  /* Notice we send this *after* the trivial `return TRUE` above; this msg implies we've
   * committed to finalizing the deployment. */
  ot_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR,
                   SD_ID128_FORMAT_VAL (OSTREE_DEPLOYMENT_FINALIZING_ID),
                   "MESSAGE=Finalizing staged deployment", "OSTREE_OSNAME=%s",
                   ostree_deployment_get_osname (self->staged_deployment), "OSTREE_CHECKSUM=%s",
                   ostree_deployment_get_csum (self->staged_deployment), "OSTREE_DEPLOYSERIAL=%u",
                   ostree_deployment_get_deployserial (self->staged_deployment), NULL);

  g_assert (self->staged_deployment_data);

  g_autoptr (OstreeDeployment) merge_deployment = NULL;
  g_autoptr (GVariant) merge_deployment_v = NULL;
  if (g_variant_lookup (self->staged_deployment_data, "merge-deployment", "@a{sv}",
                        &merge_deployment_v))
    {
      g_autoptr (OstreeDeployment) merge_deployment_stub
          = _ostree_sysroot_deserialize_deployment_from_variant (merge_deployment_v, error);
      if (!merge_deployment_stub)
        return FALSE;
      for (guint i = 0; i < self->deployments->len; i++)
        {
          OstreeDeployment *deployment = self->deployments->pdata[i];
          if (ostree_deployment_equal (deployment, merge_deployment_stub))
            {
              merge_deployment = g_object_ref (deployment);
              break;
            }
        }

      if (!merge_deployment)
        return glnx_throw (error, "Failed to find merge deployment %s.%d for staged",
                           ostree_deployment_get_csum (merge_deployment_stub),
                           ostree_deployment_get_deployserial (merge_deployment_stub));
    }

  /* Unlink the staged state now; if we're interrupted in the middle,
   * we don't want e.g. deal with the partially written /etc merge.
   */
  if (!glnx_unlinkat (AT_FDCWD, _OSTREE_SYSROOT_RUNSTATE_STAGED, 0, error))
    return FALSE;

  if (!sysroot_finalize_deployment (self, self->staged_deployment, merge_deployment, cancellable,
                                    error))
    return FALSE;
  ot_journal_print (LOG_INFO, "Finalized deployment");

  /* Now, take ownership of the staged state, as normally the API below strips
   * it out.
   */
  g_autoptr (OstreeDeployment) staged = g_steal_pointer (&self->staged_deployment);
  staged->staged = FALSE;
  g_ptr_array_remove_index (self->deployments, 0);

  /* TODO: Proxy across flags too?
   *
   * But note that we always use NO_CLEAN to avoid adding more latency at
   * shutdown, and also because e.g. rpm-ostree wants to own the cleanup
   * process.
   */
  OstreeSysrootSimpleWriteDeploymentFlags flags
      = OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NO_CLEAN;
  if (!ostree_sysroot_simple_write_deployment (self, ostree_deployment_get_osname (staged), staged,
                                               merge_deployment, flags, cancellable, error))
    return FALSE;
  ot_journal_print (LOG_INFO, "Finished writing deployment");

  /* Do the basic cleanup that may impact /boot, but not the repo pruning */
  if (!ostree_sysroot_prepare_cleanup (self, cancellable, error))
    return FALSE;
  ot_journal_print (LOG_INFO, "Cleanup complete");

  // Cleanup will have closed some FDs, re-ensure writability
  if (!_ostree_sysroot_ensure_writable (self, error))
    return FALSE;

  return TRUE;
}

/* Invoked at shutdown time by ostree-finalize-staged.service */
gboolean
_ostree_sysroot_finalize_staged (OstreeSysroot *self, GCancellable *cancellable, GError **error)
{
  g_autoptr (GError) finalization_error = NULL;
  if (!_ostree_sysroot_ensure_boot_fd (self, error))
    return FALSE;
  if (!_ostree_sysroot_finalize_staged_inner (self, cancellable, &finalization_error))
    {
      g_autoptr (GError) writing_error = NULL;
      g_assert_cmpint (self->boot_fd, !=, -1);
      if (!glnx_file_replace_contents_at (self->boot_fd, _OSTREE_FINALIZE_STAGED_FAILURE_PATH,
                                          (guint8 *)finalization_error->message, -1, 0, cancellable,
                                          &writing_error))
        {
          // We somehow failed to write the failure message...that's not great.  Maybe ENOSPC on
          // /boot.
          g_printerr ("Failed to write %s: %s\n", _OSTREE_FINALIZE_STAGED_FAILURE_PATH,
                      writing_error->message);
        }
      g_propagate_error (error, g_steal_pointer (&finalization_error));
      return FALSE;
    }
  else
    {
      /* we may have failed in a previous invocation on this boot, but we were
       * rerun again (likely manually) and passed this time; nuke any stamp */
      if (!glnx_shutil_rm_rf_at (self->boot_fd, _OSTREE_FINALIZE_STAGED_FAILURE_PATH, cancellable,
                                 error))
        return FALSE;
    }
  return TRUE;
}

/* Invoked at bootup time by ostree-boot-complete.service */
gboolean
_ostree_sysroot_boot_complete (OstreeSysroot *self, GCancellable *cancellable, GError **error)
{
  if (!_ostree_sysroot_ensure_boot_fd (self, error))
    return FALSE;

  glnx_autofd int failure_fd = -1;
  g_assert_cmpint (self->boot_fd, !=, -1);
  if (!ot_openat_ignore_enoent (self->boot_fd, _OSTREE_FINALIZE_STAGED_FAILURE_PATH, &failure_fd,
                                error))
    return FALSE;
  // If we didn't find a failure log, then there's nothing to do right now.
  // (Actually this unit shouldn't even be invoked, but we may do more in the future)
  if (failure_fd == -1)
    return TRUE;
  g_autofree char *failure_data = glnx_fd_readall_utf8 (failure_fd, NULL, cancellable, error);
  if (failure_data == NULL)
    return glnx_prefix_error (error, "Reading from %s", _OSTREE_FINALIZE_STAGED_FAILURE_PATH);
  // Remove the file; we don't want to continually error out.
  (void)unlinkat (self->boot_fd, _OSTREE_FINALIZE_STAGED_FAILURE_PATH, 0);
  return glnx_throw (error, "ostree-finalize-staged.service failed on previous boot: %s",
                     failure_data);
}

/**
 * ostree_sysroot_deployment_set_kargs:
 * @self: Sysroot
 * @deployment: A deployment
 * @new_kargs: (array zero-terminated=1) (element-type utf8): Replace deployment's kernel arguments
 * @cancellable: Cancellable
 * @error: Error
 *
 * Entirely replace the kernel arguments of @deployment with the
 * values in @new_kargs.
 */
gboolean
ostree_sysroot_deployment_set_kargs (OstreeSysroot *self, OstreeDeployment *deployment,
                                     char **new_kargs, GCancellable *cancellable, GError **error)
{
  if (!_ostree_sysroot_ensure_writable (self, error))
    return FALSE;

  /* For now; instead of this do a redeployment */
  g_assert (!ostree_deployment_is_staged (deployment));

  g_autoptr (OstreeDeployment) new_deployment = ostree_deployment_clone (deployment);
  OstreeBootconfigParser *new_bootconfig = ostree_deployment_get_bootconfig (new_deployment);

  g_autoptr (OstreeKernelArgs) kargs = ostree_kernel_args_new ();
  ostree_kernel_args_append_argv (kargs, new_kargs);
  g_autofree char *new_options = ostree_kernel_args_to_string (kargs);
  ostree_bootconfig_parser_set (new_bootconfig, "options", new_options);

  g_autoptr (GPtrArray) new_deployments = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < self->deployments->len; i++)
    {
      OstreeDeployment *cur = self->deployments->pdata[i];
      if (cur == deployment)
        g_ptr_array_add (new_deployments, g_object_ref (new_deployment));
      else
        g_ptr_array_add (new_deployments, g_object_ref (cur));
    }

  if (!ostree_sysroot_write_deployments (self, new_deployments, cancellable, error))
    return FALSE;

  return TRUE;
}

/**
 * ostree_sysroot_deployment_set_kargs_in_place:
 * @self: Sysroot
 * @deployment: A deployment
 * @kargs_str: (allow-none): Replace @deployment's kernel arguments
 * @cancellable: Cancellable
 * @error: Error
 *
 * Replace the kernel arguments of @deployment with the values in @kargs_str.
 */
gboolean
ostree_sysroot_deployment_set_kargs_in_place (OstreeSysroot *self, OstreeDeployment *deployment,
                                              char *kargs_str, GCancellable *cancellable,
                                              GError **error)
{
  if (!ostree_sysroot_initialize (self, error))
    return FALSE;
  if (!_ostree_sysroot_ensure_boot_fd (self, error))
    return FALSE;
  if (!_ostree_sysroot_ensure_writable (self, error))
    return FALSE;

  // handle staged deployment
  if (ostree_deployment_is_staged (deployment))
    {
      /* Read the staged state from disk */
      glnx_autofd int fd = -1;
      if (!glnx_openat_rdonly (AT_FDCWD, _OSTREE_SYSROOT_RUNSTATE_STAGED, TRUE, &fd, error))
        return FALSE;

      g_autoptr (GBytes) contents = ot_fd_readall_or_mmap (fd, 0, error);
      if (!contents)
        return FALSE;
      g_autoptr (GVariant) staged_deployment_data
          = g_variant_new_from_bytes ((GVariantType *)"a{sv}", contents, TRUE);
      g_autoptr (GVariantDict) staged_deployment_dict = g_variant_dict_new (staged_deployment_data);

      g_autoptr (OstreeKernelArgs) kargs = ostree_kernel_args_from_string (kargs_str);
      g_auto (GStrv) kargs_strv = ostree_kernel_args_to_strv (kargs);

      g_variant_dict_insert (staged_deployment_dict, "kargs", "^a&s", kargs_strv);
      g_autoptr (GVariant) new_staged_deployment_data = g_variant_dict_end (staged_deployment_dict);

      if (!glnx_file_replace_contents_at (fd, _OSTREE_SYSROOT_RUNSTATE_STAGED,
                                          g_variant_get_data (new_staged_deployment_data),
                                          g_variant_get_size (new_staged_deployment_data),
                                          GLNX_FILE_REPLACE_NODATASYNC, cancellable, error))
        return FALSE;
    }
  else
    {
      OstreeBootconfigParser *new_bootconfig = ostree_deployment_get_bootconfig (deployment);
      ostree_bootconfig_parser_set (new_bootconfig, "options", kargs_str);

      g_autofree char *bootconf_name = g_strdup_printf (
          "ostree-%d-%s.conf", self->deployments->len - ostree_deployment_get_index (deployment),
          ostree_deployment_get_osname (deployment));

      g_autofree char *bootconfdir = g_strdup_printf ("loader.%d/entries", self->bootversion);
      glnx_autofd int bootconf_dfd = -1;
      if (!glnx_opendirat (self->boot_fd, bootconfdir, TRUE, &bootconf_dfd, error))
        return FALSE;

      if (!ostree_bootconfig_parser_write_at (new_bootconfig, bootconf_dfd, bootconf_name,
                                              cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/**
 * ostree_sysroot_deployment_set_mutable:
 * @self: Sysroot
 * @deployment: A deployment
 * @is_mutable: Whether or not deployment's files can be changed
 * @cancellable: Cancellable
 * @error: Error
 *
 * By default, deployment directories are not mutable.  This function
 * will allow making them temporarily mutable, for example to allow
 * layering additional non-OSTree content.
 */
gboolean
ostree_sysroot_deployment_set_mutable (OstreeSysroot *self, OstreeDeployment *deployment,
                                       gboolean is_mutable, GCancellable *cancellable,
                                       GError **error)
{
  if (!_ostree_sysroot_ensure_writable (self, error))
    return FALSE;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  g_autofree char *deployment_path = ostree_sysroot_get_deployment_dirpath (self, deployment);
  glnx_autofd int fd = -1;
  if (!glnx_opendirat (self->sysroot_fd, deployment_path, TRUE, &fd, error))
    return FALSE;

  if (!_ostree_linuxfs_fd_alter_immutable_flag (fd, !is_mutable, cancellable, error))
    return FALSE;

  return TRUE;
}
