/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012,2014 Colin Walters <walters@verbum.org>
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
#include <sys/mount.h>
#include <sys/statvfs.h>

#ifdef HAVE_LIBMOUNT
#include <libmount.h>
#endif

#include "ostree-sysroot-private.h"
#include "ostree-deployment-private.h"
#include "ostree-core-private.h"
#include "ostree-linuxfsutil.h"
#include "otutil.h"
#include "libglnx.h"

#define OSTREE_VARRELABEL_ID          "da679b08acd34504b789d96f818ea781"
#define OSTREE_CONFIGMERGE_ID         "d3863baec13e4449ab0384684a8af3a7"
#define OSTREE_DEPLOYMENT_COMPLETE_ID "dd440e3e549083b63d0efc7dc15255f1"

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
  gboolean ret = FALSE;
  int res;
  /* Possibly in the future generate a temporary random name here,
   * would need to move "generate a temporary name" code into
   * libglnx or glib?
   */
  g_autofree char *temppath = g_strconcat (newpath, ".tmp", NULL);

  /* Clean up any stale temporary links */ 
  (void) unlinkat (parent_dfd, temppath, 0);

  /* Create the temp link */ 
  do
    res = symlinkat (oldpath, parent_dfd, temppath);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  /* Rename it into place */ 
  do
    res = renameat (parent_dfd, temppath, parent_dfd, newpath);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/* Try a hardlink if we can, otherwise fall back to copying.  Used
 * right now for kernels/initramfs in /boot, where we can just
 * hardlink if we're on the same partition.
 */
