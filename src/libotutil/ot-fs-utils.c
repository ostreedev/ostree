/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include "ot-fs-utils.h"
#include "libglnx.h"
#include <sys/xattr.h>
#include <sys/mman.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

/* Convert a fd-relative path to a GFile* - use
 * for legacy code.
 */
GFile *
ot_fdrel_to_gfile (int dfd, const char *path)
{
  g_autofree char *abspath = glnx_fdrel_abspath (dfd, path);
  return g_file_new_for_path (abspath);
}

/* Wraps readlinkat(), and sets the `symlink-target` property
 * of @target_info.
 */
gboolean
ot_readlinkat_gfile_info (int             dfd,
                          const char     *path,
                          GFileInfo      *target_info,
                          GCancellable   *cancellable,
                          GError        **error)
{
  char targetbuf[PATH_MAX+1];
  ssize_t len;

  if (TEMP_FAILURE_RETRY (len = readlinkat (dfd, path, targetbuf, sizeof (targetbuf) - 1)) < 0)
    return glnx_throw_errno_prefix (error, "readlinkat");
  targetbuf[len] = '\0';
  g_file_info_set_symlink_target (target_info, targetbuf);

  return TRUE;
}

/**
 * ot_openat_read_stream:
 * @dfd: Directory file descriptor
 * @path: Subpath
 * @follow: Whether or not to follow symbolic links
 * @out_istream: (out): Return location for input stream
 * @cancellable: Cancellable
 * @error: Error
 *
 * Open a file for reading starting from @dfd for @path.
 * The @follow parameter determines whether or not to follow
 * if the last element of @path is a symbolic link.  Intermediate
 * symlink path components are always followed.
 */
gboolean
ot_openat_read_stream (int             dfd,
                       const char     *path,
                       gboolean        follow,
                       GInputStream  **out_istream,
                       GCancellable   *cancellable,
                       GError        **error)
{
  glnx_autofd int fd = -1;
  if (!glnx_openat_rdonly (dfd, path, follow, &fd, error))
    return FALSE;
  *out_istream = g_unix_input_stream_new (glnx_steal_fd (&fd), TRUE);
  return TRUE;
}

/* Like unlinkat() but ignore ENOENT */
gboolean
ot_ensure_unlinked_at (int dfd,
                       const char *path,
                       GError **error)
{
  if (unlinkat (dfd, path, 0) != 0)
    {
      if (G_UNLIKELY (errno != ENOENT))
        return glnx_throw_errno_prefix (error, "unlink(%s)", path);
    }
  return TRUE;
}

gboolean
ot_openat_ignore_enoent (int dfd,
                         const char *path,
                         int *out_fd,
                         GError **error)
{
  int target_fd = openat (dfd, path, O_CLOEXEC | O_RDONLY);
  if (target_fd < 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "openat(%s)", path);
    }

  *out_fd = target_fd;
  return TRUE;
}

/* Like glnx_dirfd_iterator_init_at(), but if %ENOENT, then set
 * @out_exists to %FALSE, and return successfully.
 */
gboolean
ot_dfd_iter_init_allow_noent (int dfd,
                              const char *path,
                              GLnxDirFdIterator *dfd_iter,
                              gboolean *out_exists,
                              GError **error)
{
  glnx_autofd int fd = glnx_opendirat_with_errno (dfd, path, TRUE);
  if (fd < 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "opendirat");
      *out_exists = FALSE;
      return TRUE;
    }
  if (!glnx_dirfd_iterator_init_take_fd (&fd, dfd_iter, error))
    return FALSE;
  *out_exists = TRUE;
  return TRUE;
}

typedef struct {
  gpointer addr;
  gsize len;
} MapData;

static void
map_data_destroy (gpointer data)
{
  MapData *mdata = data;
  (void) munmap (mdata->addr, mdata->len);
  g_free (mdata);
}

/* Return a newly-allocated GBytes that refers to the contents of the file
 * starting at offset @start. If the file is large enough, mmap() may be used.
 */
GBytes *
ot_fd_readall_or_mmap (int           fd,
                       goffset       start,
                       GError      **error)
{
  struct stat stbuf;
  if (!glnx_fstat (fd, &stbuf, error))
    return FALSE;

  /* http://stackoverflow.com/questions/258091/when-should-i-use-mmap-for-file-access */
  if (start > stbuf.st_size)
    return g_bytes_new_static (NULL, 0);
  const gsize len = stbuf.st_size - start;
  if (len > 16*1024)
    {
      /* The reason we don't use g_mapped_file_new_from_fd() here
       * is it doesn't support passing an offset, which is actually
       * used by the static delta code.
       */
      gpointer map = mmap (NULL, len, PROT_READ, MAP_PRIVATE, fd, start);
      if (map == (void*)-1)
        return glnx_null_throw_errno_prefix (error, "mmap");

      MapData *mdata = g_new (MapData, 1);
      mdata->addr = map;
      mdata->len = len;

      return g_bytes_new_with_free_func (map, len, map_data_destroy, mdata);
    }

  /* Fall through to plain read into a malloc buffer */
  if (lseek (fd, start, SEEK_SET) < 0)
    return glnx_null_throw_errno_prefix (error, "lseek");
  /* Not cancellable since this should be small */
  return glnx_fd_readall_bytes (fd, NULL, error);
}

/* Given an input stream, splice it to an anonymous file (O_TMPFILE).
 * Useful for potentially large but transient files.
 */
GBytes *
ot_map_anonymous_tmpfile_from_content (GInputStream *instream,
                                       GCancellable *cancellable,
                                       GError      **error)
{
  g_auto(GLnxTmpfile) tmpf = { 0, };
  if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &tmpf, error))
    return NULL;

  g_autoptr(GOutputStream) out = g_unix_output_stream_new (tmpf.fd, FALSE);
  gssize n_bytes_written = g_output_stream_splice (out, instream,
                                                   G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                                   G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                                   cancellable, error);
  if (n_bytes_written < 0)
    return NULL;

  g_autoptr(GMappedFile) mfile = g_mapped_file_new_from_fd (tmpf.fd, FALSE, error);
  if (!mfile)
    return NULL;
  return g_mapped_file_get_bytes (mfile);
}

gboolean
ot_parse_file_by_line (const char    *path,
                       gboolean     (*cb)(const char*, void*, GError**),
                       void          *cbdata,
                       GCancellable  *cancellable,
                       GError       **error)
{
  g_autofree char *contents =
    glnx_file_get_contents_utf8_at (AT_FDCWD, path, NULL, cancellable, error);
  if (!contents)
    return FALSE;

  g_auto(GStrv) lines = g_strsplit (contents, "\n", -1);
  for (char **iter = lines; iter && *iter; iter++)
    {
      /* skip empty lines at least */
      if (**iter == '\0')
        continue;

      if (!cb (*iter, cbdata, error))
        return FALSE;
    }

  return TRUE;
}
