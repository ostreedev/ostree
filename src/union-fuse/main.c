/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015,2016 Colin Walters <walters@verbum.org>
 * Copyright (C) 2016 Red Hat, Inc.
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
#include <gio/gio.h>
#include <glib/gprintf.h>

#include "libglnx.h"
#include "ot-gio-utils.h"
#include <ostree-repo.h>
#include <ostree-repo-file.h>

static OstreeRepo *repo;

#define WHITEOUT_PREFIX ".wh."

struct
opt_config
{
  char *repo_location;
  char *layers;
  int  whiteouts;
  int  memcache;
};

static struct opt_config config;

struct
layer
{
  GFile *root;
  GHashTable *whiteouts;
};

static GArray *layers;

static int objects_basefd;

static GHashTable *memcache_dir;

/* path is a buffer at least (OSTREE_SHA256_STRING_LEN + 8) bytes.  */
static void
get_ostree_object_path (char *path, const char *checksum)
{
  g_sprintf (path, "%.2s/%s.file", checksum, checksum + 2);
}

/* Access directly the file in the repository. This is much better than dealing with seek on an GInputStream,
   and we support only BARE repositories anyway.  */
static int
open_ostree_object_file (const char *checksum)
{
  char path[OSTREE_SHA256_STRING_LEN + 8];
  get_ostree_object_path (path, checksum);
  return openat (objects_basefd, path, O_RDONLY);
}

static void
convert_file_info_to_stat (GFileInfo *info, struct stat *stat)
{
  int type = g_file_info_get_file_type (info);
  memset (stat, 0, sizeof (*stat));
  switch (type)
    {
    case G_FILE_TYPE_REGULAR:
      stat->st_mode = S_IFREG;
      break;

    case G_FILE_TYPE_SYMBOLIC_LINK:
      stat->st_mode = S_IFLNK;
      break;

    case G_FILE_TYPE_DIRECTORY:
      stat->st_mode = S_IFDIR;
      break;
      /* OSTree does not store other types.  */
    }

  stat->st_dev = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_DEVICE);
  stat->st_ino = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_INODE);
  stat->st_mode = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE);
  stat->st_nlink = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_NLINK);
  stat->st_uid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID);
  stat->st_gid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID);
  stat->st_rdev = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_RDEV);
  stat->st_mtime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
  stat->st_atime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS);
  stat->st_size =  g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_SIZE);
}

/* Check if any component in path is present in the whiteouts hash table.  */
static gboolean
check_if_any_component_present (char *path, GHashTable *whiteouts)
{
  char *it = path;
  for (;;)
    {
      it = strrchr (it + 1, '/');
      if (!it)
        return g_hash_table_contains (whiteouts, path);

      *it = '\0';
      if (g_hash_table_contains (whiteouts, path))
        {
          *it = '/';
          return TRUE;
        }
      *it = '/';
    }
}

static int
stat_file (const char *path, struct stat *st_data, OstreeRepoFile **out, char **target)
{
  int i;
  g_autofree char *path_copy = NULL;
  if (out)
    *out = NULL;

  /* Go backward in the layers until the file is found.  If whiteouts
     are used and a whiteout is found, then we exit immediately as
     the file was removed and not upper layers (that we already
     checked) add it back.  */
  for (i = layers->len - 1; i >= 0; i--)
    {
      g_autoptr(GFileInfo) file_info = NULL;
      struct layer *layer = g_array_index (layers, struct layer *, i);
      g_autoptr(GFile) f = g_file_resolve_relative_path (layer->root, path);
      file_info = g_file_query_info (f, OSTREE_GIO_FAST_QUERYINFO,
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                     NULL, NULL);
      if (file_info == NULL)
        {
          if (layer->whiteouts)
            {
              if (!path_copy)
                path_copy = g_strdup (path);

              /* If any component in path was deleted, then the file
                 is not visible in upper layers, return ENOENT.  */
              if (check_if_any_component_present (path_copy, layer->whiteouts))
                return -ENOENT;
            }
          continue;
        }

      if (st_data)
        convert_file_info_to_stat (file_info, st_data);
      if (target)
        {
          if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_SYMBOLIC_LINK)
            *target = NULL;
          else
            {
              const char *target_path;
              g_autoptr(GFile) path_parent = NULL;
              path_parent = g_file_get_parent (f);
              target_path = g_file_info_get_symlink_target (file_info);
              if (!path_parent)
                *target = g_strdup (target_path);
              else
                {
                  g_autoptr(GFile) relative_path = g_file_resolve_relative_path (path_parent, target_path);
                  *target = g_file_get_path (relative_path);
                }
            }

        }
      if (out)
        *out = (OstreeRepoFile *) g_steal_pointer (&f);
      return 0;
    }

  return -ENOENT;
}

