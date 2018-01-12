/*
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <glib-unix.h>
#include <sys/xattr.h>
#include <gio/gfiledescriptorbased.h>
#include <gio/gunixoutputstream.h>
#include "otutil.h"

#include "ostree-repo-file.h"
#include "ostree-sepolicy-private.h"
#include "ostree-core-private.h"
#include "ostree-repo-private.h"

#define WHITEOUT_PREFIX ".wh."

/* Per-checkout call state/caching */
typedef struct {
  GString *selabel_path_buf;
} CheckoutState;

static void
checkout_state_clear (CheckoutState *state)
{
  if (state->selabel_path_buf)
    g_string_free (state->selabel_path_buf, TRUE);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(CheckoutState, checkout_state_clear)

static gboolean
checkout_object_for_uncompressed_cache (OstreeRepo      *self,
                                        const char      *loose_path,
                                        GFileInfo       *src_info,
                                        GInputStream    *content,
                                        GCancellable    *cancellable,
                                        GError         **error)
{
  /* Don't make setuid files in uncompressed cache */
  guint32 file_mode = g_file_info_get_attribute_uint32 (src_info, "unix::mode");
  file_mode &= ~(S_ISUID|S_ISGID);

  g_auto(GLnxTmpfile) tmpf = { 0, };
  if (!glnx_open_tmpfile_linkable_at (self->tmp_dir_fd, ".", O_WRONLY | O_CLOEXEC,
                                      &tmpf, error))
    return FALSE;
  g_autoptr(GOutputStream) temp_out = g_unix_output_stream_new (tmpf.fd, FALSE);

  if (g_output_stream_splice (temp_out, content, 0, cancellable, error) < 0)
    return FALSE;

  if (!g_output_stream_flush (temp_out, cancellable, error))
    return FALSE;

  if (!self->disable_fsync)
    {
      if (TEMP_FAILURE_RETRY (fsync (tmpf.fd)) < 0)
        return glnx_throw_errno (error);
    }

  if (!g_output_stream_close (temp_out, cancellable, error))
    return FALSE;

  if (!glnx_fchmod (tmpf.fd, file_mode, error))
    return FALSE;

  if (!_ostree_repo_ensure_loose_objdir_at (self->uncompressed_objects_dir_fd,
                                            loose_path,
                                            cancellable, error))
    return FALSE;

  if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_NOREPLACE_IGNORE_EXIST,
                             self->uncompressed_objects_dir_fd, loose_path,
                             error))
    return FALSE;

  return TRUE;
}

static gboolean
fsync_is_enabled (OstreeRepo   *self,
                  OstreeRepoCheckoutAtOptions *options)
{
  return options->enable_fsync;
}

