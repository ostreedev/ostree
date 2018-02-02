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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib-unix.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <linux/fs.h>
#include <err.h>

#ifdef HAVE_LIBMOUNT
#include <libmount.h>
#endif
#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-journal.h>
#endif

#include "otutil.h"
#include "ostree.h"
#include "ostree-sysroot-private.h"
#include "ostree-sepolicy-private.h"
#include "ostree-deployment-private.h"
#include "ostree-core-private.h"
#include "ostree-linuxfsutil.h"
#include "libglnx.h"

#ifdef HAVE_LIBSYSTEMD
#define OSTREE_VARRELABEL_ID          SD_ID128_MAKE(da,67,9b,08,ac,d3,45,04,b7,89,d9,6f,81,8e,a7,81)
#define OSTREE_CONFIGMERGE_ID         SD_ID128_MAKE(d3,86,3b,ae,c1,3e,44,49,ab,03,84,68,4a,8a,f3,a7)
#define OSTREE_DEPLOYMENT_COMPLETE_ID SD_ID128_MAKE(dd,44,0e,3e,54,90,83,b6,3d,0e,fc,7d,c1,52,55,f1)
#endif

/*
 * Like symlinkat() but overwrites (atomically) an existing
 * symlink.
 */
static gboolean
symlink_at_replace (const char    *oldpath,
                    int            parent_dfd,
                    const char    *newpath,
                    GCancellable  *cancellable,
                    GError       **error)
{
  /* Possibly in the future generate a temporary random name here,
   * would need to move "generate a temporary name" code into
   * libglnx or glib?
   */
  g_autofree char *temppath = g_strconcat (newpath, ".tmp", NULL);

  /* Clean up any stale temporary links */
  (void) unlinkat (parent_dfd, temppath, 0);

  /* Create the temp link */
  if (TEMP_FAILURE_RETRY (symlinkat (oldpath, parent_dfd, temppath)) < 0)
    return glnx_throw_errno_prefix (error, "symlinkat");

  /* Rename it into place */
  if (!glnx_renameat (parent_dfd, temppath, parent_dfd, newpath, error))
    return FALSE;

  return TRUE;
}

static GLnxFileCopyFlags
sysroot_flags_to_copy_flags (GLnxFileCopyFlags defaults,
                             OstreeSysrootDebugFlags sysrootflags)
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
install_into_boot (OstreeSePolicy *sepolicy,
                   int         src_dfd,
                   const char *src_subpath,
                   int         dest_dfd,
                   const char *dest_subpath,
                   OstreeSysrootDebugFlags flags,
                   GCancellable  *cancellable,
                   GError       **error)
{
  if (linkat (src_dfd, src_subpath, dest_dfd, dest_subpath, 0) != 0)
    {
      if (G_IN_SET (errno, EMLINK, EXDEV))
        {
          /* Be sure we relabel when copying the kernel, as in current
           * e.g. Fedora it might be labeled module_object_t or usr_t,
           * but policy may not allow other processes to read from that
           * like kdump.
           * See also https://github.com/fedora-selinux/selinux-policy/commit/747f4e6775d773ab74efae5aa37f3e5e7f0d4aca
           * This means we also drop xattrs but...I doubt anyone uses
           * non-SELinux xattrs for the kernel anyways aside from perhaps
           * IMA but that's its own story.
           */
          g_auto(OstreeSepolicyFsCreatecon) fscreatecon = { 0, };
          const char *boot_path = glnx_strjoina ("/boot/", glnx_basename (dest_subpath));
          if (!_ostree_sepolicy_preparefscreatecon (&fscreatecon, sepolicy,
                                                    boot_path, S_IFREG | 0644,
                                                    error))
            return FALSE;
          return glnx_file_copy_at (src_dfd, src_subpath, NULL, dest_dfd, dest_subpath,
                                    GLNX_FILE_COPY_NOXATTRS,
                                    cancellable, error);
        }
      else
        return glnx_throw_errno_prefix (error, "linkat(%s)", dest_subpath);
    }

  return TRUE;
}