static int
callback_getattr (const char *path, struct stat *st_data)
{
  return stat_file (path, st_data, NULL, NULL);
}

static void
set_stat_from_dirmeta (struct stat *stat,
                       GVariant   *metadata)
{
  guint32 uid, gid, mode;

  stat->st_mode = S_IFDIR;

  /* PARSE OSTREE_OBJECT_TYPE_DIR_META */
  g_variant_get (metadata, "(uuu@a(ayay))",
                 &uid, &gid, &mode, NULL);
  stat->st_uid = GUINT32_FROM_BE (uid);
  stat->st_gid = GUINT32_FROM_BE (gid);
  stat->st_mode = GUINT32_FROM_BE (mode);
}

static int
read_single_directory (GHashTable *files, GFile *f)
{
  int err = -ENOENT;
  GError *error = NULL;
  int c, n;
  struct stat *stat;
  const char *name = NULL;
  GVariant *files_variant = NULL;
  GVariant *dirs_variant = NULL;
  OstreeRepoFile *file = (OstreeRepoFile *) f;
  GVariant *root_contents = NULL;
  char tmp_checksum[OSTREE_SHA256_STRING_LEN + 1];
  const guchar *csum_bytes;
  g_autoptr(GFileInfo) file_info = NULL;

  file_info = g_file_query_info (f, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 NULL, NULL);
  if (!file_info)
    goto out;
  if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
    {
      err = -ENOTDIR;
      goto out;
    }


  if (!ostree_repo_file_ensure_resolved (file, &error))
    goto out;

  root_contents = ostree_repo_file_tree_get_contents (file);

  files_variant = g_variant_get_child_value (root_contents, 0);
  dirs_variant = g_variant_get_child_value (root_contents, 1);

  c = g_variant_n_children (dirs_variant);
  for (n = 0; n < c; n++)
    {
      g_autoptr(GVariant) meta_csum_v = NULL;
      g_autoptr(GVariant) metadata = NULL;

      g_variant_get_child (dirs_variant, n, "(&s@ay@ay)", &name, NULL, &meta_csum_v);

      if (config.whiteouts && g_str_has_prefix (name, WHITEOUT_PREFIX))
        {
          const char *file_to_remove = name + strlen (WHITEOUT_PREFIX);
          g_hash_table_remove (files, file_to_remove);
          continue;
        }

      csum_bytes = ostree_checksum_bytes_peek_validate (meta_csum_v, &error);
      if (csum_bytes == NULL)
        goto out;

      ostree_checksum_inplace_from_bytes (csum_bytes, tmp_checksum);

      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_DIR_META,
                                     tmp_checksum, &metadata, &error))
        goto out;

      stat = g_malloc0 (sizeof *stat);
      set_stat_from_dirmeta (stat, metadata);
      g_hash_table_replace (files, g_strdup (name), stat);
    }

  c = g_variant_n_children (files_variant);
  for (n = 0; n < c; n++)
    {
      char csum_path[OSTREE_SHA256_STRING_LEN + 8];
      g_autoptr(GVariant) content_csum_v = NULL;

      g_variant_get_child (files_variant, n, "(&s@ay)", &name, &content_csum_v);

      if (config.whiteouts && g_str_has_prefix (name, WHITEOUT_PREFIX))
        {
          const char *file_to_remove = name + strlen (WHITEOUT_PREFIX);
          g_hash_table_remove (files, file_to_remove);
          continue;
        }

      csum_bytes = ostree_checksum_bytes_peek_validate (content_csum_v, &error);
      if (csum_bytes == NULL)
        goto out;
      ostree_checksum_inplace_from_bytes (csum_bytes, tmp_checksum);

      get_ostree_object_path (csum_path, tmp_checksum);
      stat = g_malloc (sizeof *stat);
      if (fstatat (objects_basefd, csum_path, stat, AT_SYMLINK_NOFOLLOW) < 0)
        {
          err = -errno;
          goto out;
        }
      g_hash_table_replace (files, g_strdup (name), stat);
    }

  err = 0;

 out:
  if (error)
    g_error_free (error);
  return err;
}