static gboolean
write_regular_file_content (OstreeRepo            *self,
                            OstreeRepoCheckoutAtOptions *options,
                            int                    outfd,
                            GFileInfo             *file_info,
                            GVariant              *xattrs,
                            GInputStream          *input,
                            GCancellable          *cancellable,
                            GError               **error)
{
  const OstreeRepoCheckoutMode mode = options->mode;
  g_autoptr(GOutputStream) outstream = NULL;

  if (G_IS_FILE_DESCRIPTOR_BASED (input))
    {
      int infd = g_file_descriptor_based_get_fd ((GFileDescriptorBased*) input);
      guint64 len = g_file_info_get_size (file_info);

      if (glnx_regfile_copy_bytes (infd, outfd, (off_t)len) < 0)
        return glnx_throw_errno_prefix (error, "regfile copy");
    }
  else
    {
      outstream = g_unix_output_stream_new (outfd, FALSE);
      if (g_output_stream_splice (outstream, input, 0,
                                  cancellable, error) < 0)
        return FALSE;

      if (!g_output_stream_flush (outstream, cancellable, error))
        return FALSE;
    }

  if (mode != OSTREE_REPO_CHECKOUT_MODE_USER)
    {
      if (TEMP_FAILURE_RETRY (fchown (outfd, g_file_info_get_attribute_uint32 (file_info, "unix::uid"),
                                      g_file_info_get_attribute_uint32 (file_info, "unix::gid"))) < 0)
        return glnx_throw_errno_prefix (error, "fchown");

      if (xattrs)
        {
          if (!glnx_fd_set_all_xattrs (outfd, xattrs, cancellable, error))
            return FALSE;
        }
    }

  guint32 file_mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");

  /* Don't make setuid files on checkout when we're doing --user */
  if (mode == OSTREE_REPO_CHECKOUT_MODE_USER)
    file_mode &= ~(S_ISUID|S_ISGID);

  if (TEMP_FAILURE_RETRY (fchmod (outfd, file_mode)) < 0)
    return glnx_throw_errno_prefix (error, "fchmod");

  if (fsync_is_enabled (self, options))
    {
      if (fsync (outfd) == -1)
        return glnx_throw_errno_prefix (error, "fsync");
    }

  if (outstream)
    {
      if (!g_output_stream_close (outstream, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/*
 * Create a copy of a file, supporting optional union/add behavior.
 */
static gboolean
create_file_copy_from_input_at (OstreeRepo     *repo,
                                OstreeRepoCheckoutAtOptions  *options,
                                CheckoutState  *state,
                                GFileInfo      *file_info,
                                GVariant       *xattrs,
                                GInputStream   *input,
                                int             destination_dfd,
                                const char     *destination_name,
                                GCancellable   *cancellable,
                                GError        **error)
{
  const gboolean sepolicy_enabled = options->sepolicy && !repo->disable_xattrs;
  g_autoptr(GVariant) modified_xattrs = NULL;

  /* If we're doing SELinux labeling, prepare it */
  if (sepolicy_enabled)
    {
      /* If doing sepolicy path-based labeling, we don't want to set the
       * security.selinux attr via the generic xattr paths in either the symlink
       * or regfile cases, so filter it out.
       */
      modified_xattrs = _ostree_filter_selinux_xattr (xattrs);
      xattrs = modified_xattrs;
    }

  if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      g_auto(OstreeSepolicyFsCreatecon) fscreatecon = { 0, };

      if (sepolicy_enabled)
        {
          /* For symlinks, since we don't have O_TMPFILE, we use setfscreatecon() */
          if (!_ostree_sepolicy_preparefscreatecon (&fscreatecon, options->sepolicy,
                                                    state->selabel_path_buf->str,
                                                    g_file_info_get_attribute_uint32 (file_info, "unix::mode"),
                                                    error))
            return FALSE;
        }

      const char *target = g_file_info_get_symlink_target (file_info);
      if (symlinkat (target, destination_dfd, destination_name) < 0)
        {
          if (errno != EEXIST)
            return glnx_throw_errno_prefix (error, "symlinkat");

          /* Handle union/add behaviors if we get EEXIST */
          switch (options->overwrite_mode)
            {
            case OSTREE_REPO_CHECKOUT_OVERWRITE_NONE:
              return glnx_throw_errno_prefix (error, "symlinkat");
            case OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES:
              {
                /* For unioning, we further bifurcate a bit; for the "process whiteouts"
                 * mode which is really "Docker/OCI", we need to match their semantics
                 * and handle replacing a directory with a symlink.  See also equivalent
                 * bits for regular files in checkout_file_hardlink().
                 */
                if (options->process_whiteouts)
                  {
                    if (!glnx_shutil_rm_rf_at (destination_dfd, destination_name, NULL, error))
                      return FALSE;
                  }
                else
                  {
                    if (unlinkat (destination_dfd, destination_name, 0) < 0)
                      {
                        if (G_UNLIKELY (errno != ENOENT))
                          return glnx_throw_errno_prefix (error, "unlinkat(%s)", destination_name);
                      }
                  }
                if (symlinkat (target, destination_dfd, destination_name) < 0)
                  return glnx_throw_errno_prefix (error, "symlinkat");
              }
            case OSTREE_REPO_CHECKOUT_OVERWRITE_ADD_FILES:
              /* Note early return - we don't want to set the xattrs below */
              return TRUE;
            case OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL:
              {
                /* See the comments for the hardlink version of this
                 * for why we do this.
                 */
                struct stat dest_stbuf;
                if (!glnx_fstatat (destination_dfd, destination_name, &dest_stbuf,
                                   AT_SYMLINK_NOFOLLOW, error))
                  return FALSE;
                if (S_ISLNK (dest_stbuf.st_mode))
                  {
                    g_autofree char *dest_target =
                      glnx_readlinkat_malloc (destination_dfd, destination_name,
                                              cancellable, error);
                    if (!dest_target)
                      return FALSE;
                    /* In theory we could also compare xattrs...but eh */
                    if (g_str_equal (dest_target, target))
                      return TRUE;
                  }
                errno = EEXIST;
                return glnx_throw_errno_prefix (error, "symlinkat");
              }
            }
        }

      /* Process ownership and xattrs now that we made the link */
      if (options->mode != OSTREE_REPO_CHECKOUT_MODE_USER)
        {
          if (TEMP_FAILURE_RETRY (fchownat (destination_dfd, destination_name,
                                            g_file_info_get_attribute_uint32 (file_info, "unix::uid"),
                                            g_file_info_get_attribute_uint32 (file_info, "unix::gid"),
                                            AT_SYMLINK_NOFOLLOW)) == -1)
            return glnx_throw_errno_prefix (error, "fchownat");

          if (xattrs != NULL &&
              !glnx_dfd_name_set_all_xattrs (destination_dfd, destination_name,
                                             xattrs, cancellable, error))
            return FALSE;
        }
    }
  else if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
      g_auto(GLnxTmpfile) tmpf = { 0, };

      if (!glnx_open_tmpfile_linkable_at (destination_dfd, ".", O_WRONLY | O_CLOEXEC,
                                          &tmpf, error))
        return FALSE;

      if (sepolicy_enabled && options->mode != OSTREE_REPO_CHECKOUT_MODE_USER)
        {
          g_autofree char *label = NULL;
          if (!ostree_sepolicy_get_label (options->sepolicy, state->selabel_path_buf->str,
                                          g_file_info_get_attribute_uint32 (file_info, "unix::mode"),
                                          &label, cancellable, error))
            return FALSE;

          if (fsetxattr (tmpf.fd, "security.selinux", label, strlen (label), 0) < 0)
            return glnx_throw_errno_prefix (error, "Setting security.selinux");
        }

      if (!write_regular_file_content (repo, options, tmpf.fd, file_info, xattrs, input,
                                       cancellable, error))
        return FALSE;

      /* The add/union/none behaviors map directly to GLnxLinkTmpfileReplaceMode */
      GLnxLinkTmpfileReplaceMode replace_mode = GLNX_LINK_TMPFILE_NOREPLACE;
      switch (options->overwrite_mode)
        {
        case OSTREE_REPO_CHECKOUT_OVERWRITE_NONE:
          /* Handled above */
          break;
        case OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES:
          /* Special case OCI/Docker - see similar code in checkout_file_hardlink()
           * and above for symlinks.
           */
          if (options->process_whiteouts)
            {
              if (!glnx_shutil_rm_rf_at (destination_dfd, destination_name, NULL, error))
                return FALSE;
              /* Inherit the NOREPLACE default...we deleted whatever's there */
            }
          else
            replace_mode = GLNX_LINK_TMPFILE_REPLACE;
          break;
        case OSTREE_REPO_CHECKOUT_OVERWRITE_ADD_FILES:
          replace_mode = GLNX_LINK_TMPFILE_NOREPLACE_IGNORE_EXIST;
          break;
        case OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL:
          /* We don't support copying in union identical */
          g_assert_not_reached ();
          break;
        }

      if (!glnx_link_tmpfile_at (&tmpf, replace_mode,
                                 destination_dfd, destination_name,
                                 error))
        return FALSE;
    }
  else
    g_assert_not_reached ();

  return TRUE;
}

typedef enum {
  HARDLINK_RESULT_NOT_SUPPORTED,
  HARDLINK_RESULT_SKIP_EXISTED,
  HARDLINK_RESULT_LINKED
} HardlinkResult;

/* Used for OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES.  In
 * order to atomically replace a target, we add a new link
 * in self->tmp_dir_fd, with a name placed into the mutable
 * buffer @tmpname.
 */
static gboolean
hardlink_add_tmp_name (OstreeRepo              *self,
                       int                      srcfd,
                       const char              *loose_path,
                       char                    *tmpname,
                       GCancellable            *cancellable,
                       GError                 **error)
{
  const int max_attempts = 128;
  guint i;

  for (i = 0; i < max_attempts; i++)
    {
      glnx_gen_temp_name (tmpname);
      if (linkat (srcfd, loose_path, self->tmp_dir_fd, tmpname, 0) < 0)
        {
          if (errno == EEXIST)
            continue;
          else
            return glnx_throw_errno_prefix (error, "linkat");
        }
      else
        break;
    }
  if (i == max_attempts)
    return glnx_throw (error, "Exhausted attempts to make temporary hardlink");

  return TRUE;
}

static gboolean
checkout_file_hardlink (OstreeRepo                          *self,
                        const char                          *checksum,
                        OstreeRepoCheckoutAtOptions         *options,
                        const char                          *loose_path,
                        int                                  destination_dfd,
                        const char                          *destination_name,
                        gboolean                             allow_noent,
                        HardlinkResult                      *out_result,
                        GCancellable                        *cancellable,
                        GError                             **error)
{
  HardlinkResult ret_result = HARDLINK_RESULT_NOT_SUPPORTED;
  int srcfd = _ostree_repo_mode_is_bare (self->mode) ?
    self->objects_dir_fd : self->uncompressed_objects_dir_fd;

  if (linkat (srcfd, loose_path, destination_dfd, destination_name, 0) == 0)
    ret_result = HARDLINK_RESULT_LINKED;
  else if (!options->no_copy_fallback && (errno == EMLINK || errno == EXDEV || errno == EPERM))
    {
      /* EMLINK, EXDEV and EPERM shouldn't be fatal; we just can't do the
       * optimization of hardlinking instead of copying.
       */
    }
  else if (allow_noent && errno == ENOENT)
    {
    }
  else if (errno == EEXIST)
    {
      /* When we get EEXIST, we need to handle the different overwrite modes. */
      switch (options->overwrite_mode)
        {
        case OSTREE_REPO_CHECKOUT_OVERWRITE_NONE:
          /* Just throw */
          return glnx_throw_errno_prefix (error, "Hardlinking %s to %s", loose_path, destination_name);
        case OSTREE_REPO_CHECKOUT_OVERWRITE_ADD_FILES:
          /* In this mode, we keep existing content.  Distinguish this case though to
           * avoid inserting into the devino cache.
           */
          ret_result = HARDLINK_RESULT_SKIP_EXISTED;
          break;
        case OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES:
        case OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL:
          {
            /* In both union-files and union-identical, see if the src/target are
             * already hardlinked.  If they are, we're done.
             *
             * If not, for union-identical we error out, which is what
             * rpm-ostree wants for package layering.
             * https://github.com/projectatomic/rpm-ostree/issues/982
             * This should be similar to the librpm version:
             * https://github.com/rpm-software-management/rpm/blob/e3cd2bc85e0578f158d14e6f9624eb955c32543b/lib/rpmfi.c#L921
             * in rpmfilesCompare().
             *
             * For union-files, we make a temporary link, then rename() it
             * into place.
             */
            struct stat src_stbuf;
            if (!glnx_fstatat (srcfd, loose_path, &src_stbuf,
                               AT_SYMLINK_NOFOLLOW, error))
              return FALSE;
            struct stat dest_stbuf;
            if (!glnx_fstatat (destination_dfd, destination_name, &dest_stbuf,
                               AT_SYMLINK_NOFOLLOW, error))
              return FALSE;
            gboolean is_identical =
              (src_stbuf.st_dev == dest_stbuf.st_dev &&
               src_stbuf.st_ino == dest_stbuf.st_ino);
            if (!is_identical && (_ostree_stbuf_equal (&src_stbuf, &dest_stbuf)))
              {
                /* As a last resort, do a checksum comparison. This is the case currently
                 * with rpm-ostree pkg layering where we overlay from the pkgcache repo onto
                 * a tree checked out from the system repo. Once those are united, we
                 * shouldn't hit this anymore. https://github.com/ostreedev/ostree/pull/1258
                 * */
                OstreeChecksumFlags flags = 0;
                if (self->disable_xattrs)
                    flags |= OSTREE_CHECKSUM_FLAGS_IGNORE_XATTRS;

                g_autofree char *actual_checksum = NULL;
                if (!ostree_checksum_file_at (destination_dfd, destination_name,
                                              &dest_stbuf, OSTREE_OBJECT_TYPE_FILE,
                                              flags, &actual_checksum, cancellable, error))
                  return FALSE;

                is_identical = g_str_equal (checksum, actual_checksum);
              }
            if (is_identical)
              ret_result = HARDLINK_RESULT_SKIP_EXISTED;
            else if (options->overwrite_mode == OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES)
              {
                char *tmpname = strdupa ("checkout-union-XXXXXX");
                /* Make a link with a temp name */
                if (!hardlink_add_tmp_name (self, srcfd, loose_path, tmpname, cancellable, error))
                  return FALSE;
                /* For OCI/Docker mode, we need to handle replacing a directory with a regular
                 * file.  See also the equivalent code for symlinks above.
                 */
                if (options->process_whiteouts)
                  {
                    if (!glnx_shutil_rm_rf_at (destination_dfd, destination_name, NULL, error))
                      return FALSE;
                  }
                /* Rename it into place - for non-OCI this will overwrite files but not directories */
                if (!glnx_renameat (self->tmp_dir_fd, tmpname, destination_dfd, destination_name, error))
                  return FALSE;
                ret_result = HARDLINK_RESULT_LINKED;
              }
            else
              {
                g_assert_cmpint (options->overwrite_mode, ==, OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL);
                return glnx_throw_errno_prefix (error, "Hardlinking %s to %s", loose_path, destination_name);
              }
            break;
          }
        }
    }
  else
    {
      return glnx_throw_errno_prefix (error, "Hardlinking %s to %s", loose_path, destination_name);
    }

  if (out_result)
    *out_result = ret_result;
  return TRUE;
}

static gboolean
checkout_one_file_at (OstreeRepo                        *repo,
                      OstreeRepoCheckoutAtOptions         *options,
                      CheckoutState                     *state,
                      const char                        *checksum,
                      int                                destination_dfd,
                      const char                        *destination_name,
                      GCancellable                      *cancellable,
                      GError                           **error)
{
  /* Validate this up front to prevent path traversal attacks */
  if (!ot_util_filename_validate (destination_name, error))
    return FALSE;

  gboolean need_copy = TRUE;
  gboolean is_bare_user_symlink = FALSE;
  char loose_path_buf[_OSTREE_LOOSE_PATH_MAX];

  /* FIXME - avoid the GFileInfo here */
  g_autoptr(GFileInfo) source_info = NULL;
  if (!ostree_repo_load_file (repo, checksum, NULL, &source_info, NULL,
                              cancellable, error))
    return FALSE;

  const gboolean is_symlink = (g_file_info_get_file_type (source_info) == G_FILE_TYPE_SYMBOLIC_LINK);
  const gboolean is_whiteout = (!is_symlink && options->process_whiteouts &&
                                g_str_has_prefix (destination_name, WHITEOUT_PREFIX));

  /* First, see if it's a Docker whiteout,
   * https://github.com/docker/docker/blob/1a714e76a2cb9008cd19609059e9988ff1660b78/pkg/archive/whiteouts.go
   */
  if (is_whiteout)
    {
      const char *name = destination_name + (sizeof (WHITEOUT_PREFIX) - 1);

      if (!name[0])
        return glnx_throw (error, "Invalid empty whiteout '%s'", name);

      g_assert (name[0] != '/'); /* Sanity */

      if (!glnx_shutil_rm_rf_at (destination_dfd, name, cancellable, error))
        return FALSE;

      need_copy = FALSE;
    }
  else if (!options->force_copy)
    {
      HardlinkResult hardlink_res = HARDLINK_RESULT_NOT_SUPPORTED;
      /* Try to do a hardlink first, if it's a regular file.  This also
       * traverses all parent repos.
       */
      OstreeRepo *current_repo = repo;

      while (current_repo)
        {
          /* TODO - Hoist this up to the toplevel at least for checking out from
           * !parent; don't need to compute it for each file.
           */
          gboolean repo_is_usermode =
            current_repo->mode == OSTREE_REPO_MODE_BARE_USER ||
            current_repo->mode == OSTREE_REPO_MODE_BARE_USER_ONLY;
          /* We're hardlinkable if the checkout mode matches the repo mode */
          gboolean is_hardlinkable =
            (current_repo->mode == OSTREE_REPO_MODE_BARE
             && options->mode == OSTREE_REPO_CHECKOUT_MODE_NONE) ||
            (repo_is_usermode && options->mode == OSTREE_REPO_CHECKOUT_MODE_USER);
          gboolean current_can_cache = (options->enable_uncompressed_cache
                                        && current_repo->enable_uncompressed_cache);
          gboolean is_archive_z2_with_cache = (current_repo->mode == OSTREE_REPO_MODE_ARCHIVE
                                               && options->mode == OSTREE_REPO_CHECKOUT_MODE_USER
                                               && current_can_cache);

          /* NOTE: bare-user symlinks are not stored as symlinks; see
           * https://github.com/ostreedev/ostree/commit/47c612e5a0688c3452a125655a245e8f4f01b2b0
           * as well as write_object().
           */
          is_bare_user_symlink = (repo_is_usermode && is_symlink);
          const gboolean is_bare = is_hardlinkable && !is_bare_user_symlink;

          /* Verify if no_copy_fallback is set that we can hardlink, with a
           * special exception for bare-user symlinks.
           */
          if (options->no_copy_fallback && !is_hardlinkable && !is_bare_user_symlink)
            return glnx_throw (error,
                               repo_is_usermode ?
                               "User repository mode requires user checkout mode to hardlink" :
                               "Bare repository mode cannot hardlink in user checkout mode");

          /* But only under these conditions */
          if (is_bare || is_archive_z2_with_cache)
            {
              /* Override repo mode; for archive we're looking in
                 the cache, which is in "bare" form */
              _ostree_loose_path (loose_path_buf, checksum, OSTREE_OBJECT_TYPE_FILE, OSTREE_REPO_MODE_BARE);
              if (!checkout_file_hardlink (current_repo,
                                           checksum,
                                           options,
                                           loose_path_buf,
                                           destination_dfd, destination_name,
                                           TRUE, &hardlink_res,
                                           cancellable, error))
                return FALSE;

              if (hardlink_res == HARDLINK_RESULT_LINKED && options->devino_to_csum_cache)
                {
                  struct stat stbuf;
                  OstreeDevIno *key;

                  if (TEMP_FAILURE_RETRY (fstatat (destination_dfd, destination_name, &stbuf, AT_SYMLINK_NOFOLLOW)) != 0)
                    return glnx_throw_errno (error);

                  key = g_new (OstreeDevIno, 1);
                  key->dev = stbuf.st_dev;
                  key->ino = stbuf.st_ino;
                  memcpy (key->checksum, checksum, OSTREE_SHA256_STRING_LEN+1);

                  g_hash_table_add ((GHashTable*)options->devino_to_csum_cache, key);
                }

              if (hardlink_res != HARDLINK_RESULT_NOT_SUPPORTED)
                break;
            }
          current_repo = current_repo->parent_repo;
        }

      need_copy = (hardlink_res == HARDLINK_RESULT_NOT_SUPPORTED);
    }

  const gboolean can_cache = (options->enable_uncompressed_cache
                              && repo->enable_uncompressed_cache);

  g_autoptr(GInputStream) input = NULL;
  g_autoptr(GVariant) xattrs = NULL;

  /* Ok, if we're archive and we didn't find an object, uncompress
   * it now, stick it in the cache, and then hardlink to that.
   */
  if (can_cache
      && !is_whiteout
      && !is_symlink
      && need_copy
      && repo->mode == OSTREE_REPO_MODE_ARCHIVE
      && options->mode == OSTREE_REPO_CHECKOUT_MODE_USER)
    {
      HardlinkResult hardlink_res = HARDLINK_RESULT_NOT_SUPPORTED;

      if (!ostree_repo_load_file (repo, checksum, &input, NULL, NULL,
                                  cancellable, error))
        return FALSE;

      /* Overwrite any parent repo from earlier */
      _ostree_loose_path (loose_path_buf, checksum, OSTREE_OBJECT_TYPE_FILE, OSTREE_REPO_MODE_BARE);

      if (!checkout_object_for_uncompressed_cache (repo, loose_path_buf,
                                                   source_info, input,
                                                   cancellable, error))
        return glnx_prefix_error (error, "Unpacking loose object %s", checksum);

      g_clear_object (&input);

      /* Store the 2-byte objdir prefix (e.g. e3) in a set.  The basic
       * idea here is that if we had to unpack an object, it's very
       * likely we're replacing some other object, so we may need a GC.
       *
       * This model ensures that we do work roughly proportional to
       * the size of the changes.  For example, we don't scan any
       * directories if we didn't modify anything, meaning you can
       * checkout the same tree multiple times very quickly.
       *
       * This is also scale independent; we don't hardcode e.g. looking
       * at 1000 objects.
       *
       * The downside is that if we're unlucky, we may not free
       * an object for quite some time.
       */
      g_mutex_lock (&repo->cache_lock);
      {
        gpointer key = GUINT_TO_POINTER ((g_ascii_xdigit_value (checksum[0]) << 4) + 
                                         g_ascii_xdigit_value (checksum[1]));
        if (repo->updated_uncompressed_dirs == NULL)
          repo->updated_uncompressed_dirs = g_hash_table_new (NULL, NULL);
        g_hash_table_add (repo->updated_uncompressed_dirs, key);
      }
      g_mutex_unlock (&repo->cache_lock);

      if (!checkout_file_hardlink (repo, checksum, options, loose_path_buf,
                                   destination_dfd, destination_name,
                                   FALSE, &hardlink_res,
                                   cancellable, error))
        return glnx_prefix_error (error, "Using new cached uncompressed hardlink of %s to %s", checksum, destination_name);

      need_copy = (hardlink_res == HARDLINK_RESULT_NOT_SUPPORTED);
    }

  /* Fall back to copy if we couldn't hardlink */
  if (need_copy)
    {
      /* Bare user mode can't hardlink symlinks, so we need to do a copy for
       * those. (Although in the future we could hardlink inside checkouts) This
       * assertion is intended to ensure that for regular files at least, we
       * succeeded at hardlinking above.
       */
      if (options->no_copy_fallback)
        g_assert (is_bare_user_symlink);
      if (!ostree_repo_load_file (repo, checksum, &input, NULL, &xattrs,
                                  cancellable, error))
        return FALSE;

      if (!create_file_copy_from_input_at (repo, options, state, source_info, xattrs, input,
                                           destination_dfd, destination_name,
                                           cancellable, error))
        return glnx_prefix_error (error, "Copy checkout of %s to %s", checksum, destination_name);

      if (input)
        {
          if (!g_input_stream_close (input, cancellable, error))
            return FALSE;
        }
    }

  return TRUE;
}

/*
 * checkout_tree_at:
 * @self: Repo
 * @mode: Options controlling all files
 * @state: Any state we're carrying through
 * @overwrite_mode: Whether or not to overwrite files
 * @destination_parent_fd: Place tree here
 * @destination_name: Use this name for tree
 * @source: Source tree
 * @source_info: Source info
 * @cancellable: Cancellable
 * @error: Error
 *
 * Like ostree_repo_checkout_tree(), but check out @source into the
 * relative @destination_name, located by @destination_parent_fd.
 */
static gboolean
checkout_tree_at_recurse (OstreeRepo                        *self,
                          OstreeRepoCheckoutAtOptions       *options,
                          CheckoutState                     *state,
                          int                                destination_parent_fd,
                          const char                        *destination_name,
                          const char                        *dirtree_checksum,
                          const char                        *dirmeta_checksum,
                          GCancellable                      *cancellable,
                          GError                           **error)
{
  gboolean did_exist = FALSE;
  const gboolean sepolicy_enabled = options->sepolicy && !self->disable_xattrs;
  g_autoptr(GVariant) dirtree = NULL;
  g_autoptr(GVariant) dirmeta = NULL;
  g_autoptr(GVariant) xattrs = NULL;
  g_autoptr(GVariant) modified_xattrs = NULL;

  if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_DIR_TREE,
                                 dirtree_checksum, &dirtree, error))
    return FALSE;
  if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_DIR_META,
                                 dirmeta_checksum, &dirmeta, error))
    return FALSE;

  /* Parse OSTREE_OBJECT_TYPE_DIR_META */
  guint32 uid, gid, mode;
  g_variant_get (dirmeta, "(uuu@a(ayay))",
                 &uid, &gid, &mode,
                 options->mode != OSTREE_REPO_CHECKOUT_MODE_USER ? &xattrs : NULL);
  uid = GUINT32_FROM_BE (uid);
  gid = GUINT32_FROM_BE (gid);
  mode = GUINT32_FROM_BE (mode);

  /* First, make the directory.  Push a new scope in case we end up using
   * setfscreatecon().
   */
  {
    g_auto(OstreeSepolicyFsCreatecon) fscreatecon = { 0, };

    /* If we're doing SELinux labeling, prepare it */
    if (sepolicy_enabled)
      {
        /* We'll set the xattr via setfscreatecon(), so don't do it via generic xattrs below. */
        modified_xattrs = _ostree_filter_selinux_xattr (xattrs);
        xattrs = modified_xattrs;

        if (!_ostree_sepolicy_preparefscreatecon (&fscreatecon, options->sepolicy,
                                                  state->selabel_path_buf->str,
                                                  mode, error))
          return FALSE;
      }

    /* Create initially with mode 0700, then chown/chmod only when we're
     * done.  This avoids anyone else being able to operate on partially
     * constructed dirs.
     */
    if (TEMP_FAILURE_RETRY (mkdirat (destination_parent_fd, destination_name, 0700)) < 0)
      {
        if (errno != EEXIST)
          return glnx_throw_errno_prefix (error, "mkdirat");

        switch (options->overwrite_mode)
          {
          case OSTREE_REPO_CHECKOUT_OVERWRITE_NONE:
            return glnx_throw_errno_prefix (error, "mkdirat");
          /* All of these cases are the same for directories */
          case OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES:
          case OSTREE_REPO_CHECKOUT_OVERWRITE_ADD_FILES:
          case OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL:
            did_exist = TRUE;
            break;
          }
      }
  }

  glnx_autofd int destination_dfd = -1;
  if (!glnx_opendirat (destination_parent_fd, destination_name, TRUE,
                       &destination_dfd, error))
    return FALSE;

  struct stat repo_dfd_stat;
  if (fstat (self->repo_dir_fd, &repo_dfd_stat) < 0)
    return glnx_throw_errno (error);
  struct stat destination_stat;
  if (fstat (destination_dfd, &destination_stat) < 0)
    return glnx_throw_errno (error);

  if (options->no_copy_fallback && repo_dfd_stat.st_dev != destination_stat.st_dev)
    return glnx_throw (error, "Unable to do hardlink checkout across devices (src=%"G_GUINT64_FORMAT" destination=%"G_GUINT64_FORMAT")",
                       (guint64)repo_dfd_stat.st_dev, (guint64)destination_stat.st_dev);

  /* Set the xattrs if we created the dir */
  if (!did_exist && xattrs)
    {
      if (!glnx_fd_set_all_xattrs (destination_dfd, xattrs, cancellable, error))
        return FALSE;
    }

  GString *selabel_path_buf = state->selabel_path_buf;
  /* Process files in this subdir */
  { g_autoptr(GVariant) dir_file_contents = g_variant_get_child_value (dirtree, 0);
    GVariantIter viter;
    g_variant_iter_init (&viter, dir_file_contents);
    const char *fname;
    g_autoptr(GVariant) contents_csum_v = NULL;
    while (g_variant_iter_loop (&viter, "(&s@ay)", &fname, &contents_csum_v))
      {
        const size_t origlen = selabel_path_buf ? selabel_path_buf->len : 0;
        if (selabel_path_buf)
          g_string_append (selabel_path_buf, fname);

        char tmp_checksum[OSTREE_SHA256_STRING_LEN+1];
        _ostree_checksum_inplace_from_bytes_v (contents_csum_v, tmp_checksum);

        if (!checkout_one_file_at (self, options, state,
                                   tmp_checksum,
                                   destination_dfd, fname,
                                   cancellable, error))
          return FALSE;

        if (selabel_path_buf)
          g_string_truncate (selabel_path_buf, origlen);
      }
    contents_csum_v = NULL; /* iter_loop freed it */
  }

  /* Process subdirectories */
  { g_autoptr(GVariant) dir_subdirs = g_variant_get_child_value (dirtree, 1);
    const char *dname;
    g_autoptr(GVariant) subdirtree_csum_v = NULL;
    g_autoptr(GVariant) subdirmeta_csum_v = NULL;
    GVariantIter viter;
    g_variant_iter_init (&viter, dir_subdirs);
    while (g_variant_iter_loop (&viter, "(&s@ay@ay)", &dname,
                                &subdirtree_csum_v, &subdirmeta_csum_v))
      {
        /* Validate this up front to prevent path traversal attacks. Note that
         * we don't validate at the top of this function like we do for
         * checkout_one_file_at() becuase I believe in some cases this function
         * can be called *initially* with user-specified paths for the root
         * directory.
         */
        if (!ot_util_filename_validate (dname, error))
          return FALSE;

        const size_t origlen = selabel_path_buf ? selabel_path_buf->len : 0;
        if (selabel_path_buf)
          {
            g_string_append (selabel_path_buf, dname);
            g_string_append_c (selabel_path_buf, '/');
          }

        char subdirtree_checksum[OSTREE_SHA256_STRING_LEN+1];
        _ostree_checksum_inplace_from_bytes_v (subdirtree_csum_v, subdirtree_checksum);
        char subdirmeta_checksum[OSTREE_SHA256_STRING_LEN+1];
        _ostree_checksum_inplace_from_bytes_v (subdirmeta_csum_v, subdirmeta_checksum);
        if (!checkout_tree_at_recurse (self, options, state,
                                       destination_dfd, dname,
                                       subdirtree_checksum, subdirmeta_checksum,
                                       cancellable, error))
          return FALSE;

        if (selabel_path_buf)
          g_string_truncate (selabel_path_buf, origlen);
      }
  }

  /* We do fchmod/fchown last so that no one else could access the
   * partially created directory and change content we're laying out.
   */
  if (!did_exist)
    {
      guint32 canonical_mode;
      /* Silently ignore world-writable directories (plus sticky, suid bits,
       * etc.) when doing a checkout for bare-user-only repos, or if requested explicitly.
       * This is related to the logic in ostree-repo-commit.c for files.
       * See also: https://github.com/ostreedev/ostree/pull/909 i.e. 0c4b3a2b6da950fd78e63f9afec602f6188f1ab0
       */
      if (self->mode == OSTREE_REPO_MODE_BARE_USER_ONLY || options->bareuseronly_dirs)
        canonical_mode = (mode & 0775) | S_IFDIR;
      else
        canonical_mode = mode;
      if (TEMP_FAILURE_RETRY (fchmod (destination_dfd, canonical_mode)) < 0)
        return glnx_throw_errno_prefix (error, "fchmod");
    }

  if (!did_exist && options->mode != OSTREE_REPO_CHECKOUT_MODE_USER)
    {
      if (TEMP_FAILURE_RETRY (fchown (destination_dfd, uid, gid)) < 0)
        return glnx_throw_errno (error);
    }

  /* Set directory mtime to OSTREE_TIMESTAMP, so that it is constant for all checkouts.
   * Must be done after setting permissions and creating all children.  Note we skip doing
   * this for directories that already exist (under the theory we possibly don't own them),
   * and we also skip it if doing copying checkouts, which is mostly for /etc.
   */
  if (!did_exist && !options->force_copy)
    {
      const struct timespec times[2] = { { OSTREE_TIMESTAMP, UTIME_OMIT }, { OSTREE_TIMESTAMP, 0} };
      if (TEMP_FAILURE_RETRY (futimens (destination_dfd, times)) < 0)
        return glnx_throw_errno (error);
    }

  if (fsync_is_enabled (self, options))
    {
      if (fsync (destination_dfd) == -1)
        return glnx_throw_errno (error);
    }

  return TRUE;
}