/* Copy ownership, mode, and xattrs from source directory to destination */
static gboolean
dirfd_copy_attributes_and_xattrs (int            src_parent_dfd,
                                  const char    *src_name,
                                  int            src_dfd,
                                  int            dest_dfd,
                                  OstreeSysrootDebugFlags flags,
                                  GCancellable  *cancellable,
                                  GError       **error)
{
  g_autoptr(GVariant) xattrs = NULL;

  /* Clone all xattrs first, so we get the SELinux security context
   * right.  This will allow other users access if they have ACLs, but
   * oh well.
   */
  if (!(flags & OSTREE_SYSROOT_DEBUG_NO_XATTRS))
    {
      if (!glnx_dfd_name_get_all_xattrs (src_parent_dfd, src_name,
                                         &xattrs, cancellable, error))
        return FALSE;
      if (!glnx_fd_set_all_xattrs (dest_dfd, xattrs,
                                   cancellable, error))
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

static gboolean
copy_dir_recurse (int              src_parent_dfd,
                  int              dest_parent_dfd,
                  const char      *name,
                  OstreeSysrootDebugFlags flags,
                  GCancellable    *cancellable,
                  GError         **error)
{
  g_auto(GLnxDirFdIterator) src_dfd_iter = { 0, };
  glnx_autofd int dest_dfd = -1;
  struct dirent *dent;

  if (!glnx_dirfd_iterator_init_at (src_parent_dfd, name, TRUE, &src_dfd_iter, error))
    return FALSE;

  /* Create with mode 0700, we'll fchmod/fchown later */
  if (!glnx_ensure_dir (dest_parent_dfd, name, 0700, error))
    return FALSE;

  if (!glnx_opendirat (dest_parent_dfd, name, TRUE, &dest_dfd, error))
    return FALSE;

  if (!dirfd_copy_attributes_and_xattrs (src_parent_dfd, name, src_dfd_iter.fd, dest_dfd,
                                         flags, cancellable, error))
    return FALSE;

  while (TRUE)
    {
      struct stat child_stbuf;

      if (!glnx_dirfd_iterator_next_dent (&src_dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (!glnx_fstatat (src_dfd_iter.fd, dent->d_name, &child_stbuf,
                         AT_SYMLINK_NOFOLLOW, error))
        return FALSE;

      if (S_ISDIR (child_stbuf.st_mode))
        {
          if (!copy_dir_recurse (src_dfd_iter.fd, dest_dfd, dent->d_name,
                                 flags, cancellable, error))
            return FALSE;
        }
      else
        {
          if (!glnx_file_copy_at (src_dfd_iter.fd, dent->d_name, &child_stbuf,
                                  dest_dfd, dent->d_name,
                                  sysroot_flags_to_copy_flags (GLNX_FILE_COPY_OVERWRITE, flags),
                                  cancellable, error))
            return FALSE;
        }
    }

  return TRUE;
}

/* If a chain of directories is added, this function will ensure
 * they're created.
 */
static gboolean
ensure_directory_from_template (int                 orig_etc_fd,
                                int                 modified_etc_fd,
                                int                 new_etc_fd,
                                const char         *path,
                                int                *out_dfd,
                                OstreeSysrootDebugFlags flags,
                                GCancellable       *cancellable,
                                GError            **error)
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

  if (!dirfd_copy_attributes_and_xattrs (modified_etc_fd, path, src_dfd, target_dfd,
                                         flags, cancellable, error))
    return FALSE;

  if (out_dfd)
    *out_dfd = glnx_steal_fd (&target_dfd);
  return TRUE;
}

/* Copy (relative) @path from @modified_etc_fd to @new_etc_fd, overwriting any
 * existing file there. The @path may refer to a regular file, a symbolic link,
 * or a directory. Directories will be copied recursively.
 */
static gboolean
copy_modified_config_file (int                 orig_etc_fd,
                           int                 modified_etc_fd,
                           int                 new_etc_fd,
                           const char         *path,
                           OstreeSysrootDebugFlags flags,
                           GCancellable       *cancellable,
                           GError            **error)
{
  struct stat modified_stbuf;
  struct stat new_stbuf;

  if (!glnx_fstatat (modified_etc_fd, path, &modified_stbuf, AT_SYMLINK_NOFOLLOW, error))
    return glnx_prefix_error (error, "Reading modified config file");

  glnx_autofd int dest_parent_dfd = -1;
  if (strchr (path, '/') != NULL)
    {
      g_autofree char *parent = g_path_get_dirname (path);

      if (!ensure_directory_from_template (orig_etc_fd, modified_etc_fd, new_etc_fd,
                                           parent, &dest_parent_dfd, flags, cancellable, error))
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
  else if (S_ISDIR(new_stbuf.st_mode))
    {
      if (!S_ISDIR(modified_stbuf.st_mode))
        {
          return g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              "Modified config file newly defaults to directory '%s', cannot merge",
                              path), FALSE;
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
      if (!copy_dir_recurse (modified_etc_fd, new_etc_fd, path, flags,
                             cancellable, error))
        return FALSE;
    }
  else if (S_ISLNK (modified_stbuf.st_mode) || S_ISREG (modified_stbuf.st_mode))
    {
      if (!glnx_file_copy_at (modified_etc_fd, path, &modified_stbuf,
                              new_etc_fd, path,
                              sysroot_flags_to_copy_flags (GLNX_FILE_COPY_OVERWRITE, flags),
                              cancellable, error))
        return FALSE;
    }
  else
    {
      return glnx_throw (error,
                         "Unsupported non-regular/non-symlink file in /etc '%s'",
                         path);
    }

  return TRUE;
}

/*
 * merge_configuration_from:
 * @sysroot: Sysroot
 * @merge_deployment: Source of configuration differences
 * @merge_deployment_dfd: Directory fd, may be -1
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
merge_configuration_from (OstreeSysroot    *sysroot,
                          OstreeDeployment *merge_deployment,
                          int               merge_deployment_dfd,
                          OstreeDeployment *new_deployment,
                          int               new_deployment_dfd,
                          GCancellable     *cancellable,
                          GError          **error)
{
  glnx_autofd int owned_merge_deployment_dfd = -1;
  const OstreeSysrootDebugFlags flags = sysroot->debug_flags;

  g_assert (merge_deployment != NULL && new_deployment != NULL);
  g_assert (new_deployment_dfd != -1);

  /* Allow the caller to pass -1 for the merge, for convenience */
  if (merge_deployment_dfd == -1)
    {
      g_autofree char *merge_deployment_path = ostree_sysroot_get_deployment_dirpath (sysroot, merge_deployment);
      if (!glnx_opendirat (sysroot->sysroot_fd, merge_deployment_path, FALSE,
                           &owned_merge_deployment_dfd, error))
        return FALSE;
      merge_deployment_dfd = owned_merge_deployment_dfd;
    }

  /* TODO: get rid of GFile usage here */
  g_autoptr(GFile) orig_etc = ot_fdrel_to_gfile (merge_deployment_dfd, "usr/etc");
  g_autoptr(GFile) modified_etc = ot_fdrel_to_gfile (merge_deployment_dfd, "etc");
  /* Return values for below */
  g_autoptr(GPtrArray) modified = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_diff_item_unref);
  g_autoptr(GPtrArray) removed = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  g_autoptr(GPtrArray) added = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  /* For now, ignore changes to xattrs; the problem is that
   * security.selinux will be different between the /usr/etc labels
   * and the ones in the real /etc, so they all show up as different.
   *
   * This means that if you want to change the security context of a
   * file, to have that change persist across upgrades, you must also
   * modify the content of the file.
   */
  if (!ostree_diff_dirs (OSTREE_DIFF_FLAGS_IGNORE_XATTRS,
                         orig_etc, modified_etc, modified, removed, added,
                         cancellable, error))
    return glnx_prefix_error (error, "While computing configuration diff");

  { g_autofree char *msg =
      g_strdup_printf ("Copying /etc changes: %u modified, %u removed, %u added",
                       modified->len, removed->len, added->len);
#ifdef HAVE_LIBSYSTEMD
    sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(OSTREE_CONFIGMERGE_ID),
                     "MESSAGE=%s", msg,
                     "ETC_N_MODIFIED=%u", modified->len,
                     "ETC_N_REMOVED=%u", removed->len,
                     "ETC_N_ADDED=%u", added->len,
                     NULL);
#endif
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

      if (!copy_modified_config_file (orig_etc_fd, modified_etc_fd, new_etc_fd, path,
                                      flags, cancellable, error))
        return FALSE;
    }
  for (guint i = 0; i < added->len; i++)
    {
      GFile *file = added->pdata[i];
      g_autofree char *path = g_file_get_relative_path (modified_etc, file);

      g_assert (path);

      if (!copy_modified_config_file (orig_etc_fd, modified_etc_fd, new_etc_fd, path,
                                      flags, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/* Look up @revision in the repository, and check it out in
 * /ostree/deploy/OS/deploy/${treecsum}.${deployserial}.
 * A dfd for the result is returned in @out_deployment_dfd.
 */
static gboolean
checkout_deployment_tree (OstreeSysroot     *sysroot,
                          OstreeRepo        *repo,
                          OstreeDeployment  *deployment,
                          int               *out_deployment_dfd,
                          GCancellable      *cancellable,
                          GError           **error)
{
  gboolean ret = FALSE;
  OstreeRepoCheckoutAtOptions checkout_opts = { 0, };
  const char *csum = ostree_deployment_get_csum (deployment);
  g_autofree char *checkout_target_name = NULL;
  g_autofree char *osdeploy_path = NULL;
  glnx_autofd int osdeploy_dfd = -1;
  int ret_fd;

  osdeploy_path = g_strconcat ("ostree/deploy/", ostree_deployment_get_osname (deployment), "/deploy", NULL);
  checkout_target_name = g_strdup_printf ("%s.%d", csum, ostree_deployment_get_deployserial (deployment));

  if (!glnx_shutil_mkdir_p_at (sysroot->sysroot_fd, osdeploy_path, 0775, cancellable, error))
    goto out;

  if (!glnx_opendirat (sysroot->sysroot_fd, osdeploy_path, TRUE, &osdeploy_dfd, error))
    goto out;

  if (!glnx_shutil_rm_rf_at (osdeploy_dfd, checkout_target_name, cancellable, error))
    goto out;

  if (!ostree_repo_checkout_at (repo, &checkout_opts, osdeploy_dfd,
                                checkout_target_name, csum,
                                cancellable, error))
    goto out;

  if (!glnx_opendirat (osdeploy_dfd, checkout_target_name, TRUE, &ret_fd, error))
    goto out;

  ret = TRUE;
  *out_deployment_dfd = ret_fd;
 out:
  return ret;
}

static char *
ptrarray_path_join (GPtrArray  *path)
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
relabel_one_path (OstreeSysroot  *sysroot,
                  OstreeSePolicy *sepolicy,
                  GFile         *path,
                  GFileInfo     *info,
                  GPtrArray     *path_parts,
                  GCancellable   *cancellable,
                  GError        **error)
{
  gboolean ret = FALSE;
  g_autofree char *relpath = NULL;

  relpath = ptrarray_path_join (path_parts);
  if (!ostree_sepolicy_restorecon (sepolicy, relpath,
                                   info, path,
                                   OSTREE_SEPOLICY_RESTORECON_FLAGS_ALLOW_NOLABEL,
                                   NULL,
                                   cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
relabel_recursively (OstreeSysroot  *sysroot,
                     OstreeSePolicy *sepolicy,
                     GFile          *dir,
                     GFileInfo      *dir_info,
                     GPtrArray      *path_parts,
                     GCancellable   *cancellable,
                     GError        **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFileEnumerator) direnum = NULL;

  if (!relabel_one_path (sysroot, sepolicy, dir, dir_info, path_parts,
                         cancellable, error))
    goto out;

  direnum = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO,
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       cancellable, error);
  if (!direnum)
    goto out;

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *child;
      GFileType ftype;

      if (!g_file_enumerator_iterate (direnum, &file_info, &child,
                                      cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      g_ptr_array_add (path_parts, (char*)g_file_info_get_name (file_info));

      ftype = g_file_info_get_file_type (file_info);
      if (ftype == G_FILE_TYPE_DIRECTORY)
        {
          if (!relabel_recursively (sysroot, sepolicy, child, file_info, path_parts,
                                    cancellable, error))
            goto out;
        }
      else
        {
          if (!relabel_one_path (sysroot, sepolicy, child, file_info, path_parts,
                                 cancellable, error))
            goto out;
        }

      g_ptr_array_remove_index (path_parts, path_parts->len - 1);
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
selinux_relabel_dir (OstreeSysroot                 *sysroot,
                     OstreeSePolicy                *sepolicy,
                     GFile                         *dir,
                     const char                    *prefix,
                     GCancellable                  *cancellable,
                     GError                       **error)
{
  gboolean ret = FALSE;
  g_autoptr(GPtrArray) path_parts = g_ptr_array_new ();
  g_autoptr(GFileInfo) root_info = NULL;

  root_info = g_file_query_info (dir, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!root_info)
    goto out;

  g_ptr_array_add (path_parts, (char*)prefix);
  if (!relabel_recursively (sysroot, sepolicy, dir, root_info, path_parts,
                            cancellable, error))
    {
      g_prefix_error (error, "Relabeling /%s: ", prefix);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/* Handles SELinux labeling for /var; this is slated to be deleted.  See
 * https://github.com/ostreedev/ostree/pull/872
 */
static gboolean
selinux_relabel_var_if_needed (OstreeSysroot                 *sysroot,
                               OstreeSePolicy                *sepolicy,
                               int                            os_deploy_dfd,
                               GCancellable                  *cancellable,
                               GError                       **error)
{
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
      { g_autofree char *msg =
          g_strdup_printf ("Relabeling /var (no stamp file '%s' found)", selabeled);
#ifdef HAVE_LIBSYSTEMD
        sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(OSTREE_VARRELABEL_ID),
                         "MESSAGE=%s", msg,
                         NULL);
#endif
        _ostree_sysroot_emit_journal_msg (sysroot, msg);
      }

      g_autoptr(GFile) deployment_var_path = ot_fdrel_to_gfile (os_deploy_dfd, "var");
      if (!selinux_relabel_dir (sysroot, sepolicy,
                                deployment_var_path, "var",
                                cancellable, error))
        {
          g_prefix_error (error, "Relabeling /var: ");
          return FALSE;
        }

      { g_auto(OstreeSepolicyFsCreatecon) con = { 0, };
        const char *selabeled_abspath = glnx_strjoina ("/", selabeled);

        if (!_ostree_sepolicy_preparefscreatecon (&con, sepolicy,
                                                  selabeled_abspath,
                                                  0644, error))
          return FALSE;

        if (!glnx_file_replace_contents_at (os_deploy_dfd, selabeled, (guint8*)"", 0,
                                            GLNX_FILE_REPLACE_DATASYNC_NEW,
                                            cancellable, error))
          return FALSE;
      }
    }

  return TRUE;
}

/* OSTree implements a "3 way" merge model for /etc. For a bit more information
 * on this, see the manual. This function uses the configuration for
 * @previous_deployment, and writes the merged configuration into @deployment's
 * /etc.  If available, we also load the SELinux policy from the new root.
 */
static gboolean
merge_configuration (OstreeSysroot         *sysroot,
                     OstreeRepo            *repo,
                     OstreeDeployment      *previous_deployment,
                     OstreeDeployment      *deployment,
                     int                    deployment_dfd,
                     OstreeSePolicy       **out_sepolicy,
                     GCancellable          *cancellable,
                     GError               **error)
{
  g_autoptr(OstreeSePolicy) sepolicy = NULL;

  if (previous_deployment)
    {
      g_autoptr(GFile) previous_path = NULL;
      OstreeBootconfigParser *previous_bootconfig;

      previous_path = ostree_sysroot_get_deployment_directory (sysroot, previous_deployment);

      previous_bootconfig = ostree_deployment_get_bootconfig (previous_deployment);
      if (previous_bootconfig)
        {
          const char *previous_options = ostree_bootconfig_parser_get (previous_bootconfig, "options");
          /* Completely overwrite the previous options here; we will extend
           * them later.
           */
          ostree_bootconfig_parser_set (ostree_deployment_get_bootconfig (deployment), "options",
                                        previous_options);
        }
    }

  struct stat stbuf;
  if (!glnx_fstatat_allow_noent (deployment_dfd, "etc", &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  gboolean etc_exists = (errno == 0);
  if (!glnx_fstatat_allow_noent (deployment_dfd, "usr/etc", &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  gboolean usretc_exists = (errno == 0);

  if (etc_exists && usretc_exists)
    return glnx_throw (error, "Tree contains both /etc and /usr/etc");
  else if (etc_exists)
    {
      /* Compatibility hack */
      if (!glnx_renameat (deployment_dfd, "etc", deployment_dfd, "usr/etc", error))
        return FALSE;
      usretc_exists = TRUE;
      etc_exists = FALSE;
    }

  if (usretc_exists)
    {
      /* We need copies of /etc from /usr/etc (so admins can use vi), and if
       * SELinux is enabled, we need to relabel.
       */
      OstreeRepoCheckoutAtOptions etc_co_opts = { .force_copy = TRUE,
                                                  .subpath = "/usr/etc",
                                                  .sepolicy_prefix = "/etc"};

      /* Here, we initialize SELinux policy from the /usr/etc inside
       * the root - this is before we've finalized the configuration
       * merge into /etc. */
      sepolicy = ostree_sepolicy_new_at (deployment_dfd, cancellable, error);
      if (!sepolicy)
        return FALSE;
      if (ostree_sepolicy_get_name (sepolicy) != NULL)
        etc_co_opts.sepolicy = sepolicy;

      /* Copy usr/etc → etc */
      if (!ostree_repo_checkout_at (repo, &etc_co_opts,
                                    deployment_dfd, "etc",
                                    ostree_deployment_get_csum (deployment),
                                    cancellable, error))
        return FALSE;

    }

  if (previous_deployment)
    {
      if (!merge_configuration_from (sysroot, previous_deployment, -1,
                                     deployment, deployment_dfd,
                                     cancellable, error))
        return FALSE;
    }

  if (out_sepolicy)
    *out_sepolicy = g_steal_pointer (&sepolicy);
  return TRUE;
}

/* Write the origin file for a deployment. */
static gboolean
write_origin_file_internal (OstreeSysroot         *sysroot,
                            OstreeDeployment      *deployment,
                            GKeyFile              *new_origin,
                            GLnxFileReplaceFlags   flags,
                            GCancellable          *cancellable,
                            GError               **error)
{
  GKeyFile *origin =
    new_origin ? new_origin : ostree_deployment_get_origin (deployment);

  if (origin)
    {
      g_autofree char *origin_path = NULL;
      g_autofree char *contents = NULL;
      gsize len;

      origin_path = g_strdup_printf ("ostree/deploy/%s/deploy/%s.%d.origin",
                                     ostree_deployment_get_osname (deployment),
                                     ostree_deployment_get_csum (deployment),
                                     ostree_deployment_get_deployserial (deployment));

      contents = g_key_file_to_data (origin, &len, error);
      if (!contents)
        return FALSE;

      if (!glnx_file_replace_contents_at (sysroot->sysroot_fd,
                                          origin_path, (guint8*)contents, len,
                                          flags,
                                          cancellable, error))
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
ostree_sysroot_write_origin_file (OstreeSysroot         *sysroot,
                                  OstreeDeployment      *deployment,
                                  GKeyFile              *new_origin,
                                  GCancellable          *cancellable,
                                  GError               **error)
{
  return write_origin_file_internal (sysroot, deployment, new_origin,
                                     GLNX_FILE_REPLACE_DATASYNC_NEW,
                                     cancellable, error);
}

typedef struct {
  int   boot_dfd;
  char *kernel_srcpath;
  char *kernel_namever;
  char *initramfs_srcpath;
  char *initramfs_namever;
  char *devicetree_srcpath;
  char *devicetree_namever;
  char *bootcsum;
} OstreeKernelLayout;
static void
_ostree_kernel_layout_free (OstreeKernelLayout *layout)
{
  glnx_close_fd (&layout->boot_dfd);
  g_free (layout->kernel_srcpath);
  g_free (layout->kernel_namever);
  g_free (layout->initramfs_srcpath);
  g_free (layout->initramfs_namever);
  g_free (layout->devicetree_srcpath);
  g_free (layout->devicetree_namever);
  g_free (layout->bootcsum);
  g_free (layout);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OstreeKernelLayout, _ostree_kernel_layout_free);

static OstreeKernelLayout*
_ostree_kernel_layout_new (void)
{
  OstreeKernelLayout *ret = g_new0 (OstreeKernelLayout, 1);
  ret->boot_dfd = -1;
  return ret;
}

/* See get_kernel_from_tree() below */
static gboolean
get_kernel_from_tree_usrlib_modules (int                  deployment_dfd,
                                     OstreeKernelLayout **out_layout,
                                     GCancellable        *cancellable,
                                     GError             **error)
{
  g_autofree char *kver = NULL;
  /* Look in usr/lib/modules */
  g_auto(GLnxDirFdIterator) mod_dfditer = { 0, };
  gboolean exists;
  if (!ot_dfd_iter_init_allow_noent (deployment_dfd, "usr/lib/modules", &mod_dfditer,
                                     &exists, error))
    return FALSE;
  if (!exists)
    {
      /* No usr/lib/modules?  We're done */
      *out_layout = NULL;
      return TRUE;
    }

  g_autoptr(OstreeKernelLayout) ret_layout = _ostree_kernel_layout_new ();

  /* Reusable buffer for path string */
  g_autoptr(GString) pathbuf = g_string_new ("");
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
  g_auto(OtChecksum) checksum = { 0, };
  ot_checksum_init (&checksum);
  glnx_autofd int fd = -1;
  /* Checksum the kernel */
  if (!glnx_openat_rdonly (ret_layout->boot_dfd, "vmlinuz", TRUE, &fd, error))
    return FALSE;
  g_autoptr(GInputStream) in = g_unix_input_stream_new (fd, FALSE);
  if (!ot_gio_splice_update_checksum (NULL, in, &checksum, cancellable, error))
    return FALSE;
  g_clear_object (&in);
  glnx_close_fd (&fd);

  /* Look for an initramfs, but it's optional; since there wasn't any precedent
   * for this, let's be a bit conservative and support both `initramfs.img` and
   * `initramfs`.
   */
  const char *initramfs_paths[] = {"initramfs.img", "initramfs"};
  const char *initramfs_path = NULL;
  for (guint i = 0; i < G_N_ELEMENTS(initramfs_paths); i++)
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

  g_clear_object (&in);
  glnx_close_fd (&fd);

  char hexdigest[OSTREE_SHA256_STRING_LEN+1];
  ot_checksum_get_hexdigest (&checksum, hexdigest, sizeof (hexdigest));
  ret_layout->bootcsum = g_strdup (hexdigest);

  *out_layout = g_steal_pointer (&ret_layout);
  return TRUE;
}

/* See get_kernel_from_tree() below */
static gboolean
get_kernel_from_tree_legacy_layouts (int                  deployment_dfd,
                                     OstreeKernelLayout **out_layout,
                                     GCancellable        *cancellable,
                                     GError             **error)
{
  const char *legacy_paths[] = {"usr/lib/ostree-boot", "boot"};
  g_autofree char *kernel_checksum = NULL;
  g_autofree char *initramfs_checksum = NULL;
  g_autofree char *devicetree_checksum = NULL;
  g_autoptr(OstreeKernelLayout) ret_layout = _ostree_kernel_layout_new ();

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
  g_auto(GLnxDirFdIterator) dfditer = { 0, };
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
      if (ret_layout->kernel_srcpath != NULL &&
          ret_layout->initramfs_srcpath != NULL &&
          ret_layout->devicetree_srcpath != NULL)
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
get_kernel_from_tree (int                  deployment_dfd,
                      OstreeKernelLayout **out_layout,
                      GCancellable        *cancellable,
                      GError             **error)
{
  g_autoptr(OstreeKernelLayout) usrlib_modules_layout = NULL;
  g_autoptr(OstreeKernelLayout) legacy_layout = NULL;

  /* First, gather from usr/lib/modules/$kver if it exists */
  if (!get_kernel_from_tree_usrlib_modules (deployment_dfd, &usrlib_modules_layout, cancellable, error))
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
  else if (usrlib_modules_layout != NULL &&
           usrlib_modules_layout->initramfs_srcpath == NULL &&
           legacy_layout->initramfs_srcpath != NULL)
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
fsfreeze_thaw_cycle (OstreeSysroot *self,
                     int            rootfs_dfd,
                     GCancellable *cancellable,
                     GError       **error)
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

  const gboolean debug_fifreeze = (self->debug_flags & OSTREE_SYSROOT_DEBUG_TEST_FIFREEZE)>0;
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
          (void) TEMP_FAILURE_RETRY (ioctl (rootfs_dfd, FITHAW, 0));
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
      if (ioctl (rootfs_dfd, FIFREEZE, 0) != 0)
        {
          /* Not supported, we're running in the unit tests (as non-root), or
           * the filesystem is already frozen (EBUSY).
           * OK, let's just do a syncfs.
           */
          if (G_IN_SET (errno, EOPNOTSUPP, EPERM, EBUSY))
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
      if (TEMP_FAILURE_RETRY (ioctl (rootfs_dfd, FITHAW, 0)) != 0)
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

typedef struct {
  guint64 root_syncfs_msec;
  guint64 boot_syncfs_msec;
  guint64 extra_syncfs_msec;
} SyncStats;

/* First, sync the root directory as well as /var and /boot which may
 * be separate mount points.  Then *in addition*, do a global
 * `sync()`.
 */
static gboolean
full_system_sync (OstreeSysroot     *self,
                  SyncStats         *out_stats,
                  GCancellable      *cancellable,
                  GError           **error)
{
  guint64 start_msec = g_get_monotonic_time () / 1000;
  if (syncfs (self->sysroot_fd) != 0)
    return glnx_throw_errno_prefix (error, "syncfs(sysroot)");
  guint64 end_msec = g_get_monotonic_time () / 1000;

  out_stats->root_syncfs_msec = (end_msec - start_msec);

  start_msec = g_get_monotonic_time () / 1000;
  glnx_autofd int boot_dfd = -1;
  if (!glnx_opendirat (self->sysroot_fd, "boot", TRUE, &boot_dfd, error))
    return FALSE;
  if (!fsfreeze_thaw_cycle (self, boot_dfd, cancellable, error))
    return FALSE;
  end_msec = g_get_monotonic_time () / 1000;
  out_stats->boot_syncfs_msec = (end_msec - start_msec);

  /* And now out of an excess of conservativism, we still invoke
   * sync().  The advantage of still using `syncfs()` above is that we
   * actually get error codes out of that API, and we more clearly
   * delineate what we actually want to sync in the future when this
   * global sync call is removed.
   */
  start_msec = g_get_monotonic_time () / 1000;
  sync ();
  end_msec = g_get_monotonic_time () / 1000;
  out_stats->extra_syncfs_msec = (end_msec - start_msec);

  return TRUE;
}

/* Write out the "bootlinks", which are symlinks pointing to deployments.
 * We might be generating a new bootversion (i.e. updating the bootloader config),
 * or we might just be generating a "sub-bootversion".
 *
 * These new links are made active by swap_bootlinks().
 */
static gboolean
create_new_bootlinks (OstreeSysroot *self,
                      int            bootversion,
                      GPtrArray     *new_deployments,
                       GCancellable  *cancellable,
                      GError       **error)
{
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
  g_autofree char *ostree_subbootdir_name = g_strdup_printf ("boot.%d.%d", bootversion, new_subbootversion);
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
      g_autofree char *bootlink_parent = g_strconcat (ostree_deployment_get_osname (deployment),
                                                      "/",
                                                      ostree_deployment_get_bootcsum (deployment),
                                                      NULL);
      g_autofree char *bootlink_pathname = g_strdup_printf ("%s/%d", bootlink_parent, ostree_deployment_get_bootserial (deployment));
      g_autofree char *bootlink_target = g_strdup_printf ("../../../deploy/%s/deploy/%s.%d",
                                                          ostree_deployment_get_osname (deployment),
                                                          ostree_deployment_get_csum (deployment),
                                                          ostree_deployment_get_deployserial (deployment));

      if (!glnx_shutil_mkdir_p_at (ostree_subbootdir_dfd, bootlink_parent, 0755, cancellable, error))
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
swap_bootlinks (OstreeSysroot *self,
                int            bootversion,
                GPtrArray     *new_deployments,
                GCancellable  *cancellable,
                GError       **error)
{
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
  g_autofree char *ostree_subbootdir_name = g_strdup_printf ("boot.%d.%d", bootversion, new_subbootversion);
  if (!symlink_at_replace (ostree_subbootdir_name, ostree_dfd, ostree_bootdir_name,
                           cancellable, error))
    return FALSE;

  return TRUE;
}

static GHashTable *
parse_os_release (const char *contents,
                  const char *split)
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
install_deployment_kernel (OstreeSysroot   *sysroot,
                           OstreeRepo      *repo,
                           int             new_bootversion,
                           OstreeDeployment   *deployment,
                           guint           n_deployments,
                           gboolean        show_osname,
                           GCancellable   *cancellable,
                           GError        **error)

{
  OstreeBootconfigParser *bootconfig = ostree_deployment_get_bootconfig (deployment);
  g_autofree char *deployment_dirpath = ostree_sysroot_get_deployment_dirpath (sysroot, deployment);
  glnx_autofd int deployment_dfd = -1;
  if (!glnx_opendirat (sysroot->sysroot_fd, deployment_dirpath, FALSE,
                       &deployment_dfd, error))
    return FALSE;

  /* We need to label the kernels */
  g_autoptr(OstreeSePolicy) sepolicy = ostree_sepolicy_new_at (deployment_dfd, cancellable, error);
  if (!sepolicy)
    return FALSE;

  /* Find the kernel/initramfs/devicetree in the tree */
  g_autoptr(OstreeKernelLayout) kernel_layout = NULL;
  if (!get_kernel_from_tree (deployment_dfd, &kernel_layout,
                             cancellable, error))
    return FALSE;

  glnx_autofd int boot_dfd = -1;
  if (!glnx_opendirat (sysroot->sysroot_fd, "boot", TRUE, &boot_dfd, error))
    return FALSE;

  const char *osname = ostree_deployment_get_osname (deployment);
  const char *bootcsum = ostree_deployment_get_bootcsum (deployment);
  g_assert_cmpstr (kernel_layout->bootcsum, ==, bootcsum);
  g_autofree char *bootcsumdir = g_strdup_printf ("ostree/%s-%s", osname, bootcsum);
  g_autofree char *bootconfdir = g_strdup_printf ("loader.%d/entries", new_bootversion);
  g_autofree char *bootconf_name = g_strdup_printf ("ostree-%s-%d.conf", osname,
                                   ostree_deployment_get_index (deployment));
  if (!glnx_shutil_mkdir_p_at (boot_dfd, bootcsumdir, 0775, cancellable, error))
    return FALSE;

  glnx_autofd int bootcsum_dfd = -1;
  if (!glnx_opendirat (boot_dfd, bootcsumdir, TRUE, &bootcsum_dfd, error))
    return FALSE;

  if (!glnx_shutil_mkdir_p_at (boot_dfd, bootconfdir, 0775, cancellable, error))
    return FALSE;

  /* Install (hardlink/copy) the kernel into /boot/ostree/osname-${bootcsum} if
   * it doesn't exist already.
   */
  struct stat stbuf;
  if (!glnx_fstatat_allow_noent (bootcsum_dfd, kernel_layout->kernel_namever, &stbuf, 0, error))
    return FALSE;
  if (errno == ENOENT)
    {
      if (!install_into_boot (sepolicy, kernel_layout->boot_dfd, kernel_layout->kernel_srcpath,
                              bootcsum_dfd, kernel_layout->kernel_namever,
                              sysroot->debug_flags,
                              cancellable, error))
        return FALSE;
    }

  /* If we have an initramfs, then install it into
   * /boot/ostree/osname-${bootcsum} if it doesn't exist already.
   */
  if (kernel_layout->initramfs_srcpath)
    {
      g_assert (kernel_layout->initramfs_namever);
      if (!glnx_fstatat_allow_noent (bootcsum_dfd, kernel_layout->initramfs_namever, &stbuf, 0, error))
        return FALSE;
      if (errno == ENOENT)
        {
          if (!install_into_boot (sepolicy, kernel_layout->boot_dfd, kernel_layout->initramfs_srcpath,
                                  bootcsum_dfd, kernel_layout->initramfs_namever,
                                  sysroot->debug_flags,
                                  cancellable, error))
            return FALSE;
        }
    }

  if (kernel_layout->devicetree_srcpath)
    {
      g_assert (kernel_layout->devicetree_namever);
      if (!glnx_fstatat_allow_noent (bootcsum_dfd, kernel_layout->devicetree_namever, &stbuf, 0, error))
        return FALSE;
      if (errno == ENOENT)
        {
          if (!install_into_boot (sepolicy, kernel_layout->boot_dfd, kernel_layout->devicetree_srcpath,
                                  bootcsum_dfd, kernel_layout->devicetree_namever,
                                  sysroot->debug_flags,
                                  cancellable, error))
            return FALSE;
        }
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

  g_autoptr(GHashTable) osrelease_values = parse_os_release (contents, "\n");
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
      g_autoptr(GVariant) variant = NULL;
      g_autoptr(GVariant) metadata = NULL;

      /* XXX Copying ot_admin_checksum_version() + bits from
       *     ot-admin-builtin-status.c.  Maybe this should be
       *     public API in libostree? */
      if (ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, csum,
                                    &variant, NULL))
        {
          metadata = g_variant_get_child_value (variant, 0);
          g_variant_lookup (metadata, OSTREE_COMMIT_META_KEY_VERSION, "s", &deployment_version);
        }
    }

  /* XXX The SYSLINUX bootloader backend actually parses the title string
   *     (specifically, it looks for the substring "(ostree"), so further
   *     changes to the title format may require updating that backend. */
  g_autoptr(GString) title_key = g_string_new (val);
  if (deployment_version && *deployment_version)
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
  if (!(deployment_version && *deployment_version))
    {
      g_string_append_printf (title_key, ":%d", ostree_deployment_get_index (deployment));
    }
  g_string_append_c (title_key, ')');
  ostree_bootconfig_parser_set (bootconfig, "title", title_key->str);

  g_autofree char *version_key = g_strdup_printf ("%d", n_deployments - ostree_deployment_get_index (deployment));
  ostree_bootconfig_parser_set (bootconfig, OSTREE_COMMIT_META_KEY_VERSION, version_key);
  g_autofree char * boot_relpath = g_strconcat ("/", bootcsumdir, "/", kernel_layout->kernel_namever, NULL);
  ostree_bootconfig_parser_set (bootconfig, "linux", boot_relpath);

  val = ostree_bootconfig_parser_get (bootconfig, "options");
  g_autoptr(OstreeKernelArgs) kargs = _ostree_kernel_args_from_string (val);

  if (kernel_layout->initramfs_namever)
    {
      g_autofree char * boot_relpath = g_strconcat ("/", bootcsumdir, "/", kernel_layout->initramfs_namever, NULL);
      ostree_bootconfig_parser_set (bootconfig, "initrd", boot_relpath);
    }
  else
    {
      g_autofree char *prepare_root_arg = NULL;
      prepare_root_arg = g_strdup_printf ("init=/ostree/boot.%d/%s/%s/%d/usr/lib/ostree/ostree-prepare-root",
                                             new_bootversion, osname, bootcsum,
                                             ostree_deployment_get_bootserial (deployment));
      _ostree_kernel_args_replace_take (kargs, g_steal_pointer (&prepare_root_arg));
    }

  if (kernel_layout->devicetree_namever)
    {
      g_autofree char * boot_relpath = g_strconcat ("/", bootcsumdir, "/", kernel_layout->devicetree_namever, NULL);
      ostree_bootconfig_parser_set (bootconfig, "devicetree", boot_relpath);
    }

  /* Note this is parsed in ostree-impl-system-generator.c */
  g_autofree char *ostree_kernel_arg = g_strdup_printf ("ostree=/ostree/boot.%d/%s/%s/%d",
                                       new_bootversion, osname, bootcsum,
                                       ostree_deployment_get_bootserial (deployment));
  _ostree_kernel_args_replace_take (kargs, g_steal_pointer (&ostree_kernel_arg));

  g_autofree char *options_key = _ostree_kernel_args_to_string (kargs);
  ostree_bootconfig_parser_set (bootconfig, "options", options_key);

  glnx_autofd int bootconf_dfd = -1;
  if (!glnx_opendirat (boot_dfd, bootconfdir, TRUE, &bootconf_dfd, error))
    return FALSE;

  if (!ostree_bootconfig_parser_write_at (ostree_deployment_get_bootconfig (deployment),
                                          bootconf_dfd, bootconf_name,
                                          cancellable, error))
    return FALSE;

  return TRUE;
}

/* We generate the symlink on disk, then potentially do a syncfs() to ensure
 * that it (and everything else we wrote) has hit disk. Only after that do we
 * rename it into place.
 */
static gboolean
prepare_new_bootloader_link (OstreeSysroot  *sysroot,
                             int             current_bootversion,
                             int             new_bootversion,
                             GCancellable   *cancellable,
                             GError        **error)
{
  g_assert ((current_bootversion == 0 && new_bootversion == 1) ||
            (current_bootversion == 1 && new_bootversion == 0));

  g_autofree char *new_target = g_strdup_printf ("loader.%d", new_bootversion);

  /* We shouldn't actually need to replace but it's easier to reuse
     that code */
  if (!symlink_at_replace (new_target, sysroot->sysroot_fd, "boot/loader.tmp",
                           cancellable, error))
    return FALSE;

  return TRUE;
}

/* Update the /boot/loader symlink to point to /boot/loader.$new_bootversion */
static gboolean
swap_bootloader (OstreeSysroot  *sysroot,
                 int             current_bootversion,
                 int             new_bootversion,
                 GCancellable   *cancellable,
                 GError        **error)
{
  glnx_autofd int boot_dfd = -1;

  g_assert ((current_bootversion == 0 && new_bootversion == 1) ||
            (current_bootversion == 1 && new_bootversion == 0));

  if (!glnx_opendirat (sysroot->sysroot_fd, "boot", TRUE, &boot_dfd, error))
    return FALSE;

  /* The symlink was already written, and we used syncfs() to ensure
   * its data is in place.  Renaming now should give us atomic semantics;
   * see https://bugzilla.gnome.org/show_bug.cgi?id=755595
   */
  if (!glnx_renameat (boot_dfd, "loader.tmp", boot_dfd, "loader", error))
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
  if (fsync (boot_dfd) != 0)
    return glnx_throw_errno_prefix (error, "fsync(boot)");

  return TRUE;
}

static GHashTable *
assign_bootserials (GPtrArray   *deployments)
{
  guint i;
  GHashTable *ret =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);

  for (i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      const char *bootcsum = ostree_deployment_get_bootcsum (deployment);
      guint count;

      count = GPOINTER_TO_UINT (g_hash_table_lookup (ret, bootcsum));
      g_hash_table_replace (ret, (char*) bootcsum,
                            GUINT_TO_POINTER (count + 1));

      ostree_deployment_set_bootserial (deployment, count);
    }
  return ret;
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
deployment_bootconfigs_equal (OstreeDeployment *a,
                              OstreeDeployment *b)
{
  const char *a_bootcsum = ostree_deployment_get_bootcsum (a);
  const char *b_bootcsum = ostree_deployment_get_bootcsum (b);

  if (strcmp (a_bootcsum, b_bootcsum) != 0)
    return FALSE;

  {
    OstreeBootconfigParser *a_bootconfig = ostree_deployment_get_bootconfig (a);
    OstreeBootconfigParser *b_bootconfig = ostree_deployment_get_bootconfig (b);
    const char *a_boot_options = ostree_bootconfig_parser_get (a_bootconfig, "options");
    const char *b_boot_options = ostree_bootconfig_parser_get (b_bootconfig, "options");
    g_autoptr(OstreeKernelArgs) a_kargs = NULL;
    g_autoptr(OstreeKernelArgs) b_kargs = NULL;
    g_autofree char *a_boot_options_without_ostree = NULL;
    g_autofree char *b_boot_options_without_ostree = NULL;

    /* We checksum the kernel arguments *except* ostree= */
    a_kargs = _ostree_kernel_args_from_string (a_boot_options);
    _ostree_kernel_args_replace (a_kargs, "ostree");
    a_boot_options_without_ostree = _ostree_kernel_args_to_string (a_kargs);

    b_kargs = _ostree_kernel_args_from_string (b_boot_options);
    _ostree_kernel_args_replace (b_kargs, "ostree");
    b_boot_options_without_ostree = _ostree_kernel_args_to_string (b_kargs);

    if (strcmp (a_boot_options_without_ostree, b_boot_options_without_ostree) != 0)
      return FALSE;
  }

  return TRUE;
}

/* This used to be a temporary hack to create "current" symbolic link
 * that's easy to follow inside the gnome-ostree build scripts (now
 * gnome-continuous).  It wasn't atomic, and nowadays people can use
 * the OSTree API to find deployments.
 */
static gboolean
cleanup_legacy_current_symlinks (OstreeSysroot         *self,
                                 GCancellable          *cancellable,
                                 GError               **error)
{
  g_autoptr(GString) buf = g_string_new ("");

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

/* Detect whether or not @path refers to a read-only mountpoint. This is
 * currently just used to handle a potentially read-only /boot by transiently
 * remounting it read-write. In the future we might also do this for e.g.
 * /sysroot.
 */
static gboolean
is_ro_mount (const char *path)
{
#ifdef HAVE_LIBMOUNT
  /* Dragging in all of this crud is apparently necessary just to determine
   * whether something is a mount point.
   *
   * Systemd has a totally different implementation in
   * src/basic/mount-util.c.
   */
  struct libmnt_table *tb = mnt_new_table_from_file ("/proc/self/mountinfo");
  struct libmnt_fs *fs;
  struct libmnt_cache *cache;
  gboolean is_mount = FALSE;
  struct statvfs stvfsbuf;

  if (!tb)
    return FALSE;

  /* to canonicalize all necessary paths */
  cache = mnt_new_cache ();
  mnt_table_set_cache (tb, cache);

  fs = mnt_table_find_target(tb, path, MNT_ITER_BACKWARD);
  is_mount = fs && mnt_fs_get_target (fs);
#ifdef HAVE_MNT_UNREF_CACHE
  mnt_unref_table (tb);
  mnt_unref_cache (cache);
#else
  mnt_free_table (tb);
  mnt_free_cache (cache);
#endif

  if (!is_mount)
    return FALSE;

  /* We *could* parse the options, but it seems more reliable to
   * introspect the actual mount at runtime.
   */
  if (statvfs (path, &stvfsbuf) == 0)
    return (stvfsbuf.f_flag & ST_RDONLY) != 0;

#endif
  return FALSE;
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
ostree_sysroot_write_deployments (OstreeSysroot     *self,
                                  GPtrArray         *new_deployments,
                                  GCancellable      *cancellable,
                                  GError           **error)
{
  OstreeSysrootWriteDeploymentsOpts opts = { .do_postclean = TRUE };
  return ostree_sysroot_write_deployments_with_options (self, new_deployments, &opts,
                                                        cancellable, error);
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
 */
gboolean
ostree_sysroot_write_deployments_with_options (OstreeSysroot     *self,
                                               GPtrArray         *new_deployments,
                                               OstreeSysrootWriteDeploymentsOpts *opts,
                                               GCancellable      *cancellable,
                                               GError           **error)
{
  gboolean ret = FALSE;
  guint i;
  gboolean requires_new_bootversion = FALSE;
  gboolean found_booted_deployment = FALSE;
  gboolean bootloader_is_atomic = FALSE;
  gboolean boot_was_ro_mount = FALSE;
  SyncStats syncstats = { 0, };
  g_autoptr(OstreeBootloader) bootloader = NULL;

  g_assert (self->loaded);

  /* Assign a bootserial to each new deployment.
   */
  g_hash_table_unref (assign_bootserials (new_deployments));

  /* Determine whether or not we need to touch the bootloader
   * configuration.  If we have an equal number of deployments with
   * matching bootloader configuration, then we can just swap the
   * subbootversion bootlinks.
   */
  if (new_deployments->len != self->deployments->len)
    requires_new_bootversion = TRUE;
  else
    {
      for (i = 0; i < new_deployments->len; i++)
        {
          if (!deployment_bootconfigs_equal (new_deployments->pdata[i],
                                             self->deployments->pdata[i]))
            {
              requires_new_bootversion = TRUE;
              break;
            }
        }
    }

  for (i = 0; i < new_deployments->len; i++)
    {
      OstreeDeployment *deployment = new_deployments->pdata[i];
      g_autoptr(GFile) deployment_root = NULL;

      if (deployment == self->booted_deployment)
        found_booted_deployment = TRUE;

      deployment_root = ostree_sysroot_get_deployment_directory (self, deployment);
      if (!g_file_query_exists (deployment_root, NULL))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Unable to find expected deployment root: %s",
                       gs_file_get_path_cached (deployment_root));
          goto out;
        }

      ostree_deployment_set_index (deployment, i);
    }

  if (self->booted_deployment && !found_booted_deployment)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Attempting to remove booted deployment");
      goto out;
    }

  if (!requires_new_bootversion)
    {
      if (!create_new_bootlinks (self, self->bootversion,
                                 new_deployments,
                                 cancellable, error))
        {
          g_prefix_error (error, "Creating new current bootlinks: ");
          goto out;
        }

      if (!full_system_sync (self, &syncstats, cancellable, error))
        {
          g_prefix_error (error, "Full sync: ");
          goto out;
        }

      if (!swap_bootlinks (self, self->bootversion,
                           new_deployments,
                           cancellable, error))
        {
          g_prefix_error (error, "Swapping current bootlinks: ");
          goto out;
        }

      bootloader_is_atomic = TRUE;
    }
  else
    {
      int new_bootversion = self->bootversion ? 0 : 1;
      g_autofree char* new_loader_entries_dir = NULL;
      g_autoptr(OstreeRepo) repo = NULL;
      gboolean show_osname = FALSE;

      if (self->booted_deployment)
        boot_was_ro_mount = is_ro_mount ("/boot");

      g_debug ("boot is ro: %s", boot_was_ro_mount ? "yes" : "no");

      if (boot_was_ro_mount)
        {
          if (mount ("/boot", "/boot", NULL, MS_REMOUNT | MS_SILENT, NULL) < 0)
            {
              glnx_set_prefix_error_from_errno (error, "%s", "Remounting /boot read-write");
              goto out;
            }
        }

      if (!_ostree_sysroot_query_bootloader (self, &bootloader, cancellable, error))
        goto out;

      new_loader_entries_dir = g_strdup_printf ("boot/loader.%d/entries", new_bootversion);
      if (!glnx_shutil_rm_rf_at (self->sysroot_fd, new_loader_entries_dir, cancellable, error))
        goto out;
      if (!glnx_shutil_mkdir_p_at (self->sysroot_fd, new_loader_entries_dir, 0755,
                                   cancellable, error))
        goto out;

      /* Need the repo to try and extract the versions for deployments.
       * But this is a "nice-to-have" for the bootloader UI, so failure
       * here is not fatal to the whole operation.  We just gracefully
       * fall back to the deployment index. */
      (void) ostree_sysroot_get_repo (self, &repo, cancellable, NULL);

      /* Only show the osname in bootloader titles if there are multiple
       * osname's among the new deployments.  Check for that here. */
      for (i = 1; i < new_deployments->len; i++)
        {
          const gchar *osname_0, *osname_i;

          osname_0 = ostree_deployment_get_osname (new_deployments->pdata[0]);
          osname_i = ostree_deployment_get_osname (new_deployments->pdata[i]);

          if (!g_str_equal (osname_0, osname_i))
            {
              show_osname = TRUE;
              break;
            }
        }

      for (i = 0; i < new_deployments->len; i++)
        {
          OstreeDeployment *deployment = new_deployments->pdata[i];
          if (!install_deployment_kernel (self, repo, new_bootversion,
                                          deployment, new_deployments->len,
                                          show_osname, cancellable, error))
            {
              g_prefix_error (error, "Installing kernel: ");
              goto out;
            }
        }

      /* Create and swap bootlinks for *new* version */
      if (!create_new_bootlinks (self, new_bootversion,
                                 new_deployments,
                                 cancellable, error))
        {
          g_prefix_error (error, "Creating new version bootlinks: ");
          goto out;
        }
      if (!swap_bootlinks (self, new_bootversion, new_deployments,
                           cancellable, error))
        {
          g_prefix_error (error, "Swapping new version bootlinks: ");
          goto out;
        }

      g_debug ("Using bootloader: %s", bootloader ?
               g_type_name (G_TYPE_FROM_INSTANCE (bootloader)) : "(none)");

      if (bootloader)
        bootloader_is_atomic = _ostree_bootloader_is_atomic (bootloader);

      if (bootloader)
        {
          if (!_ostree_bootloader_write_config (bootloader, new_bootversion,
                                                cancellable, error))
            {
              g_prefix_error (error, "Bootloader write config: ");
              goto out;
            }
        }

      if (!prepare_new_bootloader_link (self, self->bootversion, new_bootversion,
                                        cancellable, error))
        {
          g_prefix_error (error, "Preparing final bootloader swap: ");
          goto out;
        }

      if (!full_system_sync (self, &syncstats, cancellable, error))
        {
          g_prefix_error (error, "Full sync: ");
          goto out;
        }

      if (!swap_bootloader (self, self->bootversion, new_bootversion,
                            cancellable, error))
        {
          g_prefix_error (error, "Final bootloader swap: ");
          goto out;
        }
    }

  { g_autofree char *msg =
      g_strdup_printf ("%s; bootconfig swap: %s deployment count change: %i",
                       (bootloader_is_atomic ? "Transaction complete" : "Bootloader updated"),
                       requires_new_bootversion ? "yes" : "no",
                       new_deployments->len - self->deployments->len);
#ifdef HAVE_LIBSYSTEMD
    sd_journal_send ("MESSAGE_ID=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(OSTREE_DEPLOYMENT_COMPLETE_ID),
                     "MESSAGE=%s", msg,
                     "OSTREE_BOOTLOADER=%s", bootloader ? _ostree_bootloader_get_name (bootloader) : "none",
                     "OSTREE_BOOTLOADER_ATOMIC=%s", bootloader_is_atomic ? "yes" : "no",
                     "OSTREE_DID_BOOTSWAP=%s", requires_new_bootversion ? "yes" : "no",
                     "OSTREE_N_DEPLOYMENTS=%u", new_deployments->len,
                     "OSTREE_SYNCFS_ROOT_MSEC=%" G_GUINT64_FORMAT, syncstats.root_syncfs_msec,
                     "OSTREE_SYNCFS_BOOT_MSEC=%" G_GUINT64_FORMAT, syncstats.boot_syncfs_msec,
                     "OSTREE_SYNCFS_EXTRA_MSEC=%" G_GUINT64_FORMAT, syncstats.extra_syncfs_msec,
                     NULL);
#endif
    _ostree_sysroot_emit_journal_msg (self, msg);
  }

  if (!_ostree_sysroot_bump_mtime (self, error))
    goto out;

  /* Now reload from disk */
  if (!ostree_sysroot_load (self, cancellable, error))
    {
      g_prefix_error (error, "Reloading deployments after commit: ");
      goto out;
    }

  if (!cleanup_legacy_current_symlinks (self, cancellable, error))
    goto out;

  /* And finally, cleanup of any leftover data.
   */
  if (opts->do_postclean)
    {
      if (!ostree_sysroot_cleanup (self, cancellable, error))
        {
          g_prefix_error (error, "Performing final cleanup: ");
          goto out;
        }
    }

  ret = TRUE;
 out:
  if (boot_was_ro_mount)
    {
      if (mount ("/boot", "/boot", NULL, MS_REMOUNT | MS_RDONLY | MS_SILENT, NULL) < 0)
        {
          /* Only make this a warning because we don't want to
           * completely bomb out if some other process happened to
           * jump in and open a file there.
           */
          int errsv = errno;
          g_printerr ("warning: Failed to remount /boot read-only: %s\n", strerror (errsv));
        }
    }
  return ret;
}

static gboolean
allocate_deployserial (OstreeSysroot           *self,
                       const char              *osname,
                       const char              *revision,
                       int                     *out_deployserial,
                       GCancellable            *cancellable,
                       GError                 **error)
{
  int new_deployserial = 0;
  g_autoptr(GPtrArray) tmp_current_deployments =
    g_ptr_array_new_with_free_func (g_object_unref);

  glnx_autofd int deploy_dfd = -1;
  if (!glnx_opendirat (self->sysroot_fd, "ostree/deploy", TRUE, &deploy_dfd, error))
    return FALSE;

  if (!_ostree_sysroot_list_deployment_dirs_for_os (deploy_dfd, osname,
                                                    tmp_current_deployments,
                                                    cancellable, error))
    return FALSE;

  for (guint i = 0; i < tmp_current_deployments->len; i++)
    {
      OstreeDeployment *deployment = tmp_current_deployments->pdata[i];

      if (strcmp (ostree_deployment_get_csum (deployment), revision) != 0)
        continue;

      new_deployserial = MAX(new_deployserial, ostree_deployment_get_deployserial (deployment)+1);
    }

  *out_deployserial = new_deployserial;
  return TRUE;
}

/**
 * ostree_sysroot_deploy_tree:
 * @self: Sysroot
 * @osname: (allow-none): osname to use for merge deployment
 * @revision: Checksum to add
 * @origin: (allow-none): Origin to use for upgrades
 * @provided_merge_deployment: (allow-none): Use this deployment for merge path
 * @override_kernel_argv: (allow-none) (array zero-terminated=1) (element-type utf8): Use these as kernel arguments; if %NULL, inherit options from provided_merge_deployment
 * @out_new_deployment: (out): The new deployment path
 * @cancellable: Cancellable
 * @error: Error
 *
 * Check out deployment tree with revision @revision, performing a 3
 * way merge with @provided_merge_deployment for configuration.
 */
gboolean
ostree_sysroot_deploy_tree (OstreeSysroot     *self,
                            const char        *osname,
                            const char        *revision,
                            GKeyFile          *origin,
                            OstreeDeployment  *provided_merge_deployment,
                            char             **override_kernel_argv,
                            OstreeDeployment **out_new_deployment,
                            GCancellable      *cancellable,
                            GError           **error)
{
  g_return_val_if_fail (osname != NULL || self->booted_deployment != NULL, FALSE);

  if (osname == NULL)
    osname = ostree_deployment_get_osname (self->booted_deployment);

  const char *osdeploypath = glnx_strjoina ("ostree/deploy/", osname);
  glnx_autofd int os_deploy_dfd = -1;
  if (!glnx_opendirat (self->sysroot_fd, osdeploypath, TRUE, &os_deploy_dfd, error))
    return FALSE;

  OstreeRepo *repo = ostree_sysroot_repo (self);
  g_autoptr(OstreeDeployment) merge_deployment = NULL;
  if (provided_merge_deployment != NULL)
    merge_deployment = g_object_ref (provided_merge_deployment);

  gint new_deployserial;
  if (!allocate_deployserial (self, osname, revision, &new_deployserial,
                              cancellable, error))
    return FALSE;

  g_autofree char *new_bootcsum = NULL;
  g_autoptr(OstreeDeployment) new_deployment =
    ostree_deployment_new (0, osname, revision, new_deployserial,
                           new_bootcsum, -1);
  ostree_deployment_set_origin (new_deployment, origin);

  /* Check out the userspace tree onto the filesystem */
  glnx_autofd int deployment_dfd = -1;
  if (!checkout_deployment_tree (self, repo, new_deployment, &deployment_dfd,
                                 cancellable, error))
    {
      g_prefix_error (error, "Checking out tree: ");
      return FALSE;
    }

  g_autoptr(OstreeKernelLayout) kernel_layout = NULL;
  if (!get_kernel_from_tree (deployment_dfd, &kernel_layout,
                             cancellable, error))
    return FALSE;

  _ostree_deployment_set_bootcsum (new_deployment, kernel_layout->bootcsum);

  /* Create an empty boot configuration; we will merge things into
   * it as we go.
   */
  g_autoptr(OstreeBootconfigParser) bootconfig = ostree_bootconfig_parser_new ();
  ostree_deployment_set_bootconfig (new_deployment, bootconfig);

  g_autoptr(OstreeSePolicy) sepolicy = NULL;
  if (!merge_configuration (self, repo, merge_deployment, new_deployment,
                            deployment_dfd,
                            &sepolicy,
                            cancellable, error))
    {
      g_prefix_error (error, "During /etc merge: ");
      return FALSE;
    }

  if (!selinux_relabel_var_if_needed (self, sepolicy, os_deploy_dfd,
                                      cancellable, error))
    return FALSE;

  if (!(self->debug_flags & OSTREE_SYSROOT_DEBUG_MUTABLE_DEPLOYMENTS))
    {
      if (!ostree_sysroot_deployment_set_mutable (self, new_deployment, FALSE,
                                                  cancellable, error))
        return FALSE;
    }

  { g_auto(OstreeSepolicyFsCreatecon) con = { 0, };

    if (!_ostree_sepolicy_preparefscreatecon (&con, sepolicy,
                                              "/etc/ostree/remotes.d/dummy.conf",
                                              0644, error))
      return FALSE;

    /* Don't fsync here, as we assume that's all done in
     * ostree_sysroot_write_deployments().
     */
    if (!write_origin_file_internal (self, new_deployment, NULL,
                                     GLNX_FILE_REPLACE_NODATASYNC,
                                     cancellable, error))
      {
        g_prefix_error (error, "Writing out origin file: ");
        return FALSE;
      }
  }

  /* After this, install_deployment_kernel() will set the other boot
   * options and write it out to disk.
   */
  if (override_kernel_argv)
    {
      g_autoptr(OstreeKernelArgs) kargs = NULL;
      g_autofree char *new_options = NULL;

      kargs = _ostree_kernel_args_new ();
      _ostree_kernel_args_append_argv (kargs, override_kernel_argv);
      new_options = _ostree_kernel_args_to_string (kargs);
      ostree_bootconfig_parser_set (bootconfig, "options", new_options);
    }

  ot_transfer_out_value (out_new_deployment, &new_deployment);
  return TRUE;
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
ostree_sysroot_deployment_set_kargs (OstreeSysroot     *self,
                                     OstreeDeployment  *deployment,
                                     char             **new_kargs,
                                     GCancellable      *cancellable,
                                     GError           **error)
{
  g_autoptr(OstreeDeployment) new_deployment = ostree_deployment_clone (deployment);
  OstreeBootconfigParser *new_bootconfig = ostree_deployment_get_bootconfig (new_deployment);

  g_autoptr(OstreeKernelArgs) kargs = _ostree_kernel_args_new ();
  _ostree_kernel_args_append_argv (kargs, new_kargs);
  g_autofree char *new_options = _ostree_kernel_args_to_string (kargs);
  ostree_bootconfig_parser_set (new_bootconfig, "options", new_options);

  g_autoptr(GPtrArray) new_deployments = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < self->deployments->len; i++)
    {
      OstreeDeployment *cur = self->deployments->pdata[i];
      if (cur == deployment)
        g_ptr_array_add (new_deployments, g_object_ref (new_deployment));
      else
        g_ptr_array_add (new_deployments, g_object_ref (cur));
    }

  if (!ostree_sysroot_write_deployments (self, new_deployments,
                                         cancellable, error))
    return FALSE;

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
ostree_sysroot_deployment_set_mutable (OstreeSysroot     *self,
                                       OstreeDeployment  *deployment,
                                       gboolean           is_mutable,
                                       GCancellable      *cancellable,
                                       GError           **error)
{

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