static int
callback_readdir (const char *path, void *buf, fuse_fill_dir_t filler,
		  off_t offset, struct fuse_file_info *fi)
{
  int i, err = 0;
  gboolean found_any = FALSE;
  GHashTable *files = NULL;
  GHashTableIter iter;
  gpointer key, value;
  g_autofree char *path_copy = NULL;
  gboolean from_cache = FALSE;

  if (memcache_dir)
    files = g_hash_table_lookup (memcache_dir, path);

  if (files)
    from_cache = TRUE;
  else
    {
      /* name -> struct stat.  */
      files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      for (i = 0; i < layers->len; i++)
        {
          struct layer *layer = g_array_index (layers, struct layer *, i);
          g_autoptr(GFile) f = NULL;

          if (layer->whiteouts)
            {
              if (!path_copy)
                path_copy = g_strdup (path);
              /* If any component in path was deleted, reset everything.  */
              if (check_if_any_component_present (path_copy, layer->whiteouts))
                {
                  g_hash_table_remove_all (files);
                  found_any = FALSE;
                  continue;
                }
            }

          f = g_file_resolve_relative_path (layer->root, path);
          err = read_single_directory (files, f);
          if (err == -ENOENT)
            continue;
          if (err != 0)
            goto out;
          found_any = TRUE;
        }
      if (!found_any && err == -ENOENT)
        goto out;
    }

  filler (buf, ".", NULL, 0);
  filler (buf, "..", NULL, 0);

  g_hash_table_iter_init (&iter, files);
  while (g_hash_table_iter_next (&iter, &key, &value))
    if (filler (buf, key, value, 0))
      break;

  if (!from_cache)
    {
       g_hash_table_replace (memcache_dir, g_strdup (path), g_steal_pointer (&files));
    }
 out:
  if (files != NULL && !from_cache)
    g_object_unref (files);
  return err;
}

static int
callback_mknod (const char *path, mode_t mode, dev_t rdev)
{
  return -EROFS;
}

static int
callback_mkdir (const char *path, mode_t mode)
{
  return -EROFS;
}

static int
callback_unlink (const char *path)
{
  return -EROFS;
}

static int
callback_rmdir (const char *path)
{
  return -EROFS;
}

static int
callback_symlink (const char *from, const char *to)
{
  return -EROFS;
}

static int
callback_rename (const char *from, const char *to)
{
  return -EROFS;
}

static int
callback_link (const char *from, const char *to)
{
  return -EROFS;
}

static int
callback_chmod (const char *path, mode_t mode)
{
  return -EROFS;
}

static int
callback_chown (const char *path, uid_t uid, gid_t gid)
{
  return -EROFS;
}

static int
callback_truncate (const char *path, off_t size)
{
  return -EROFS;
}