/* Begin a checkout process */
static gboolean
checkout_tree_at (OstreeRepo                        *self,
                  OstreeRepoCheckoutAtOptions       *options,
                  int                                destination_parent_fd,
                  const char                        *destination_name,
                  OstreeRepoFile                    *source,
                  GFileInfo                         *source_info,
                  GCancellable                      *cancellable,
                  GError                           **error)
{
  g_auto(CheckoutState) state = { 0, };
  // If SELinux labeling is enabled, we need to keep track of the full path string
  if (options->sepolicy)
    {
      GString *buf = g_string_new (options->sepolicy_prefix ?: options->subpath);
      g_assert_cmpint (buf->len, >, 0);
      // Ensure it ends with /
      if (buf->str[buf->len-1] != '/')
        g_string_append_c (buf, '/');
      state.selabel_path_buf = buf;

      /* Otherwise it'd just be corrupting things, and there's no use case */
      g_assert (options->force_copy);
    }

  /* Special case handling for subpath of a non-directory */
  if (g_file_info_get_file_type (source_info) != G_FILE_TYPE_DIRECTORY)
    {
      /* For backwards compat reasons, we do a mkdir() here. However, as a
       * special case to allow callers to directly check out files without an
       * intermediate root directory, we will skip mkdirat() if
       * `destination_name` == `.`, since obviously the current directory
       * exists.
       */
      int destination_dfd = destination_parent_fd;
      glnx_autofd int destination_dfd_owned = -1;
      if (strcmp (destination_name, ".") != 0)
        {
          if (mkdirat (destination_parent_fd, destination_name, 0700) < 0
              && errno != EEXIST)
            return glnx_throw_errno_prefix (error, "mkdirat");
          if (!glnx_opendirat (destination_parent_fd, destination_name, TRUE,
                               &destination_dfd_owned, error))
            return FALSE;
          destination_dfd = destination_dfd_owned;
        }
      return checkout_one_file_at (self, options, &state,
                                   ostree_repo_file_get_checksum (source),
                                   destination_dfd,
                                   g_file_info_get_name (source_info),
                                   cancellable, error);
    }

  /* Cache any directory metadata we read during this operation;
   * see commit b7afe91e21143d7abb0adde440683a52712aa246
   */
  g_auto(OstreeRepoMemoryCacheRef) memcache_ref;
  _ostree_repo_memory_cache_ref_init (&memcache_ref, self);

  g_assert_cmpint (g_file_info_get_file_type (source_info), ==, G_FILE_TYPE_DIRECTORY);
  const char *dirtree_checksum = ostree_repo_file_tree_get_contents_checksum (source);
  const char *dirmeta_checksum = ostree_repo_file_tree_get_metadata_checksum (source);
  return checkout_tree_at_recurse (self, options, &state, destination_parent_fd,
                                   destination_name,
                                   dirtree_checksum, dirmeta_checksum,
                                   cancellable, error);
}

