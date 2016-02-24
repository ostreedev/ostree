/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
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
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/xattr.h>
#include <dirent.h>
#include <unistd.h>
#include <fuse.h>

#include <glib.h>

#include "libglnx.h"

// Global to store our read-write path
static int basefd = -1;
static GHashTable *created_devino_hash = NULL;

static inline const char *
ENSURE_RELPATH (const char *path)
{
  path = path + strspn (path, "/");
  if (*path == 0)
    return ".";
  return path;
}

typedef struct {
  dev_t dev;
  ino_t ino;
} DevIno;

static guint
devino_hash (gconstpointer a)
{
  DevIno *a_i = (gpointer)a;
  return (guint) (a_i->dev + a_i->ino);
}

static int
devino_equal (gconstpointer   a,
              gconstpointer   b)
{
  DevIno *a_i = (gpointer)a;
  DevIno *b_i = (gpointer)b;
  return a_i->dev == b_i->dev
    && a_i->ino == b_i->ino;
}

static gboolean
devino_set_contains (dev_t dev, ino_t ino)
{
  DevIno devino = { dev, ino };
  return g_hash_table_contains (created_devino_hash, &devino);
}

static gboolean
devino_set_insert (dev_t dev, ino_t ino)
{
  DevIno *devino = g_new (DevIno, 1);
  devino->dev = dev;
  devino->ino = ino;
  return g_hash_table_add (created_devino_hash, devino);
}

static gboolean
devino_set_remove (dev_t dev, ino_t ino)
{
  DevIno devino = { dev, ino };
  return g_hash_table_remove (created_devino_hash, &devino);
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
  struct stat stbuf;
  path = ENSURE_RELPATH (path);

  if (fstatat (basefd, path, &stbuf, AT_SYMLINK_NOFOLLOW) == 0)
    {
      if (!S_ISDIR (stbuf.st_mode))
	devino_set_remove (stbuf.st_dev, stbuf.st_ino);
    }

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
      exit (1);
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

static int
can_write (const char *path)
{
  struct stat stbuf;
  if (fstatat (basefd, path, &stbuf, 0) == -1)
    {
      if (errno == ENOENT)
	return 0;
      else
	return -errno;
    }
  if (devino_set_contains (stbuf.st_dev, stbuf.st_ino))
    return -EROFS;
  return 0;
}

#define VERIFY_WRITE(path) do { \
  int r = can_write (path); \
  if (r != 0) \
    return r; \
  } while (0)

static int
callback_chmod (const char *path, mode_t mode)
{
  path = ENSURE_RELPATH (path);
  VERIFY_WRITE(path);
  if (fchmodat (basefd, path, mode, 0) != 0)
    return -errno;
  return 0;
}

static int
callback_chown (const char *path, uid_t uid, gid_t gid)
{
  path = ENSURE_RELPATH (path);
  VERIFY_WRITE(path);
  if (fchownat (basefd, path, uid, gid, 0) != 0)
    return -errno;
  return 0;
}

static int
callback_truncate (const char *path, off_t size)
{
  glnx_fd_close int fd = -1;

  path = ENSURE_RELPATH (path);
  VERIFY_WRITE(path);

  fd = openat (basefd, path, O_WRONLY);
  if (fd == -1)
    return -errno;

  if (ftruncate (fd, size) == -1)
    return -errno;

  return 0;
}

static int
callback_utime (const char *path, struct utimbuf *buf)
{
  struct timespec ts[2];

  path = ENSURE_RELPATH (path);

  ts[0].tv_sec = buf->actime;
  ts[0].tv_nsec = UTIME_OMIT;
  ts[1].tv_sec = buf->modtime;
  ts[1].tv_nsec = UTIME_OMIT;

  if (utimensat (basefd, path, ts, AT_SYMLINK_NOFOLLOW) == -1)
    return -errno;

  return 0;
}

static int
do_open (const char *path, mode_t mode, struct fuse_file_info *finfo)
{
  const int flags = finfo->flags & O_ACCMODE;
  int fd;
  struct stat stbuf;

  /* Support read only opens */
  G_STATIC_ASSERT (O_RDONLY == 0);

  path = ENSURE_RELPATH (path);

  if (flags == 0)
    fd = openat (basefd, path, flags);
  else
    {
      const int forced_excl_flags = flags | O_CREAT | O_EXCL;
      /* Do an exclusive open, don't allow writable fds for existing
	 files */
      fd = openat (basefd, path, forced_excl_flags, mode);
      /* If they didn't specify O_EXCL, give them EROFS if the file
       * exists.
       */
      if (fd == -1 && (flags & O_EXCL) == 0)
	{
	  if (errno == EEXIST)
	    errno = EROFS;
	}
      else if (fd != -1)
	{
	  if (fstat (fd, &stbuf) == -1)
	    return -errno;
	  devino_set_insert (stbuf.st_dev, stbuf.st_ino);
	}
    }

  if (fd == -1)
    return -errno;

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
  if (faccessat (basefd, path, mode, 0) == -1)
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
  .utime = callback_utime,
  .create = callback_create,
  .open = callback_open,
  .read = callback_read,
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

enum
{
  KEY_HELP,
  KEY_VERSION,
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
	      exit (1);
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
      exit (0);
    default:
      fprintf (stderr, "see `%s -h' for usage\n", outargs->argv[0]);
      exit (1);
    }
  return 1;
}

static struct fuse_opt rofs_opts[] = {
  FUSE_OPT_KEY ("-h", KEY_HELP),
  FUSE_OPT_KEY ("--help", KEY_HELP),
  FUSE_OPT_KEY ("-V", KEY_VERSION),
  FUSE_OPT_KEY ("--version", KEY_VERSION),
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
      exit (1);
    }
  if (basefd == -1)
    {
      fprintf (stderr, "Missing basepath\n");
      fprintf (stderr, "see `%s -h' for usage\n", argv[0]);
      exit (1);
    }

  created_devino_hash = g_hash_table_new_full (devino_hash, devino_equal, g_free, NULL); 

  fuse_main (args.argc, args.argv, &callback_oper, NULL);

  return 0;
}