static int
callback_utime (const char *path, struct utimbuf *buf)
{
  OstreeRepoFile *file = NULL;
  struct stat st_data;
  int ret;

  ret = stat_file (path, &st_data,  &file, NULL);
  if (ret < 0)
    goto out;

  buf->actime = st_data.st_mtime;
  buf->modtime = st_data.st_atime;

  ret = 0;
 out:
  return ret;
}

static int
callback_open (const char *path, struct fuse_file_info *finfo)
{
  const char *csum;
  struct stat st_data;
  OstreeRepoFile *file = NULL;
  int ret;
  int fd;

  if ((finfo->flags & O_ACCMODE) != O_RDONLY)
    {
      ret = -EROFS;
      goto out;
    }

  ret = stat_file (path, &st_data,  &file, NULL);
  if (ret)
    goto out;

  if (st_data.st_mode == S_IFDIR)
    {
      ret = -EISDIR;
      goto out;
    }

  csum = ostree_repo_file_get_checksum (file);
  fd = open_ostree_object_file (csum);
  if (fd < 0)
    {
      ret = -errno;
      goto out;
    }

  ret = 0;
  finfo->fh = fd;
 out:
  if (file)
    g_object_unref (file);
  return ret;
}

static int
callback_create (const char *path, mode_t mode, struct fuse_file_info *finfo)
{
  return -EROFS;
}

static int
callback_read (const char *path, char *buf, size_t size, off_t offset,
	       struct fuse_file_info *finfo)
{
  int r = pread (finfo->fh, buf, size, offset);
  if (r < 0)
    return -errno;
  return r;
}

static int
callback_write (const char *path, const char *buf, size_t size, off_t offset,
		struct fuse_file_info *finfo)
{
  return -EROFS;
}

static int
callback_release (const char *path, struct fuse_file_info *finfo)
{
  (void) close (finfo->fh);
  return 0;
}

static int
callback_fsync (const char *path, int notused, struct fuse_file_info *finfo)
{
  if (fsync (finfo->fh) == -1)
    return -errno;
  return 0;
}

static int
callback_access (const char *path, int mode)
{
  int ret;
  struct stat st_data;
  if (mode & W_OK)
    return -1;
  ret = stat_file (path, &st_data, NULL, NULL);
  if (ret < 0)
    return ret;

  return !(st_data.st_mode & mode);
}

static int
callback_setxattr (const char *path, const char *name, const char *value,
		   size_t size, int flags)
{
  return -EROFS;
}

static int
callback_getxattr (const char *path, const char *name, char *value, size_t size)
{
  GVariant *xattrs = NULL;
  OstreeRepoFile *file = NULL;
  int ret, i, n;
  ret = stat_file (path, NULL, &file, NULL);
  if (ret < 0)
    goto out;

  if (!ostree_repo_file_get_xattrs (file, &xattrs, NULL, NULL))
    {
      ret = 0;
      goto out;
    }

  n = g_variant_n_children (xattrs);
  for (i = 0; i < n; i++)
    {
      const guint8* name_xattr;
      g_autoptr(GVariant) value_xattr = NULL;
      const guint8* value_xattr_data;
      gsize value_xattr_len;

      g_variant_get_child (xattrs, i, "(^&ay@ay)", &name_xattr, &value_xattr);
      if (g_str_equal (name, name_xattr))
        {
          value_xattr_data = g_variant_get_fixed_array (value_xattr, &value_xattr_len, 1);
          if (value == NULL)
            {
              ret = value_xattr_len;
              goto out;
            }
          if (value_xattr_len > size)
            {
              ret = -ERANGE;
              goto out;
            }

          memcpy (value, value_xattr_data, value_xattr_len);
          ret = value_xattr_len;
          break;
        }
    }

 out:
  if (file)
    g_object_unref (file);
  if (xattrs)
    g_variant_unref (xattrs);
  return ret;
}