static void
canonicalize_options (OstreeRepo                  *self,
                      OstreeRepoCheckoutAtOptions *options)
{
  /* Canonicalize subpath to / */
  if (!options->subpath)
    options->subpath = "/";

  /* Force USER mode for BARE_USER_ONLY always - nothing else makes sense */
  if (ostree_repo_get_mode (self) == OSTREE_REPO_MODE_BARE_USER_ONLY)
    options->mode = OSTREE_REPO_CHECKOUT_MODE_USER;
}

/**
 * ostree_repo_checkout_tree:
 * @self: Repo
 * @mode: Options controlling all files
 * @overwrite_mode: Whether or not to overwrite files
 * @destination: Place tree here
 * @source: Source tree
 * @source_info: Source info
 * @cancellable: Cancellable
 * @error: Error
 *
 * Check out @source into @destination, which must live on the
 * physical filesystem.  @source may be any subdirectory of a given
 * commit.  The @mode and @overwrite_mode allow control over how the
 * files are checked out.
 */
gboolean
ostree_repo_checkout_tree (OstreeRepo               *self,
                           OstreeRepoCheckoutMode    mode,
                           OstreeRepoCheckoutOverwriteMode    overwrite_mode,
                           GFile                    *destination,
                           OstreeRepoFile           *source,
                           GFileInfo                *source_info,
                           GCancellable             *cancellable,
                           GError                  **error)
{
  OstreeRepoCheckoutAtOptions options = { 0, };
  options.mode = mode;
  options.overwrite_mode = overwrite_mode;
  /* Backwards compatibility */
  options.enable_uncompressed_cache = TRUE;
  canonicalize_options (self, &options);

  return checkout_tree_at (self, &options,
                           AT_FDCWD, gs_file_get_path_cached (destination),
                           source, source_info,
                           cancellable, error);
}

