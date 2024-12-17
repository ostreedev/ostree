/*
 * Copyright (C) 2015,2016 Colin Walters <walters@verbum.org>
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

#ifndef FUSE_USE_VERSION
#error config.h needs to define FUSE_USE_VERSION
#endif

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <glib.h>

#include "libglnx.h"
#include "ostree.h"

// Global to store our read-write path
static int basefd = -1;
/* Whether or not to automatically "copyup" (in overlayfs terms).
 * What we're really doing is breaking hardlinks.
 */
static gboolean opt_copyup;

static inline const char *
ENSURE_RELPATH (const char *path)
{
  path = path + strspn (path, "/");
  if (*path == 0)
    return ".";
  return path;
}

static int
#if FUSE_USE_VERSION >= 31
callback_getattr (const char *path, struct stat *st_data, struct fuse_file_info *finfo)
#else
callback_getattr (const char *path, struct stat *st_data)
#endif
{
  path = ENSURE_RELPATH (path);
  if (!*path)
    {
      if (fstat (basefd, st_data) == -1)
        return -errno;
    }
  else
    {
      if (fstatat (basefd, path, st_data, AT_SYMLINK_NOFOLLOW) == -1)
        return -errno;
    }
  return 0;
}

static int
callback_readlink (const char *path, char *buf, size_t size)
{
  int r;

  path = ENSURE_RELPATH (path);

  /* Note FUSE wants the string to be always nul-terminated, even if
   * truncated.
   */
  r = readlinkat (basefd, path, buf, size - 1);
  if (r == -1)
    return -errno;
  buf[r] = '\0';
  return 0;
}

