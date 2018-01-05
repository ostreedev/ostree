/*
 * Copyright (C) 2015,2016 Colin Walters <walters@verbum.org>
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

#define FUSE_USE_VERSION 26

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/xattr.h>
#include <dirent.h>
#include <unistd.h>
#include <fuse.h>

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
callback_getattr (const char *path, struct stat *st_data)
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
callback_readdir (const char *path, void *buf, fuse_fill_dir_t filler,
                  off_t offset, struct fuse_file_info *fi)
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
      if (filler (buf, de->d_name, &st, 0))
        break;
    }

  (void) closedir (dp);
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
      fprintf (stderr, "Failed to find newly created symlink '%s': %s\n",
               to, g_strerror (errno));
      exit (EXIT_FAILURE);
    }
  return 0;
}

static int
callback_rename (const char *from, const char *to)
{
  from = ENSURE_RELPATH (from);
  to = ENSURE_RELPATH (to);
  if (renameat (basefd, from, basefd, to) == -1)
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
can_write_stbuf (const struct stat *stbuf)
{
  /* If it's not a regular file or symlink, ostree won't hardlink it, so allow
   * writes - it might be a FIFO or device that somehow
   * ended up underneath our mount.
   */
  if (!(S_ISREG (stbuf->st_mode) || S_ISLNK (stbuf->st_mode)))
    return TRUE;
  /* If the object isn't hardlinked, it's OK to write */
  if (stbuf->st_nlink <= 1)
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

static int
verify_write_or_copyup (const char *path, const struct stat *stbuf,
                        gboolean *out_did_copyup)
{
  struct stat stbuf_local;

  if (out_did_copyup)
    *out_did_copyup = FALSE;

  /* If a stbuf wasn't provided, gather it now */
  if (!stbuf)
    {
      if (fstatat (basefd, path, &stbuf_local, AT_SYMLINK_NOFOLLOW) == -1)
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
          g_autoptr(GError) tmp_error = NULL;
          if (!ostree_break_hardlink (basefd, path, FALSE, NULL, &tmp_error))
            return -gioerror_to_errno ((GIOErrorEnum)tmp_error->code);
          if (out_did_copyup)
            *out_did_copyup = TRUE;
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
#define PATH_WRITE_ENTRYPOINT(path) do {                     \
    path = ENSURE_RELPATH (path);                            \
    int r = verify_write_or_copyup (path, NULL, NULL);       \
    if (r != 0)                                              \
      return r;                                              \
  } while (0)

static int
callback_chmod (const char *path, mode_t mode)
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
callback_chown (const char *path, uid_t uid, gid_t gid)
{
  PATH_WRITE_ENTRYPOINT (path);

  if (fchownat (basefd, path, uid, gid, AT_SYMLINK_NOFOLLOW) != 0)
    return -errno;
  return 0;
}

static int
callback_truncate (const char *path, off_t size)
{
  PATH_WRITE_ENTRYPOINT (path);

  glnx_autofd int fd = openat (basefd, path, O_NOFOLLOW|O_WRONLY);
  if (fd == -1)
    return -errno;

  if (ftruncate (fd, size) == -1)
    return -errno;

  return 0;
}

static int
callback_utimens (const char *path, const struct timespec tv[2])
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
  struct stat stbuf;

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

      /* We need to specially handle O_TRUNC */
      fd = openat (basefd, path, finfo->flags & ~O_TRUNC, mode);
      if (fd == -1)
        return -errno;

      if (fstat (fd, &stbuf) == -1)
        {
          (void) close (fd);
          return -errno;
        }

      gboolean did_copyup;
      int r = verify_write_or_copyup (path, &stbuf, &did_copyup);
      if (r != 0)
        {
          (void) close (fd);
          return r;
        }

      /* In the copyup case, we need to re-open */
      if (did_copyup)
        {
          (void) close (fd);
          /* Note that unlike the initial open, we will pass through
           * O_TRUNC.  More ideally in this copyup case we'd avoid copying
           * the whole file in the first place, but eh.  It's not like we're
           * high performance anyways.
           */
          fd = openat (basefd, path, finfo->flags & ~(O_EXCL|O_CREAT), mode);
          if (fd == -1)
            return -errno;
        }
      else
        {
          /* In the non-copyup case we handle O_TRUNC here, after we've verified
           * the hardlink state above with verify_write_or_copyup().
           */
          if (finfo->flags & O_TRUNC)
            {
              if (ftruncate (fd, 0) == -1)
                {
                  (void) close (fd);
                  return -errno;
                }
            }
        }
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
callback_create(const char *path, mode_t mode, struct fuse_file_info *finfo)
{
  return do_open (path, mode, finfo);
}

static int
callback_read_buf (const char *path, struct fuse_bufvec **bufp,
                   size_t size, off_t offset, struct fuse_file_info *finfo)
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
callback_read (const char *path, char *buf, size_t size, off_t offset,
               struct fuse_file_info *finfo)
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
  (void) close (finfo->fh);
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
callback_setxattr (const char *path, const char *name, const char *value,
                   size_t size, int flags)
{
  return -ENOTSUP;
}

static int
callback_getxattr (const char *path, const char *name, char *value,
                   size_t size)
{
  return -ENOTSUP;
}

/*
 * List the supported extended attributes.
 */
static int
callback_listxattr (const char *path, char *list, size_t size)
{
  return -ENOTSUP;

}

/*
 * Remove an extended attribute.
 */
static int
callback_removexattr (const char *path, const char *name)
{
  return -ENOTSUP;

}

struct fuse_operations callback_oper = {
  .getattr = callback_getattr,
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
  .removexattr = callback_removexattr
};

enum {
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
           "   Makes basepath visible at mountpoint such that files are read-only, directories are writable\n"
           "\n"
           "general options:\n"
           "   -o opt,[opt...]     mount options\n"
           "   -h  --help          print help\n"
           "\n", progname);
}

static int
rofs_parse_opt (void *data, const char *arg, int key,
                struct fuse_args *outargs)
{
  (void) data;

  switch (key)
    {
    case FUSE_OPT_KEY_NONOPT:
      if (basefd == -1)
        {
          basefd = openat (AT_FDCWD, arg, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
          if (basefd == -1)
            {
              perror ("openat");
              exit (EXIT_FAILURE);
            }
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

static struct fuse_opt rofs_opts[] = {
  FUSE_OPT_KEY ("-h", KEY_HELP),
  FUSE_OPT_KEY ("--help", KEY_HELP),
  FUSE_OPT_KEY ("-V", KEY_VERSION),
  FUSE_OPT_KEY ("--version", KEY_VERSION),
  FUSE_OPT_KEY ("--copyup", KEY_COPYUP),
  FUSE_OPT_END
};

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

  fuse_main (args.argc, args.argv, &callback_oper, NULL);

  return 0;
}