/**
 * ostree_repo_checkout_tree_at: (skip)
 * @self: Repo
 * @options: (allow-none): Options
 * @destination_dfd: Directory FD for destination
 * @destination_path: Directory for destination
 * @commit: Checksum for commit
 * @cancellable: Cancellable
 * @error: Error
 *
 * Similar to ostree_repo_checkout_tree(), but uses directory-relative
 * paths for the destination, uses a new `OstreeRepoCheckoutAtOptions`,
 * and takes a commit checksum and optional subpath pair, rather than
 * requiring use of `GFile` APIs for the caller.
 *
 * Note in addition that unlike ostree_repo_checkout_tree(), the
 * default is not to use the repository-internal uncompressed objects
 * cache.
 *
 * This function is deprecated.  Use ostree_repo_checkout_at() instead.
 */
gboolean
ostree_repo_checkout_tree_at (OstreeRepo                        *self,
                              OstreeRepoCheckoutOptions         *options,
                              int                                destination_dfd,
                              const char                        *destination_path,
                              const char                        *commit,
                              GCancellable                      *cancellable,
                              GError                           **error)
{
  OstreeRepoCheckoutAtOptions new_opts = {0, };
  new_opts.mode = options->mode;
  new_opts.overwrite_mode = options->overwrite_mode;
  new_opts.enable_uncompressed_cache = options->enable_uncompressed_cache;
  new_opts.enable_fsync = options->disable_fsync ? FALSE : self->disable_fsync;
  new_opts.process_whiteouts = options->process_whiteouts;
  new_opts.no_copy_fallback = options->no_copy_fallback;
  new_opts.subpath = options->subpath;
  new_opts.devino_to_csum_cache = options->devino_to_csum_cache;
  return ostree_repo_checkout_at (self, &new_opts, destination_dfd,
                                  destination_path, commit, cancellable, error);
}