static int
#if FUSE_USE_VERSION >= 31
callback_readdir (const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                  struct fuse_file_info *fi, enum fuse_readdir_flags flags)
#else
callback_readdir (const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                  struct fuse_file_info *fi)
#endif
{
  DIR *dp;
  struct dirent *de;
  int dfd;

  path = ENSURE_RELPATH (path);

  if (!*path)
    {
      dfd = fcntl (basefd, F_DUPFD_CLOEXEC, 3);
      if (dfd < 0)
        return -errno;
      lseek (dfd, 0, SEEK_SET);
    }
  else
    {
      dfd = openat (basefd, path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
      if (dfd == -1)
        return -errno;
    }

  /* Transfers ownership of fd */
  dp = fdopendir (dfd);
  if (dp == NULL)
    return -errno;

  while ((de = readdir (dp)) != NULL)
    {
      struct stat st;
      memset (&st, 0, sizeof (st));
      st.st_ino = de->d_ino;
      st.st_mode = de->d_type << 12;

#if FUSE_USE_VERSION >= 31
      if (filler (buf, de->d_name, &st, 0, 0))
        break;
#else
      if (filler (buf, de->d_name, &st, 0))
        break;
#endif
    }

  (void)closedir (dp);
  return 0;
}

static int
callback_mknod (const char *path, mode_t mode, dev_t rdev)
{
  return -EROFS;
}

static int
callback_mkdir (const char *path, mode_t mode)
{
  path = ENSURE_RELPATH (path);
  if (mkdirat (basefd, path, mode) == -1)
    return -errno;
  return 0;
}

static int
callback_unlink (const char *path)
{
  path = ENSURE_RELPATH (path);
  if (unlinkat (basefd, path, 0) == -1)
    return -errno;
  return 0;
}

static int
callback_rmdir (const char *path)
{
  path = ENSURE_RELPATH (path);
  if (unlinkat (basefd, path, AT_REMOVEDIR) == -1)
    return -errno;
  return 0;
}

static int
callback_symlink (const char *from, const char *to)
{
  struct stat stbuf;

  to = ENSURE_RELPATH (to);

  if (symlinkat (from, basefd, to) == -1)
    return -errno;

  if (fstatat (basefd, to, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
    {
      fprintf (stderr, "Failed to find newly created symlink '%s': %s\n", to, g_strerror (errno));
      exit (EXIT_FAILURE);
    }
  return 0;
}

static int
#if FUSE_USE_VERSION >= 31
callback_rename (const char *from, const char *to, unsigned int flags)
#else
callback_rename (const char *from, const char *to)
#endif
{
#if FUSE_USE_VERSION < 31
  unsigned int flags = 0;
#endif

  from = ENSURE_RELPATH (from);
  to = ENSURE_RELPATH (to);

  /* This assumes Linux 3.15+ */
  if (renameat2 (basefd, from, basefd, to, flags) == -1)
    return -errno;
  return 0;
}

static int
callback_link (const char *from, const char *to)
{
  from = ENSURE_RELPATH (from);
  to = ENSURE_RELPATH (to);
  if (linkat (basefd, from, basefd, to, 0) == -1)
    return -errno;
  return 0;
}

/* Check whether @stbuf refers to a hardlinked regfile or symlink, and if so
 * return -EROFS. Otherwise return 0.
 */
static gboolean
can_write_stbuf (const struct statx *stbuf)
{
  /* If it's not a regular file or symlink, ostree won't hardlink it, so allow
   * writes - it might be a FIFO or device that somehow
   * ended up underneath our mount.
   */
  if (!(S_ISREG (stbuf->stx_mode) || S_ISLNK (stbuf->stx_mode)))
    return TRUE;
#ifdef STATX_ATTR_VERITY
  /* Can't write to fsverity files */
  if (stbuf->stx_attributes & STATX_ATTR_VERITY)
    return FALSE;
#endif
  /* If the object isn't hardlinked, it's OK to write */
  if (stbuf->stx_nlink <= 1)
    return TRUE;
  /* Otherwise, it's a hardlinked file or symlink; it must be
   * immutable.
   */
  return FALSE;
}

static int
gioerror_to_errno (GIOErrorEnum e)
{
  /* It's obviously crappy to have to do this but
   * we also don't want to try to have "raw errno" versions
   * of everything down in ostree_break_hardlink() so...
   * let's just reverse map a few ones I think are going to be common.
   */
  switch (e)
    {
    case G_IO_ERROR_NOT_FOUND:
      return ENOENT;
    case G_IO_ERROR_IS_DIRECTORY:
      return EISDIR;
    case G_IO_ERROR_PERMISSION_DENIED:
      return EPERM;
    case G_IO_ERROR_NO_SPACE:
      return ENOSPC;
    default:
      return EIO;
    }
}

// The libglnx APIs take a stat buffer, so we need to be able to
// convert from statx.
static inline void
statx_to_stat (const struct statx *stxbuf, struct stat *stbuf)
{
  stbuf->st_dev = makedev (stxbuf->stx_dev_major, stxbuf->stx_dev_minor);
  stbuf->st_rdev = makedev (stxbuf->stx_rdev_major, stxbuf->stx_rdev_minor);
  stbuf->st_ino = stxbuf->stx_ino;
  stbuf->st_mode = stxbuf->stx_mode;
  stbuf->st_nlink = stxbuf->stx_nlink;
  stbuf->st_uid = stxbuf->stx_uid;
  stbuf->st_gid = stxbuf->stx_gid;
  stbuf->st_size = stxbuf->stx_size;
  stbuf->st_blksize = stxbuf->stx_blksize;
}

// A copy of ostree_break_hardlink but without the check for hardlinks, which
// is mainly relevant for regular files, where we need to handle verity.
static gboolean
copyup (int dfd, const char *path, const struct statx *stxbuf, GError **error)
{
  if (S_ISREG (stxbuf->stx_mode))
    {
      struct stat stbuf;
      statx_to_stat (stxbuf, &stbuf);
      // Note GLNX_FILE_COPY_OVERWRITE always uses O_TMPFILE+rename
      return glnx_file_copy_at (dfd, path, &stbuf, dfd, path, GLNX_FILE_COPY_OVERWRITE, NULL,
                                error);
    }
  else
    {
      // For symlinks, we can just directly call the ostree API.  This avoids
      // more code duplication because atomically copying symlinks requires
      // a temp-link dance.
      return ostree_break_hardlink (dfd, path, FALSE, NULL, error);
    }
}

static int
verify_write_or_copyup (const char *path, const struct statx *stbuf)
{
  struct statx stbuf_local;

  /* If a stbuf wasn't provided, gather it now */
  if (!stbuf)
    {
      if (statx (basefd, path, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, STATX_BASIC_STATS,
                 &stbuf_local)
          < 0)
        {
          if (errno == ENOENT)
            return 0;
          else
            return -errno;
        }
      stbuf = &stbuf_local;
    }

  /* Verify writability, if that fails, perform copy-up if enabled */
  if (!can_write_stbuf (stbuf))
    {
      if (opt_copyup)
        {
          g_autoptr (GError) tmp_error = NULL;
          if (!copyup (basefd, path, stbuf, &tmp_error))
            return -gioerror_to_errno ((GIOErrorEnum)tmp_error->code);
        }
      else
        return -EROFS;
    }

  return 0;
}

/* Given a path (which is absolute), convert it
 * to a relative path (even for the caller) and
 * perform either write verification or copy-up.
 */
#define PATH_WRITE_ENTRYPOINT(path) \
  do \
    { \
      path = ENSURE_RELPATH (path); \
      int r = verify_write_or_copyup (path, NULL); \
      if (r != 0) \
        return r; \
    } \
  while (0)

static int
#if FUSE_USE_VERSION >= 31
callback_chmod (const char *path, mode_t mode, struct fuse_file_info *finfo)
#else
callback_chmod (const char *path, mode_t mode)
#endif
{
  PATH_WRITE_ENTRYPOINT (path);

  /* Note we can't use AT_SYMLINK_NOFOLLOW yet;
   * https://marc.info/?l=linux-kernel&m=148830147803162&w=2
   * https://marc.info/?l=linux-fsdevel&m=149193779929561&w=2
   */
  if (fchmodat (basefd, path, mode, 0) != 0)
    return -errno;
  return 0;
}

static int
#if FUSE_USE_VERSION >= 31
callback_chown (const char *path, uid_t uid, gid_t gid, struct fuse_file_info *finfo)
#else
callback_chown (const char *path, uid_t uid, gid_t gid)
#endif
{
  PATH_WRITE_ENTRYPOINT (path);

  if (fchownat (basefd, path, uid, gid, AT_SYMLINK_NOFOLLOW) != 0)
    return -errno;
  return 0;
}

static int
#if FUSE_USE_VERSION >= 31
callback_truncate (const char *path, off_t size, struct fuse_file_info *finfo)
#else
callback_truncate (const char *path, off_t size)
#endif
{
  PATH_WRITE_ENTRYPOINT (path);

  glnx_autofd int fd = openat (basefd, path, O_NOFOLLOW | O_WRONLY);
  if (fd == -1)
    return -errno;

  if (ftruncate (fd, size) == -1)
    return -errno;

  return 0;
}

static int
#if FUSE_USE_VERSION >= 31
callback_utimens (const char *path, const struct timespec tv[2], struct fuse_file_info *finfo)
#else
callback_utimens (const char *path, const struct timespec tv[2])
#endif
{
  /* This one isn't write-verified, we support changing times
   * even for hardlinked files.
   */
  path = ENSURE_RELPATH (path);

  if (utimensat (basefd, path, tv, AT_SYMLINK_NOFOLLOW) == -1)
    return -errno;

  return 0;
}

static int
do_open (const char *path, mode_t mode, struct fuse_file_info *finfo)
{
  int fd;
  struct statx stbuf;

  path = ENSURE_RELPATH (path);

  if ((finfo->flags & O_ACCMODE) == O_RDONLY)
    {
      /* Read */
      fd = openat (basefd, path, finfo->flags, mode);
      if (fd == -1)
        return -errno;
    }
  else
    {
      /* Write */
      if (statx (basefd, path, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, STATX_BASIC_STATS, &stbuf)
          == -1)
        {
          if (errno != ENOENT)
            return -errno;
        }
      else
        {
          int r = verify_write_or_copyup (path, &stbuf);
          if (r != 0)
            return r;
        }

      fd = openat (basefd, path, finfo->flags, mode);
      if (fd == -1)
        return -errno;
    }

  finfo->fh = fd;

  return 0;
}

static int
callback_open (const char *path, struct fuse_file_info *finfo)
{
  return do_open (path, 0, finfo);
}

static int
callback_create (const char *path, mode_t mode, struct fuse_file_info *finfo)
{
  return do_open (path, mode, finfo);
}

static int
callback_read_buf (const char *path, struct fuse_bufvec **bufp, size_t size, off_t offset,
                   struct fuse_file_info *finfo)
{
  struct fuse_bufvec *src;

  src = malloc (sizeof (struct fuse_bufvec));
  if (src == NULL)
    return -ENOMEM;

  *src = FUSE_BUFVEC_INIT (size);

  src->buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
  src->buf[0].fd = finfo->fh;
  src->buf[0].pos = offset;
  *bufp = src;

  return 0;
}

static int
callback_read (const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *finfo)
{
  int r;
  r = pread (finfo->fh, buf, size, offset);
  if (r == -1)
    return -errno;
  return r;
}

static int
callback_write_buf (const char *path, struct fuse_bufvec *buf, off_t offset,
                    struct fuse_file_info *finfo)
{
  struct fuse_bufvec dst = FUSE_BUFVEC_INIT (fuse_buf_size (buf));

  dst.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
  dst.buf[0].fd = finfo->fh;
  dst.buf[0].pos = offset;

  return fuse_buf_copy (&dst, buf, FUSE_BUF_SPLICE_NONBLOCK);
}

static int
callback_write (const char *path, const char *buf, size_t size, off_t offset,
                struct fuse_file_info *finfo)
{
  int r;
  r = pwrite (finfo->fh, buf, size, offset);
  if (r == -1)
    return -errno;
  return r;
}

static int
callback_statfs (const char *path, struct statvfs *st_buf)
{
  if (fstatvfs (basefd, st_buf) == -1)
    return -errno;
  return 0;
}

static int
callback_release (const char *path, struct fuse_file_info *finfo)
{
  (void)close (finfo->fh);
  return 0;
}

static int
callback_fsync (const char *path, int crap, struct fuse_file_info *finfo)
{
  if (fsync (finfo->fh) == -1)
    return -errno;
  return 0;
}

static int
callback_access (const char *path, int mode)
{
  path = ENSURE_RELPATH (path);

  /* Apparently at least GNU coreutils rm calls `faccessat(W_OK)`
   * before trying to do an unlink.  So...we'll just lie about
   * writable access here.
   */
  if (faccessat (basefd, path, mode, AT_SYMLINK_NOFOLLOW) == -1)
    return -errno;
  return 0;
}

static int
callback_setxattr (const char *path, const char *name, const char *value, size_t size, int flags)
{
  PATH_WRITE_ENTRYPOINT (path);

  char buf[PATH_MAX];
  snprintf (buf, sizeof (buf), "/proc/self/fd/%d/%s", basefd, path);

  if (setxattr (buf, name, value, size, flags) == -1)
    return -errno;
  return 0;
}

static int
callback_getxattr (const char *path, const char *name, char *value, size_t size)
{
  path = ENSURE_RELPATH (path);

  char buf[PATH_MAX];
  snprintf (buf, sizeof (buf), "/proc/self/fd/%d/%s", basefd, path);

  ssize_t n = getxattr (buf, name, value, size);
  if (n == -1)
    return -errno;
  return n;
}

/*
 * List the supported extended attributes.
 */
static int
callback_listxattr (const char *path, char *list, size_t size)
{
  path = ENSURE_RELPATH (path);

  char buf[PATH_MAX];
  snprintf (buf, sizeof (buf), "/proc/self/fd/%d/%s", basefd, path);

  ssize_t n = llistxattr (buf, list, size);
  if (n == -1)
    return -errno;
  return n;
}

/*
 * Remove an extended attribute.
 */
static int
callback_removexattr (const char *path, const char *name)
{
  path = ENSURE_RELPATH (path);

  char buf[PATH_MAX];
  snprintf (buf, sizeof (buf), "/proc/self/fd/%d/%s", basefd, path);

  if (lremovexattr (buf, name) == -1)
    return -errno;
  return 0;
}

struct fuse_operations callback_oper = { .getattr = callback_getattr,
                                         .readlink = callback_readlink,
                                         .readdir = callback_readdir,
                                         .mknod = callback_mknod,
                                         .mkdir = callback_mkdir,
                                         .symlink = callback_symlink,
                                         .unlink = callback_unlink,
                                         .rmdir = callback_rmdir,
                                         .rename = callback_rename,
                                         .link = callback_link,
                                         .chmod = callback_chmod,
                                         .chown = callback_chown,
                                         .truncate = callback_truncate,
                                         .utimens = callback_utimens,
                                         .create = callback_create,
                                         .open = callback_open,
                                         .read_buf = callback_read_buf,
                                         .read = callback_read,
                                         .write_buf = callback_write_buf,
                                         .write = callback_write,
                                         .statfs = callback_statfs,
                                         .release = callback_release,
                                         .fsync = callback_fsync,
                                         .access = callback_access,

                                         /* Extended attributes support for userland interaction */
                                         .setxattr = callback_setxattr,
                                         .getxattr = callback_getxattr,
                                         .listxattr = callback_listxattr,
                                         .removexattr = callback_removexattr };

enum
{
  KEY_HELP,
  KEY_VERSION,
  KEY_COPYUP,
};

static void
usage (const char *progname)
{
  fprintf (stdout,
           "usage: %s basepath mountpoint [options]\n"
           "\n"
           "   Makes basepath visible at mountpoint such that files are read-only, directories "
           "are writable\n"
           "\n"
           "general options:\n"
           "   -o opt,[opt...]     mount options\n"
           "   -h  --help          print help\n"
           "\n",
           progname);
}

static int
rofs_parse_opt (void *data, const char *arg, int key, struct fuse_args *outargs)
{
  (void)data;

  switch (key)
    {
    case FUSE_OPT_KEY_NONOPT:
      if (basefd == -1)
        {
          basefd
              = openat (AT_FDCWD, arg, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
          if (basefd == -1)
            err (1, "opening rootfs %s", arg);
          return 0;
        }
      else
        {
          return 1;
        }
    case FUSE_OPT_KEY_OPT:
      return 1;
    case KEY_HELP:
      usage (outargs->argv[0]);
      exit (EXIT_SUCCESS);
    case KEY_COPYUP:
      opt_copyup = TRUE;
      return 0;
    default:
      fprintf (stderr, "see `%s -h' for usage\n", outargs->argv[0]);
      exit (EXIT_FAILURE);
    }
  return 1;
}

static struct fuse_opt rofs_opts[]
    = { FUSE_OPT_KEY ("-h", KEY_HELP),         FUSE_OPT_KEY ("--help", KEY_HELP),
        FUSE_OPT_KEY ("-V", KEY_VERSION),      FUSE_OPT_KEY ("--version", KEY_VERSION),
        FUSE_OPT_KEY ("--copyup", KEY_COPYUP), FUSE_OPT_END };

int
main (int argc, char *argv[])
{
  struct fuse_args args = FUSE_ARGS_INIT (argc, argv);
  int res;

  res = fuse_opt_parse (&args, &basefd, rofs_opts, rofs_parse_opt);
  if (res != 0)
    {
      fprintf (stderr, "Invalid arguments\n");
      fprintf (stderr, "see `%s -h' for usage\n", argv[0]);
      exit (EXIT_FAILURE);
    }
  if (basefd == -1)
    {
      fprintf (stderr, "Missing basepath\n");
      fprintf (stderr, "see `%s -h' for usage\n", argv[0]);
      exit (EXIT_FAILURE);
    }

  // Refer to https://man.openbsd.org/fuse_main.3
  return (fuse_main (args.argc, args.argv, &callback_oper, NULL));
}