static int
callback_listxattr (const char *path, char *list, size_t size)
{
  GString *buf = NULL;
  OstreeRepoFile *file = NULL;
  int ret;
  GVariant *xattrs = NULL;
  GBytes *bytes = NULL;
  ret = stat_file (path, NULL, &file, NULL);
  if (ret < 0)
    goto out;

  if (!ostree_repo_file_get_xattrs (file, &xattrs, NULL, NULL))
    {
      ret = 0;
      goto out;
    }

  {
    int i, n;
    n = g_variant_n_children (xattrs);
    if (n)
      buf = g_string_new ("");
    for (i = 0; i < n; i++)
      {
        const guint8* name;
        g_autoptr(GVariant) value = NULL;
        g_variant_get_child (xattrs, i, "(^&ay@ay)", &name, &value);
        g_string_append (buf, (const char *) name);
        g_string_append_len (buf, "\0", 1);
      }
  }

  bytes = g_string_free_to_bytes (buf);

  if (list == NULL)
    {
      ret = g_bytes_get_size (bytes);
      goto out;
    }
  if (g_bytes_get_size (bytes) > size)
    {
      ret = -ERANGE;
      goto out;
    }

  ret = g_bytes_get_size (bytes);
  memcpy (list, g_bytes_get_data (bytes, NULL), ret);

 out:
  if (file)
    g_object_unref (file);
  if (xattrs)
    g_variant_unref (xattrs);
  if (bytes)
    g_bytes_unref (bytes);
  return ret;
}

/*
 * Remove an extended attribute.
 */
static int
callback_removexattr (const char *path, const char *name)
{
  return -EROFS;
}

static int
callback_readlink (const char *path, char *buf, size_t size)
{

  OstreeRepoFile *file = NULL;
  int ret;
  g_autofree char *target_path = NULL;

  ret = stat_file (path, NULL, &file, &target_path);
  if (ret < 0)
    goto out;

  if (target_path == NULL)
    return -ENOENT;

  ret = g_strlcpy (buf, target_path, size);
  if (ret == size)
    return -ERANGE;

 out:
  if (file)
    g_object_unref (file);
  return 0;
}

struct fuse_operations callback_oper = {
  .getattr = callback_getattr,
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
  .release = callback_release,
  .fsync = callback_fsync,
  .access = callback_access,
  .readlink = callback_readlink,

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
	   "usage: %s -orepo=repo [-owhiteouts] [-o memcache] -olayers=BRANCH_1[:BRANCH_N] mountpoint [options]\n"
	   "\n"
	   "   Mount a tree from OSTree\n"
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
      return 1;
    case FUSE_OPT_KEY_OPT:
      return 1;
    case KEY_HELP:
      usage (outargs->argv[0]);
      exit (EXIT_SUCCESS);
    default:
      fprintf (stderr, "see `%s -h' for usage\n", outargs->argv[0]);
      exit (EXIT_FAILURE);
    }
  return 1;
}

#define MYFS_OPT(t, p, v) { t, offsetof(struct opt_config, p), v }

static struct fuse_opt rofs_opts[] = {
  FUSE_OPT_KEY ("-h", KEY_HELP),
  FUSE_OPT_KEY ("--help", KEY_HELP),
  FUSE_OPT_KEY ("-V", KEY_VERSION),
  FUSE_OPT_KEY ("--version", KEY_VERSION),
  MYFS_OPT ("layers=%s", layers, 0),
  MYFS_OPT ("repo=%s", repo_location, 0),
  MYFS_OPT ("whiteouts", whiteouts, 1),
  MYFS_OPT ("memcache", memcache, 1),
  FUSE_OPT_END
};