static gboolean
hardlink_or_copy_at (int         src_dfd,
                     const char *src_subpath,
                     int         dest_dfd,
                     const char *dest_subpath,
                     GCancellable  *cancellable,
                     GError       **error)
{
  gboolean ret = FALSE;

  if (linkat (src_dfd, src_subpath, dest_dfd, dest_subpath, 0) != 0)
    {
      if (errno == EMLINK || errno == EXDEV)
        {
          return glnx_file_copy_at (src_dfd, src_subpath, NULL, dest_dfd, dest_subpath, 0,
                                    cancellable, error);
        }
      else
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
dirfd_copy_attributes_and_xattrs (int            src_parent_dfd,
                                  const char    *src_name,
                                  int            src_dfd,
                                  int            dest_dfd,
                                  GCancellable  *cancellable,
                                  GError       **error)
{
  gboolean ret = FALSE;
  struct stat src_stbuf;
  g_autoptr(GVariant) xattrs = NULL;

  /* Clone all xattrs first, so we get the SELinux security context
   * right.  This will allow other users access if they have ACLs, but
   * oh well.
   */ 
  if (!glnx_dfd_name_get_all_xattrs (src_parent_dfd, src_name,
                                       &xattrs, cancellable, error))
    goto out;
  if (!glnx_fd_set_all_xattrs (dest_dfd, xattrs,
                             cancellable, error))
    goto out;

  if (fstat (src_dfd, &src_stbuf) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }
  if (fchown (dest_dfd, src_stbuf.st_uid, src_stbuf.st_gid) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }
  if (fchmod (dest_dfd, src_stbuf.st_mode) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
copy_dir_recurse (int              src_parent_dfd,
                  int              dest_parent_dfd,
                  const char      *name,
                  GCancellable    *cancellable,
                  GError         **error)
{
  g_auto(GLnxDirFdIterator) src_dfd_iter = { 0, };
  glnx_fd_close int dest_dfd = -1;
  struct dirent *dent;

  if (!glnx_dirfd_iterator_init_at (src_parent_dfd, name, TRUE, &src_dfd_iter, error))
    return FALSE;

  /* Create with mode 0700, we'll fchmod/fchown later */
  if (mkdirat (dest_parent_dfd, name, 0700) != 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  if (!glnx_opendirat (dest_parent_dfd, name, TRUE, &dest_dfd, error))
    return FALSE;

  if (!dirfd_copy_attributes_and_xattrs (src_parent_dfd, name, src_dfd_iter.fd, dest_dfd,
                                         cancellable, error))
    return FALSE;
 
  while (TRUE)
    {
      struct stat child_stbuf;

      if (!glnx_dirfd_iterator_next_dent (&src_dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (fstatat (src_dfd_iter.fd, dent->d_name, &child_stbuf,
                   AT_SYMLINK_NOFOLLOW) != 0)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }

      if (S_ISDIR (child_stbuf.st_mode))
        {
          if (!copy_dir_recurse (src_dfd_iter.fd, dest_dfd, dent->d_name,
                                 cancellable, error))
            return FALSE;
        }
      else
        {
          if (!glnx_file_copy_at (src_dfd_iter.fd, dent->d_name, &child_stbuf,
                                  dest_dfd, dent->d_name,
                                  GLNX_FILE_COPY_OVERWRITE,
                                  cancellable, error))
            return FALSE;
        }
    }

  return TRUE;
}

static gboolean
ensure_directory_from_template (int                 orig_etc_fd,
                                int                 modified_etc_fd,
                                int                 new_etc_fd,
                                const char         *path,
                                int                *out_dfd,
                                GCancellable       *cancellable,
                                GError            **error)
{
  gboolean ret = FALSE;
  glnx_fd_close int src_dfd = -1;
  glnx_fd_close int target_dfd = -1;

  g_assert (path != NULL);
  g_assert (*path != '/' && *path != '\0');

  if (!ot_gopendirat (modified_etc_fd, path, TRUE, &src_dfd, error))
    goto out;

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
                                                   parent_path, NULL, cancellable, error))
                goto out;

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
        {
          glnx_set_error_from_errno (error);
          g_prefix_error (error, "mkdirat: ");
          goto out;
        }
    }

  if (!ot_gopendirat (new_etc_fd, path, TRUE, &target_dfd, error))
    goto out;

  if (!dirfd_copy_attributes_and_xattrs (modified_etc_fd, path, src_dfd, target_dfd,
                                         cancellable, error))
    goto out;

  ret = TRUE;
  if (out_dfd)
    {
      g_assert (target_dfd != -1);
      *out_dfd = target_dfd;
      target_dfd = -1;
    }
 out:
  return ret;
}

/**
 * copy_modified_config_file:
 *
 * Copy @file from @modified_etc to @new_etc, overwriting any existing
 * file there.  The @file may refer to a regular file, a symbolic
 * link, or a directory.  Directories will be copied recursively.
 */
static gboolean
copy_modified_config_file (int                 orig_etc_fd,
                           int                 modified_etc_fd,
                           int                 new_etc_fd,
                           const char         *path,
                           GCancellable       *cancellable,
                           GError            **error)
{
  gboolean ret = FALSE;
  struct stat modified_stbuf;
  struct stat new_stbuf;
  glnx_fd_close int dest_parent_dfd = -1;

  if (fstatat (modified_etc_fd, path, &modified_stbuf, AT_SYMLINK_NOFOLLOW) < 0)
    {
      glnx_set_error_from_errno (error);
      g_prefix_error (error, "Failed to read modified config file '%s': ", path);
      goto out;
    }

  if (strchr (path, '/') != NULL)
    {
      g_autofree char *parent = g_path_get_dirname (path);

      if (!ensure_directory_from_template (orig_etc_fd, modified_etc_fd, new_etc_fd,
                                           parent, &dest_parent_dfd, cancellable, error))
        goto out;
    }
  else
    {
      dest_parent_dfd = dup (new_etc_fd);
      if (dest_parent_dfd == -1)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  g_assert (dest_parent_dfd != -1);

  if (fstatat (new_etc_fd, path, &new_stbuf, AT_SYMLINK_NOFOLLOW) < 0)
    {
      if (errno == ENOENT)
        ;
      else
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }
  else if (S_ISDIR(new_stbuf.st_mode))
    {
      if (!S_ISDIR(modified_stbuf.st_mode))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "Modified config file newly defaults to directory '%s', cannot merge",
                       path);
          goto out;
        }
      else
        {
          /* Do nothing here - we assume that we've already
           * recursively copied the parent directory.
           */
          ret = TRUE;
          goto out;
        }
    }
  else
    {
      if (unlinkat (new_etc_fd, path, 0) < 0)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  if (S_ISDIR (modified_stbuf.st_mode))
    {
      if (!copy_dir_recurse (modified_etc_fd, new_etc_fd, path,
                             cancellable, error))
        goto out;
    }
  else if (S_ISLNK (modified_stbuf.st_mode) || S_ISREG (modified_stbuf.st_mode))
    {
      if (!glnx_file_copy_at (modified_etc_fd, path, &modified_stbuf, 
                              new_etc_fd, path,
                              GLNX_FILE_COPY_OVERWRITE,
                              cancellable, error))
        goto out;
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unsupported non-regular/non-symlink file in /etc '%s'",
                   path);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * merge_etc_changes:
 *
 * Compute the difference between @orig_etc and @modified_etc,
 * and apply that to @new_etc.
 *
 * The algorithm for computing the difference is pretty simple; it's
 * approximately equivalent to "diff -unR orig_etc modified_etc",
 * except that rather than attempting a 3-way merge if a file is also
 * changed in @new_etc, the modified version always wins.
 */
static gboolean
merge_etc_changes (GFile          *orig_etc,
                   GFile          *modified_etc,
                   GFile          *new_etc,
                   GCancellable   *cancellable,
                   GError        **error)
{
  gboolean ret = FALSE;
  g_autoptr(GPtrArray) modified = NULL;
  g_autoptr(GPtrArray) removed = NULL;
  g_autoptr(GPtrArray) added = NULL;
  guint i;
  glnx_fd_close int orig_etc_fd = -1;
  glnx_fd_close int modified_etc_fd = -1;
  glnx_fd_close int new_etc_fd = -1; 

  modified = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_diff_item_unref);
  removed = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  added = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

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
    {
      g_prefix_error (error, "While computing configuration diff: ");
      goto out;
    }

  ot_log_structured_print_id_v (OSTREE_CONFIGMERGE_ID,
                                "Copying /etc changes: %u modified, %u removed, %u added", 
                                modified->len,
                                removed->len,
                                added->len);

  if (!glnx_opendirat (AT_FDCWD, gs_file_get_path_cached (orig_etc), TRUE,
                       &orig_etc_fd, error))
    goto out;
  if (!glnx_opendirat (AT_FDCWD, gs_file_get_path_cached (modified_etc), TRUE,
                       &modified_etc_fd, error))
    goto out;
  if (!glnx_opendirat (AT_FDCWD, gs_file_get_path_cached (new_etc), TRUE,
                       &new_etc_fd, error))
    goto out;

  for (i = 0; i < removed->len; i++)
    {
      GFile *file = removed->pdata[i];
      g_autoptr(GFile) target_file = NULL;
      g_autofree char *path = NULL;

      path = g_file_get_relative_path (orig_etc, file);
      g_assert (path);
      target_file = g_file_resolve_relative_path (new_etc, path);

      if (!glnx_shutil_rm_rf_at (AT_FDCWD, gs_file_get_path_cached (target_file), cancellable, error))
        goto out;
    }

  for (i = 0; i < modified->len; i++)
    {
      OstreeDiffItem *diff = modified->pdata[i];
      g_autofree char *path = g_file_get_relative_path (modified_etc, diff->target);

      g_assert (path);

      if (!copy_modified_config_file (orig_etc_fd, modified_etc_fd, new_etc_fd, path,
                                      cancellable, error))
        goto out;
    }
  for (i = 0; i < added->len; i++)
    {
      GFile *file = added->pdata[i];
      g_autofree char *path = g_file_get_relative_path (modified_etc, file);

      g_assert (path);

      if (!copy_modified_config_file (orig_etc_fd, modified_etc_fd, new_etc_fd, path,
                                      cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * checkout_deployment_tree:
 *
 * Look up @revision in the repository, and check it out in
 * /ostree/deploy/OS/deploy/${treecsum}.${deployserial}.
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
  g_autoptr(GFile) ret_deploy_target_path = NULL;
  glnx_fd_close int osdeploy_dfd = -1;
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

static gboolean
selinux_relabel_file (OstreeSysroot                 *sysroot,
                      OstreeSePolicy                *sepolicy,
                      GFile                         *path,
                      const char                    *prefix,
                      GCancellable                  *cancellable,
                      GError                       **error)
{
  gboolean ret = FALSE;
  g_autoptr(GPtrArray) path_parts = g_ptr_array_new ();
  g_autoptr(GFileInfo) file_info = g_file_query_info (path, OSTREE_GIO_FAST_QUERYINFO,
                                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                            cancellable, error);
  if (!file_info)
    goto out;

  g_ptr_array_add (path_parts, (char*)prefix);
  g_ptr_array_add (path_parts, (char*)g_file_info_get_name (file_info));
  if (!relabel_one_path (sysroot, sepolicy, path, file_info, path_parts,
                         cancellable, error))
    {
      g_prefix_error (error, "Relabeling /%s/%s: ", prefix,
                      g_file_info_get_name (file_info));
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
selinux_relabel_var_if_needed (OstreeSysroot                 *sysroot,
                               OstreeSePolicy                *sepolicy,
                               GFile                         *deployment_var_path,
                               GCancellable                  *cancellable,
                               GError                       **error)
{
  gboolean ret = FALSE;
  /* This is a bit of a hack; we should change the code at some
   * point in the distant future to only create (and label) /var
   * when doing a deployment.
   */
  g_autoptr(GFile) deployment_var_labeled = 
    g_file_get_child (deployment_var_path, ".ostree-selabeled");
  g_autoptr(GFile) deployment_var_labeled_tmp = 
    g_file_get_child (deployment_var_path, ".ostree-selabeled.tmp");
      
  if (!g_file_query_exists (deployment_var_labeled, NULL))
    {
      ot_log_structured_print_id_v (OSTREE_VARRELABEL_ID,
                                    "Relabeling /var (no stamp file '%s' found)",
                                    gs_file_get_path_cached (deployment_var_labeled));

      if (!selinux_relabel_dir (sysroot, sepolicy,
                                deployment_var_path, "var",
                                cancellable, error))
        {
          g_prefix_error (error, "Relabeling /var: ");
          goto out;
        }

      if (!g_file_replace_contents (deployment_var_labeled_tmp, "", 0, NULL, FALSE,
                                    G_FILE_CREATE_REPLACE_DESTINATION, NULL,
                                    cancellable, error))
        goto out;
          
      if (!selinux_relabel_file (sysroot, sepolicy,
                                 deployment_var_labeled_tmp, "var",
                                 cancellable, error))
        goto out;

      if (rename (gs_file_get_path_cached (deployment_var_labeled_tmp),
                  gs_file_get_path_cached (deployment_var_labeled)) < 0)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
merge_configuration (OstreeSysroot         *sysroot,
                     OstreeDeployment      *previous_deployment,
                     OstreeDeployment      *deployment,
                     int                    deployment_dfd,
                     OstreeSePolicy       **out_sepolicy,
                     GCancellable          *cancellable,
                     GError               **error)
{
  gboolean ret = FALSE;
  g_autofree char *deployment_abspath = glnx_fdrel_abspath (deployment_dfd, ".");
  g_autoptr(GFile) deployment_path = g_file_new_for_path (deployment_abspath);
  g_autoptr(GFile) source_etc_path = NULL;
  g_autoptr(GFile) source_etc_pristine_path = NULL;
  g_autoptr(GFile) deployment_usretc_path = NULL;
  g_autoptr(GFile) deployment_etc_path = NULL;
  glnx_unref_object OstreeSePolicy *sepolicy = NULL;
  gboolean etc_exists;
  gboolean usretc_exists;

  if (previous_deployment)
    {
      g_autoptr(GFile) previous_path = NULL;
      OstreeBootconfigParser *previous_bootconfig;

      previous_path = ostree_sysroot_get_deployment_directory (sysroot, previous_deployment);
      source_etc_path = g_file_resolve_relative_path (previous_path, "etc");
      source_etc_pristine_path = g_file_resolve_relative_path (previous_path, "usr/etc");

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

  deployment_etc_path = g_file_get_child (deployment_path, "etc");
  deployment_usretc_path = g_file_resolve_relative_path (deployment_path, "usr/etc");
  
  etc_exists = g_file_query_exists (deployment_etc_path, NULL);
  usretc_exists = g_file_query_exists (deployment_usretc_path, NULL);

  if (etc_exists && usretc_exists)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "Tree contains both /etc and /usr/etc");
      goto out;
    }
  else if (etc_exists)
    {
      /* Compatibility hack */
      if (renameat (deployment_dfd, "etc", deployment_dfd, "usr/etc") < 0)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
      usretc_exists = TRUE;
      etc_exists = FALSE;
    }
  
  if (usretc_exists)
    {
      glnx_fd_close int deployment_usr_dfd = -1;

      if (!glnx_opendirat (deployment_dfd, "usr", TRUE, &deployment_usr_dfd, error))
        goto out;

      /* TODO - set out labels as we copy files */
      g_assert (!etc_exists);
      if (!copy_dir_recurse (deployment_usr_dfd, deployment_dfd, "etc",
                             cancellable, error))
        goto out;

      /* Here, we initialize SELinux policy from the /usr/etc inside
       * the root - this is before we've finalized the configuration
       * merge into /etc. */
      sepolicy = ostree_sepolicy_new (deployment_path, cancellable, error);
      if (!sepolicy)
        goto out;

      if (ostree_sepolicy_get_name (sepolicy) != NULL)
        {
          if (!selinux_relabel_dir (sysroot, sepolicy, deployment_etc_path, "etc",
                                    cancellable, error))
            goto out;
        }
    }

  if (source_etc_path)
    {
      if (!merge_etc_changes (source_etc_pristine_path, source_etc_path, deployment_etc_path, 
                              cancellable, error))
        goto out;
    }

  ret = TRUE;
  if (out_sepolicy)
    *out_sepolicy = g_steal_pointer (&sepolicy);
 out:
  return ret;
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
  gboolean ret = FALSE;
  GKeyFile *origin =
    new_origin ? new_origin : ostree_deployment_get_origin (deployment);

  if (origin)
    {
      g_autoptr(GFile) deployment_path = ostree_sysroot_get_deployment_directory (sysroot, deployment);
      g_autoptr(GFile) origin_path = ostree_sysroot_get_deployment_origin_path (deployment_path);
      g_autoptr(GFile) origin_parent = g_file_get_parent (origin_path);
      g_autofree char *contents = NULL;
      gsize len;
      g_autoptr(GBytes) contents_bytes = NULL;

      contents = g_key_file_to_data (origin, &len, error);
      if (!contents)
        goto out;
      contents_bytes = g_bytes_new_static (contents, len);

      if (!ot_gfile_replace_contents_fsync (origin_path, contents_bytes,
                                            cancellable, error))
        goto out;

      if (!ot_util_fsync_directory (origin_parent, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
get_kernel_from_tree (int             deployment_dfd,
                      int            *out_boot_dfd,
                      char          **out_kernel_name,
                      char          **out_initramfs_name,
                      GCancellable   *cancellable,
                      GError        **error)
{
  gboolean ret = FALSE;
  glnx_fd_close int ret_boot_dfd = -1;
  g_auto(GLnxDirFdIterator) dfditer = { 0, };
  g_autofree char *ret_kernel_name = NULL;
  g_autofree char *ret_initramfs_name = NULL;
  g_autofree char *kernel_checksum = NULL;
  g_autofree char *initramfs_checksum = NULL;

  ret_boot_dfd = glnx_opendirat_with_errno (deployment_dfd, "usr/lib/ostree-boot", TRUE);
  if (ret_boot_dfd == -1)
    {
      if (errno != ENOENT)
        {
          glnx_set_prefix_error_from_errno (error, "%s", "openat");
          goto out;
        }
      else
        {
          if (!glnx_opendirat (deployment_dfd, "boot", TRUE, &ret_boot_dfd, error))
            goto out;
        }
    }

  if (!glnx_dirfd_iterator_init_at (ret_boot_dfd, ".", FALSE, &dfditer, error))
    goto out;

  while (TRUE)
    {
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent (&dfditer, &dent, cancellable, error))
        goto out;
          
      if (dent == NULL)
        break;

      if (ret_kernel_name == NULL && g_str_has_prefix (dent->d_name, "vmlinuz-"))
        {
          const char *dash = strrchr (dent->d_name, '-');
          g_assert (dash);
          if (ostree_validate_structureof_checksum_string (dash + 1, NULL))
            {
              kernel_checksum = g_strdup (dash + 1);
              ret_kernel_name = g_strdup (dent->d_name);
            }
        }
      else if (ret_initramfs_name == NULL && g_str_has_prefix (dent->d_name, "initramfs-"))
        {
          const char *dash = strrchr (dent->d_name, '-');
          g_assert (dash);
          if (ostree_validate_structureof_checksum_string (dash + 1, NULL))
            {
              initramfs_checksum = g_strdup (dash + 1);
              ret_initramfs_name = g_strdup (dent->d_name);
            }
        }
      
      if (ret_kernel_name != NULL && ret_initramfs_name != NULL)
        break;
    }

  if (ret_kernel_name == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Failed to find boot/vmlinuz-<CHECKSUM> in tree");
      goto out;
    }

  if (ret_initramfs_name != NULL)
    {
      if (strcmp (kernel_checksum, initramfs_checksum) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "Mismatched kernel checksum vs initrd in tree");
          goto out;
        }
    }

  *out_boot_dfd = ret_boot_dfd;
  ret_boot_dfd = -1;
  *out_kernel_name = g_steal_pointer (&ret_kernel_name);
  *out_initramfs_name = g_steal_pointer (&ret_initramfs_name);
  ret = TRUE;
 out:
  return ret;
}

static gboolean
checksum_from_kernel_src (const char   *name,
                          char        **out_checksum,
                          GError     **error)
{
  const char *last_dash = strrchr (name, '-');
  if (!last_dash)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Malformed kernel/initramfs name '%s', missing '-'", name);
      return FALSE;
    }
  *out_checksum = g_strdup (last_dash + 1);
  return TRUE;
}

static gboolean
syncfs_dir_at (int            dfd,
               const char    *path,
               GCancellable  *cancellable,
               GError       **error)
{
  gboolean ret = FALSE;
  glnx_fd_close int child_dfd = -1;

  if (!glnx_opendirat (dfd, path, TRUE, &child_dfd, error))
    goto out;
  if (syncfs (child_dfd) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/* First, sync the root directory as well as /var and /boot which may
 * be separate mount points.  Then *in addition*, do a global
 * `sync()`.
 */
static gboolean
full_system_sync (OstreeSysroot     *self,
                  GCancellable      *cancellable,
                  GError           **error)
{
  gboolean ret = FALSE;

  if (syncfs (self->sysroot_fd) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (!syncfs_dir_at (self->sysroot_fd, "boot", cancellable, error))
    goto out;

  /* And now out of an excess of conservativism, we still invoke
   * sync().  The advantage of still using `syncfs()` above is that we
   * actually get error codes out of that API, and we more clearly
   * delineate what we actually want to sync in the future when this
   * global sync call is removed.
   */
  sync ();

  ret = TRUE;
 out:
  return ret;
}

static gboolean
create_new_bootlinks (OstreeSysroot *self,
                      int            bootversion,
                      GPtrArray     *new_deployments,
                       GCancellable  *cancellable,
                      GError       **error)
{
  gboolean ret = FALSE;
  guint i;
  int old_subbootversion;
  int new_subbootversion;
  glnx_fd_close int ostree_dfd = -1;
  glnx_fd_close int ostree_subbootdir_dfd = -1;
  g_autofree char *ostree_bootdir_name = NULL;
  g_autofree char *ostree_subbootdir_name = NULL;

  if (!glnx_opendirat (self->sysroot_fd, "ostree", TRUE, &ostree_dfd, error))
    goto out;

  ostree_bootdir_name = g_strdup_printf ("boot.%d", bootversion);

  if (bootversion != self->bootversion)
    {
      if (!_ostree_sysroot_read_current_subbootversion (self, bootversion, &old_subbootversion,
                                                        cancellable, error))
        goto out;
    }
  else
    old_subbootversion = self->subbootversion;

  new_subbootversion = old_subbootversion == 0 ? 1 : 0;

  /* Create the "subbootdir", which is a directory holding a symlink farm pointing to
   * deployments per-osname.
   */
  ostree_subbootdir_name = g_strdup_printf ("boot.%d.%d", bootversion, new_subbootversion);
  if (!glnx_shutil_rm_rf_at (ostree_dfd, ostree_subbootdir_name, cancellable, error))
    goto out;
  if (!glnx_shutil_mkdir_p_at (ostree_dfd, ostree_subbootdir_name, 0755, cancellable, error))
    goto out;
  if (!glnx_opendirat (ostree_dfd, ostree_subbootdir_name, FALSE, &ostree_subbootdir_dfd, error))
    goto out;

  for (i = 0; i < new_deployments->len; i++)
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
        goto out;

      if (!symlink_at_replace (bootlink_target, ostree_subbootdir_dfd, bootlink_pathname,
                               cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
swap_bootlinks (OstreeSysroot *self,
                int            bootversion,
                GPtrArray     *new_deployments,
                GCancellable  *cancellable,
                GError       **error)
{
  gboolean ret = FALSE;
  int old_subbootversion, new_subbootversion;
  glnx_fd_close int ostree_dfd = -1;
  glnx_fd_close int ostree_subbootdir_dfd = -1;
  g_autofree char *ostree_bootdir_name = NULL;
  g_autofree char *ostree_subbootdir_name = NULL;

  if (!glnx_opendirat (self->sysroot_fd, "ostree", TRUE, &ostree_dfd, error))
    goto out;

  ostree_bootdir_name = g_strdup_printf ("boot.%d", bootversion);

  if (bootversion != self->bootversion)
    {
      if (!_ostree_sysroot_read_current_subbootversion (self, bootversion, &old_subbootversion,
                                                        cancellable, error))
        goto out;
    }
  else
    old_subbootversion = self->subbootversion;

  new_subbootversion = old_subbootversion == 0 ? 1 : 0;

  ostree_subbootdir_name = g_strdup_printf ("boot.%d.%d", bootversion, new_subbootversion);

  if (!symlink_at_replace (ostree_subbootdir_name, ostree_dfd, ostree_bootdir_name,
                           cancellable, error))
    goto out;
  
  ret = TRUE;
 out:
  return ret;
}

static char *
remove_checksum_from_kernel_name (const char *name,
                                  const char *csum)
{
  const char *p = strrchr (name, '-');
  g_assert_cmpstr (p+1, ==, csum);
  return g_strndup (name, p-name);
}

static GHashTable *
parse_os_release (const char *contents,
                  const char *split)
{
  g_autofree char **lines = g_strsplit (contents, split, -1);
  char **iter;
  GHashTable *ret = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  for (iter = lines; *iter; iter++)
    {
      g_autofree char *line = *iter;
      char *eq;
      const char *quotedval;
      char *val;

      if (g_str_has_prefix (line, "#"))
        continue;
      
      eq = strchr (line, '=');
      if (!eq)
        continue;
      
      *eq = '\0';
      quotedval = eq + 1;
      val = g_shell_unquote (quotedval, NULL);
      if (!val)
        continue;
      
      g_hash_table_insert (ret, g_steal_pointer (&line), val);
    }

  return ret;
}

/*
 * install_deployment_kernel:
 * 
 * Write out an entry in /boot/loader/entries for @deployment.
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
  gboolean ret = FALSE;
  struct stat stbuf;
  const char *osname = ostree_deployment_get_osname (deployment);
  const char *bootcsum = ostree_deployment_get_bootcsum (deployment);
  g_autofree char *bootcsumdir = NULL;
  g_autofree char *bootconfdir = NULL;
  g_autofree char *bootconf_name = NULL;
  g_autofree char *dest_kernel_name = NULL;
  g_autofree char *dest_initramfs_name = NULL;
  g_autofree char *tree_kernel_name = NULL;
  g_autofree char *tree_initramfs_name = NULL;
  g_autofree char *deployment_dirpath = NULL;
  glnx_fd_close int deployment_dfd = -1;
  glnx_fd_close int tree_boot_dfd = -1;
  glnx_fd_close int boot_dfd = -1;
  glnx_fd_close int bootcsum_dfd = -1;
  g_autofree char *contents = NULL;
  g_autofree char *deployment_version = NULL;
  g_autoptr(GHashTable) osrelease_values = NULL;
  g_autofree char *version_key = NULL;
  g_autofree char *ostree_kernel_arg = NULL;
  g_autofree char *options_key = NULL;
  GString *title_key;
  __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *kargs = NULL;
  const char *val;
  OstreeBootconfigParser *bootconfig;

  bootconfig = ostree_deployment_get_bootconfig (deployment);
  deployment_dirpath = ostree_sysroot_get_deployment_dirpath (sysroot, deployment);

  if (!glnx_opendirat (sysroot->sysroot_fd, deployment_dirpath, FALSE,
                       &deployment_dfd, error))
    goto out;

  if (!get_kernel_from_tree (deployment_dfd, &tree_boot_dfd,
                             &tree_kernel_name, &tree_initramfs_name,
                             cancellable, error))
    goto out;

  if (!glnx_opendirat (sysroot->sysroot_fd, "boot", TRUE, &boot_dfd, error))
    goto out;

  bootcsumdir = g_strdup_printf ("ostree/%s-%s", osname, bootcsum);
  bootconfdir = g_strdup_printf ("loader.%d/entries", new_bootversion);
  bootconf_name = g_strdup_printf ("ostree-%s-%d.conf", osname, 
                                   ostree_deployment_get_index (deployment));

  if (!glnx_shutil_mkdir_p_at (boot_dfd, bootcsumdir, 0775, cancellable, error))
    goto out;
  if (!glnx_opendirat (boot_dfd, bootcsumdir, TRUE, &bootcsum_dfd, error))
    goto out;

  if (!glnx_shutil_mkdir_p_at (boot_dfd, bootconfdir, 0775, cancellable, error))
    goto out;

  dest_kernel_name = remove_checksum_from_kernel_name (tree_kernel_name, bootcsum);

  if (fstatat (bootcsum_dfd, dest_kernel_name, &stbuf, 0) != 0)
    {
      if (errno != ENOENT)
        {
          glnx_set_prefix_error_from_errno (error, "fstat %s", dest_kernel_name);
          goto out;
        }
      if (!hardlink_or_copy_at (tree_boot_dfd, tree_kernel_name,
                                bootcsum_dfd, dest_kernel_name,
                                cancellable, error))
        goto out;
    }

  if (tree_initramfs_name)
    {
      dest_initramfs_name = remove_checksum_from_kernel_name (tree_initramfs_name, bootcsum);

      if (fstatat (bootcsum_dfd, dest_initramfs_name, &stbuf, 0) != 0)
        {
          if (errno != ENOENT)
            {
              glnx_set_prefix_error_from_errno (error, "fstat %s", dest_initramfs_name);
              goto out;
            }
          if (!hardlink_or_copy_at (tree_boot_dfd, tree_initramfs_name,
                                    bootcsum_dfd, dest_initramfs_name,
                                    cancellable, error))
            goto out;
        }
    }

  if (fstatat (deployment_dfd, "usr/lib/os-release", &stbuf, 0) != 0)
    {
      if (errno != ENOENT)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
      else
        {
          contents = glnx_file_get_contents_utf8_at (deployment_dfd, "etc/os-release", NULL,
                                                     cancellable, error);
          if (!contents)
            {
              g_prefix_error (error, "Reading /etc/os-release: ");
              goto out;
            }
        }
    }
  else
    {
      contents = glnx_file_get_contents_utf8_at (deployment_dfd, "usr/lib/os-release", NULL,
                                                 cancellable, error);
      if (!contents)
        {
          g_prefix_error (error, "Reading /usr/lib/os-release: ");
          goto out;
        }
    }

  osrelease_values = parse_os_release (contents, "\n");

  /* title */
  val = g_hash_table_lookup (osrelease_values, "PRETTY_NAME");
  if (val == NULL)
      val = g_hash_table_lookup (osrelease_values, "ID");
  if (val == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "No PRETTY_NAME or ID in /etc/os-release");
      goto out;
    }
  

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
          g_variant_lookup (metadata, "version", "s", &deployment_version);
        }
    }

  /* XXX The SYSLINUX bootloader backend actually parses the title string
   *     (specifically, it looks for the substring "(ostree"), so further
   *     changes to the title format may require updating that backend. */
  title_key = g_string_new (val);
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
  g_string_free (title_key, TRUE);

  version_key = g_strdup_printf ("%d", n_deployments - ostree_deployment_get_index (deployment));
  ostree_bootconfig_parser_set (bootconfig, "version", version_key);

  { g_autofree char * boot_relpath = g_strconcat ("/", bootcsumdir, "/", dest_kernel_name, NULL);
    ostree_bootconfig_parser_set (bootconfig, "linux", boot_relpath);
  }

  if (dest_initramfs_name)
    {
      g_autofree char * boot_relpath = g_strconcat ("/", bootcsumdir, "/", dest_initramfs_name, NULL);
      ostree_bootconfig_parser_set (bootconfig, "initrd", boot_relpath);
    }

  val = ostree_bootconfig_parser_get (bootconfig, "options");

  ostree_kernel_arg = g_strdup_printf ("ostree=/ostree/boot.%d/%s/%s/%d",
                                       new_bootversion, osname, bootcsum,
                                       ostree_deployment_get_bootserial (deployment));
  kargs = _ostree_kernel_args_from_string (val);
  _ostree_kernel_args_replace_take (kargs, ostree_kernel_arg);
  ostree_kernel_arg = NULL;
  options_key = _ostree_kernel_args_to_string (kargs);
  ostree_bootconfig_parser_set (bootconfig, "options", options_key);
  
  { glnx_fd_close int bootconf_dfd = -1;

    if (!glnx_opendirat (boot_dfd, bootconfdir, TRUE, &bootconf_dfd, error))
      goto out;

    if (!ostree_bootconfig_parser_write_at (ostree_deployment_get_bootconfig (deployment),
                                            bootconf_dfd, bootconf_name,
                                            cancellable, error))
      goto out;
  }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
prepare_new_bootloader_link (OstreeSysroot  *sysroot,
                             int             current_bootversion,
                             int             new_bootversion,
                             GCancellable   *cancellable,
                             GError        **error)
{
  gboolean ret = FALSE;
  g_autofree char *new_target = NULL;

  g_assert ((current_bootversion == 0 && new_bootversion == 1) ||
            (current_bootversion == 1 && new_bootversion == 0));

  new_target = g_strdup_printf ("loader.%d", new_bootversion);

  /* We shouldn't actually need to replace but it's easier to reuse
     that code */
  if (!symlink_at_replace (new_target, sysroot->sysroot_fd, "boot/loader.tmp",
                           cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
swap_bootloader (OstreeSysroot  *sysroot,
                 int             current_bootversion,
                 int             new_bootversion,
                 GCancellable   *cancellable,
                 GError        **error)
{
  gboolean ret = FALSE;
  glnx_fd_close int boot_dfd = -1;
  int res;

  g_assert ((current_bootversion == 0 && new_bootversion == 1) ||
            (current_bootversion == 1 && new_bootversion == 0));

  if (!glnx_opendirat (sysroot->sysroot_fd, "boot", TRUE, &boot_dfd, error))
    goto out;

  /* The symlink was already written, and we used syncfs() to ensure
   * its data is in place.  Renaming now should give us atomic semantics;
   * see https://bugzilla.gnome.org/show_bug.cgi?id=755595
   */
  do
    res = renameat (boot_dfd, "loader.tmp", boot_dfd, "loader");
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

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
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
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
    __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *a_kargs = NULL;
    __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *b_kargs = NULL;
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
  gboolean ret = FALSE;
  guint i;
  g_autoptr(GHashTable) created_current_for_osname =
    g_hash_table_new (g_str_hash, g_str_equal);

  for (i = 0; i < self->deployments->len; i++)
    {
      OstreeDeployment *deployment = self->deployments->pdata[i];
      const char *osname = ostree_deployment_get_osname (deployment);
      g_autoptr(GFile) osdir = ot_gfile_resolve_path_printf (self->path, "ostree/deploy/%s", osname);
      g_autoptr(GFile) legacy_link = g_file_get_child (osdir, "current");

      if (!ot_gfile_ensure_unlinked (legacy_link, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

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
  mnt_free_cache (cache);
  mnt_free_table (tb);

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
 * Assuming @new_deployments have already been deployed in place on
 * disk, atomically update bootloader configuration.
 */
gboolean
ostree_sysroot_write_deployments (OstreeSysroot     *self,
                                  GPtrArray         *new_deployments,
                                  GCancellable      *cancellable,
                                  GError           **error)
{
  gboolean ret = FALSE;
  guint i;
  gboolean requires_new_bootversion = FALSE;
  gboolean found_booted_deployment = FALSE;
  gboolean bootloader_is_atomic = FALSE;
  gboolean boot_was_ro_mount = FALSE;

  g_assert (self->loaded);

  /* Assign a bootserial to each new deployment.
   */
  assign_bootserials (new_deployments);

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
      
      if (!full_system_sync (self, cancellable, error))
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
      glnx_unref_object OstreeBootloader *bootloader = NULL;
      g_autoptr(GFile) new_loader_entries_dir = NULL;
      glnx_unref_object OstreeRepo *repo = NULL;
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

      new_loader_entries_dir = ot_gfile_resolve_path_printf (self->path, "boot/loader.%d/entries",
                                                             new_bootversion);
      if (!glnx_shutil_rm_rf_at (AT_FDCWD, gs_file_get_path_cached (new_loader_entries_dir), cancellable, error))
        goto out;
      if (!ot_util_ensure_directory_and_fsync (new_loader_entries_dir, cancellable, error))
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

      if (!full_system_sync (self, cancellable, error))
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

  ot_log_structured_print_id_v (OSTREE_DEPLOYMENT_COMPLETE_ID,
                                "%s; bootconfig swap: %s deployment count change: %i",
                                (bootloader_is_atomic ? "Transaction complete" : "Bootloader updated"),
                                requires_new_bootversion ? "yes" : "no",
                                new_deployments->len - self->deployments->len);

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
  if (!ostree_sysroot_cleanup (self, cancellable, error))
    {
      g_prefix_error (error, "Performing final cleanup: ");
      goto out;
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
  gboolean ret = FALSE;
  guint i;
  int new_deployserial = 0;
  g_autoptr(GFile) osdir = NULL;
  g_autoptr(GPtrArray) tmp_current_deployments =
    g_ptr_array_new_with_free_func (g_object_unref);

  osdir = ot_gfile_get_child_build_path (self->path, "ostree/deploy", osname, NULL);
  
  if (!_ostree_sysroot_list_deployment_dirs_for_os (osdir, tmp_current_deployments,
                                                    cancellable, error))
    goto out;

  for (i = 0; i < tmp_current_deployments->len; i++)
    {
      OstreeDeployment *deployment = tmp_current_deployments->pdata[i];
      
      if (strcmp (ostree_deployment_get_osname (deployment), osname) != 0)
        continue;
      if (strcmp (ostree_deployment_get_csum (deployment), revision) != 0)
        continue;

      new_deployserial = MAX(new_deployserial, ostree_deployment_get_deployserial (deployment)+1);
    }

  ret = TRUE;
  *out_deployserial = new_deployserial;
 out:
  return ret;
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
  gboolean ret = FALSE;
  gint new_deployserial;
  glnx_unref_object OstreeDeployment *new_deployment = NULL;
  glnx_unref_object OstreeDeployment *merge_deployment = NULL;
  glnx_unref_object OstreeRepo *repo = NULL;
  g_autoptr(GFile) osdeploydir = NULL;
  g_autoptr(GFile) deployment_var = NULL;
  glnx_fd_close int tree_boot_dfd = -1;
  g_autofree char *tree_kernel_path = NULL;
  g_autofree char *tree_initramfs_path = NULL;
  glnx_fd_close int deployment_dfd = -1;
  glnx_unref_object OstreeSePolicy *sepolicy = NULL;
  g_autofree char *new_bootcsum = NULL;
  glnx_unref_object OstreeBootconfigParser *bootconfig = NULL;

  g_return_val_if_fail (osname != NULL || self->booted_deployment != NULL, FALSE);

  if (osname == NULL)
    osname = ostree_deployment_get_osname (self->booted_deployment);

  osdeploydir = ot_gfile_get_child_build_path (self->path, "ostree", "deploy", osname, NULL);
  if (!g_file_query_exists (osdeploydir, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No OS named \"%s\" known", osname);
      goto out;
    }

  deployment_var = g_file_get_child (osdeploydir, "var");

  if (!ostree_sysroot_get_repo (self, &repo, cancellable, error))
    goto out;

  if (provided_merge_deployment != NULL)
    merge_deployment = g_object_ref (provided_merge_deployment);

  if (!allocate_deployserial (self, osname, revision, &new_deployserial,
                              cancellable, error))
    goto out;

  new_deployment = ostree_deployment_new (0, osname, revision, new_deployserial,
                                          new_bootcsum, -1);
  ostree_deployment_set_origin (new_deployment, origin);

  /* Check out the userspace tree onto the filesystem */
  if (!checkout_deployment_tree (self, repo, new_deployment, &deployment_dfd,
                                 cancellable, error))
    {
      g_prefix_error (error, "Checking out tree: ");
      goto out;
    }

  if (!get_kernel_from_tree (deployment_dfd, &tree_boot_dfd,
                             &tree_kernel_path, &tree_initramfs_path,
                             cancellable, error))
    goto out;
  
  if (tree_initramfs_path != NULL)
    {
      if (!checksum_from_kernel_src (tree_initramfs_path, &new_bootcsum, error))
        goto out;
    }
  else
    {
      if (!checksum_from_kernel_src (tree_kernel_path, &new_bootcsum, error))
        goto out;
    }
  
  _ostree_deployment_set_bootcsum (new_deployment, new_bootcsum);

  /* Create an empty boot configuration; we will merge things into
   * it as we go.
   */
  bootconfig = ostree_bootconfig_parser_new ();
  ostree_deployment_set_bootconfig (new_deployment, bootconfig);

  if (!merge_configuration (self, merge_deployment, new_deployment,
                            deployment_dfd,
                            &sepolicy,
                            cancellable, error))
    {
      g_prefix_error (error, "During /etc merge: ");
      goto out;
    }

  g_clear_object (&self->sepolicy);
  self->sepolicy = g_object_ref (sepolicy);

  if (!selinux_relabel_var_if_needed (self, sepolicy, deployment_var,
                                      cancellable, error))
    goto out;

  if (!(self->debug_flags & OSTREE_SYSROOT_DEBUG_MUTABLE_DEPLOYMENTS))
    {
      if (!ostree_sysroot_deployment_set_mutable (self, new_deployment, FALSE,
                                                  cancellable, error))
        goto out;
    }

  { ostree_cleanup_sepolicy_fscreatecon gpointer dummy = NULL;

    /* Explicitly override the label for the origin file to ensure
     * it's system_conf_t.
     */
    if (self->sepolicy != NULL
        && ostree_sepolicy_get_name (self->sepolicy) != NULL)
      {
        if (!ostree_sepolicy_setfscreatecon (self->sepolicy,
                                             "/etc/ostree/remotes.d/dummy.conf",
                                             0644,
                                             error))
          goto out;
      }

    if (!ostree_sysroot_write_origin_file (self, new_deployment, NULL,
                                           cancellable, error))
      {
        g_prefix_error (error, "Writing out origin file: ");
        goto out;
      }
  }

  /* After this, install_deployment_kernel() will set the other boot
   * options and write it out to disk.
   */
  if (override_kernel_argv)
    {
      __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *kargs = NULL;
      g_autofree char *new_options = NULL;

      kargs = _ostree_kernel_args_new ();
      _ostree_kernel_args_append_argv (kargs, override_kernel_argv);
      new_options = _ostree_kernel_args_to_string (kargs);
      ostree_bootconfig_parser_set (bootconfig, "options", new_options);
    }

  ret = TRUE;
  ot_transfer_out_value (out_new_deployment, &new_deployment);
 out:
  return ret;
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
  gboolean ret = FALSE;
  guint i;
  g_autoptr(GPtrArray) new_deployments = g_ptr_array_new_with_free_func (g_object_unref);
  glnx_unref_object OstreeDeployment *new_deployment = NULL;
  __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *kargs = NULL;
  g_autofree char *new_options = NULL;
  OstreeBootconfigParser *new_bootconfig;

  new_deployment = ostree_deployment_clone (deployment);
  new_bootconfig = ostree_deployment_get_bootconfig (new_deployment);

  kargs = _ostree_kernel_args_new ();
  _ostree_kernel_args_append_argv (kargs, new_kargs);
  new_options = _ostree_kernel_args_to_string (kargs);
  ostree_bootconfig_parser_set (new_bootconfig, "options", new_options);

  for (i = 0; i < self->deployments->len; i++)
    {
      OstreeDeployment *cur = self->deployments->pdata[i];
      if (cur == deployment)
        g_ptr_array_add (new_deployments, g_object_ref (new_deployment));
      else
        g_ptr_array_add (new_deployments, g_object_ref (cur));
    }

  if (!ostree_sysroot_write_deployments (self, new_deployments,
                                         cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_sysroot_deployment_set_mutable:
 * @self: Sysroot
 * @deployment: A deployment
 * @is_mutable: Whether or not deployment's files can be changed
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
  gboolean ret = FALSE;
  g_autofree char *deployment_path = ostree_sysroot_get_deployment_dirpath (self, deployment);
  glnx_fd_close int fd = -1;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (!glnx_opendirat (self->sysroot_fd, deployment_path, TRUE, &fd, error))
    goto out;

  if (!_ostree_linuxfs_fd_alter_immutable_flag (fd, !is_mutable, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