/**
 * ostree_repo_checkout_at:
 * @self: Repo
 * @options: (allow-none): Options
 * @destination_dfd: Directory FD for destination
 * @destination_path: Directory for destination
 * @commit: Checksum for commit
 * @cancellable: Cancellable
 * @error: Error
 *
 * Similar to ostree_repo_checkout_tree(), but uses directory-relative
 * paths for the destination, uses a new `OstreeRepoCheckoutAtOptions`,
 * and takes a commit checksum and optional subpath pair, rather than
 * requiring use of `GFile` APIs for the caller.
 *
 * It also replaces ostree_repo_checkout_at() which was not safe to
 * use with GObject introspection.
 *
 * Note in addition that unlike ostree_repo_checkout_tree(), the
 * default is not to use the repository-internal uncompressed objects
 * cache.
 */
gboolean
ostree_repo_checkout_at (OstreeRepo                        *self,
                         OstreeRepoCheckoutAtOptions       *options,
                         int                                destination_dfd,
                         const char                        *destination_path,
                         const char                        *commit,
                         GCancellable                      *cancellable,
                         GError                           **error)
{
  OstreeRepoCheckoutAtOptions default_options = { 0, };
  OstreeRepoCheckoutAtOptions real_options;

  if (!options)
    {
      default_options.subpath = NULL;
      options = &default_options;
    }

  /* Make a copy so we can modify the options */
  real_options = *options;
  options = &real_options;
  canonicalize_options (self, options);

  g_return_val_if_fail (!(options->force_copy && options->no_copy_fallback), FALSE);
  g_return_val_if_fail (!options->sepolicy || options->force_copy, FALSE);
  /* union identical requires hardlink mode */
  g_return_val_if_fail (!(options->overwrite_mode == OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL &&
                          !options->no_copy_fallback), FALSE);

  g_autoptr(GFile) commit_root = (GFile*) _ostree_repo_file_new_for_commit (self, commit, error);
  if (!commit_root)
    return FALSE;

  if (!ostree_repo_file_ensure_resolved ((OstreeRepoFile*)commit_root, error))
    return FALSE;

  g_autoptr(GFile) target_dir = NULL;

  if (strcmp (options->subpath, "/") != 0)
    target_dir = g_file_get_child (commit_root, options->subpath);
  else
    target_dir = g_object_ref (commit_root);
  g_autoptr(GFileInfo) target_info =
    g_file_query_info (target_dir, OSTREE_GIO_FAST_QUERYINFO,
                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                       cancellable, error);
  if (!target_info)
    return FALSE;

  if (!checkout_tree_at (self, options,
                         destination_dfd,
                         destination_path,
                         (OstreeRepoFile*)target_dir, target_info,
                         cancellable, error))
    return FALSE;

  return TRUE;
}