static gboolean
collect_whiteout_files (GFile    *f,
                        GHashTable *whiteouts,
                        GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFile) child = NULL;
  g_autoptr(GFileInfo) child_info = NULL;

  dir_enum = g_file_enumerate_children (f, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL,
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, NULL, error)) != NULL)
    {
      g_autofree char *path;
      g_autofree char *basename = NULL;
      g_clear_object (&child);
      child = g_file_get_child (f, g_file_info_get_name (child_info));

      path = g_file_get_path (child);
      basename = g_file_get_basename (child);

      if (g_str_has_prefix (basename, WHITEOUT_PREFIX))
        {
          char *whiteout;
          g_autoptr(GFile) removed_path = g_file_resolve_relative_path (f, basename + strlen (WHITEOUT_PREFIX));

          whiteout = g_file_get_path (removed_path);
          g_hash_table_replace (whiteouts, whiteout, whiteout);
        }
      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!collect_whiteout_files (child, whiteouts, error))
            goto out;
        }

      g_clear_object (&child_info);
    }

  ret = TRUE;
 out:
  return ret;
}

int
main (int argc, char *argv[])
{
  struct fuse_args args = FUSE_ARGS_INIT (argc, argv);
  int res;
  GError *error = NULL;

  res = fuse_opt_parse (&args, &config, rofs_opts, rofs_parse_opt);
  if (res != 0)
    {
      fprintf (stderr, "Invalid arguments\n");
      fprintf (stderr, "see `%s -h' for usage\n", argv[0]);
      exit (EXIT_FAILURE);
    }

  if (config.repo_location != NULL)
    config.repo_location = realpath (config.repo_location, NULL);
  else
    {
      fprintf (stderr, "Missing repo\n");
      fprintf (stderr, "see `%s -h' for usage\n", argv[0]);
      exit (EXIT_FAILURE);
    }
  if (config.layers == NULL)
    {
      fprintf (stderr, "Missing layers\n");
      fprintf (stderr, "see `%s -h' for usage\n", argv[0]);
      exit (EXIT_FAILURE);
    }

  {
    g_autofree char *obj_path = g_strdup_printf ("%s/objects", config.repo_location);
    objects_basefd = openat (AT_FDCWD, obj_path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
    if (objects_basefd < 0)
      {
        fprintf (stderr, "Could not open objects directory: %s\n", strerror (errno));
        goto fail;
      }
  }

  {
    OstreeRepoMode repo_mode;
    g_autoptr(GFile) repo_file = g_file_new_for_path (config.repo_location);
    repo = ostree_repo_new (repo_file);
    if (!ostree_repo_open (repo, NULL, &error))
      goto fail;
    repo_mode = ostree_repo_get_mode (repo);
    if (repo_mode != OSTREE_REPO_MODE_BARE &&
        repo_mode != OSTREE_REPO_MODE_BARE_USER)
      {
        fprintf (stderr, "Invalid repo type, can mount only from bare repositories\n");
        goto fail;
      }
    if (ostree_repo_get_parent (repo) != NULL)
      {
        fprintf (stderr, "Repositories with a parent are not supported (yet).\n");
        goto fail;
      }
  }

  {
    int i;
    gchar **commits =  g_strsplit (config.layers, ":", -1);

    for (i = 0; commits[i]; i++);

    layers = g_array_sized_new (TRUE, TRUE, sizeof (struct layer *), i + 1);

    for (i = 0; commits[i]; i++)
      {
        char *rev;
        GFile *root;
        struct layer *layer = g_malloc (sizeof *layer);

        if (!ostree_repo_resolve_rev (repo, commits[i], FALSE, &rev, &error))
          goto fail;

        if (!ostree_repo_read_commit (repo, rev, &root, NULL, NULL, &error))
          goto fail;

        layer->root = root;
        if (!config.whiteouts)
          layer->whiteouts = NULL;
        else
          {
            layer->whiteouts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
            if (!collect_whiteout_files (root, layer->whiteouts, &error))
              goto fail;
          }

        g_array_append_val (layers, layer);
      }
    g_strfreev (commits);
  }

  if (config.memcache)
    memcache_dir = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  fuse_main (args.argc, args.argv, &callback_oper, NULL);

  return 0;

fail:
  g_printerr ("error: %s\n", error->message);
  g_error_free (error);
  return -1;
}