/**
 * ostree_repo_checkout_at_options_set_devino:
 * @opts: Checkout options
 * @cache: (transfer none) (nullable): Devino cache
 *
 * This function simply assigns @cache to the `devino_to_csum_cache` member of
 * @opts; it's only useful for introspection.
 *
 * Note that cache does *not* have its refcount incremented - the lifetime of
 * @cache must be equal to or greater than that of @opts.
 */
void
ostree_repo_checkout_at_options_set_devino (OstreeRepoCheckoutAtOptions *opts,
                                            OstreeRepoDevInoCache *cache)
{
  opts->devino_to_csum_cache = cache;
}

static guint
devino_hash (gconstpointer a)
{
  OstreeDevIno *a_i = (gpointer)a;
  return (guint) (a_i->dev + a_i->ino);
}

static int
devino_equal (gconstpointer   a,
              gconstpointer   b)
{
  OstreeDevIno *a_i = (gpointer)a;
  OstreeDevIno *b_i = (gpointer)b;
  return a_i->dev == b_i->dev
    && a_i->ino == b_i->ino;
}

/**
 * ostree_repo_devino_cache_new:
 * 
 * OSTree has support for pairing ostree_repo_checkout_tree_at() using
 * hardlinks in combination with a later
 * ostree_repo_write_directory_to_mtree() using a (normally modified)
 * directory.  In order for OSTree to optimally detect just the new
 * files, use this function and fill in the `devino_to_csum_cache`
 * member of `OstreeRepoCheckoutAtOptions`, then call
 * ostree_repo_commit_set_devino_cache().
 *
 * Returns: (transfer full): Newly allocated cache
 */
OstreeRepoDevInoCache *
ostree_repo_devino_cache_new (void)
{
  return (OstreeRepoDevInoCache*) g_hash_table_new_full (devino_hash, devino_equal, g_free, NULL);
}

/**
 * ostree_repo_checkout_gc:
 * @self: Repo
 * @cancellable: Cancellable
 * @error: Error
 *
 * Call this after finishing a succession of checkout operations; it
 * will delete any currently-unused uncompressed objects from the
 * cache.
 */
gboolean
ostree_repo_checkout_gc (OstreeRepo        *self,
                         GCancellable      *cancellable,
                         GError           **error)
{
  g_autoptr(GHashTable) to_clean_dirs = NULL;

  g_mutex_lock (&self->cache_lock);
  to_clean_dirs = self->updated_uncompressed_dirs;
  self->updated_uncompressed_dirs = g_hash_table_new (NULL, NULL);
  g_mutex_unlock (&self->cache_lock);

  if (!to_clean_dirs)
    return TRUE; /* Note early return */

  GLNX_HASH_TABLE_FOREACH (to_clean_dirs, gpointer, prefix)
    {
      g_autofree char *objdir_name = g_strdup_printf ("%02x", GPOINTER_TO_UINT (prefix));
      g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

      if (!glnx_dirfd_iterator_init_at (self->uncompressed_objects_dir_fd, objdir_name, FALSE,
                                        &dfd_iter, error))
        return FALSE;

      while (TRUE)
        {
          struct dirent *dent;
          struct stat stbuf;

          if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
            return FALSE;
          if (dent == NULL)
            break;

          if (fstatat (dfd_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) != 0)
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }

          if (stbuf.st_nlink == 1)
            {
              if (!glnx_unlinkat (dfd_iter.fd, dent->d_name, 0, error))
                return FALSE;
            }
        }
    }

  return TRUE;
}
