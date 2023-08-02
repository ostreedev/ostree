/*
 * Copyright (C) Red Hat, Inc.
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

#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <sys/ioctl.h>

#include "ostree-core-private.h"
#include "ostree-repo-file.h"
#include "ostree-repo-private.h"

#ifdef HAVE_COMPOSEFS
#include <libcomposefs/lcfs-writer.h>
#endif

#ifdef HAVE_LINUX_FSVERITY_H
#include <linux/fsverity.h>
#endif

gboolean
_ostree_repo_parse_composefs_config (OstreeRepo *self, GError **error)
{
  /* Currently experimental */
  OtTristate use_composefs;

  if (!ot_keyfile_get_tristate_with_default (self->config, _OSTREE_INTEGRITY_SECTION, "composefs",
                                             OT_TRISTATE_NO, &use_composefs, error))
    return FALSE;

  self->composefs_wanted = use_composefs;
#ifdef HAVE_COMPOSEFS
  self->composefs_supported = TRUE;
#else
  self->composefs_supported = FALSE;
#endif

  if (use_composefs == OT_TRISTATE_YES && !self->composefs_supported)
    return glnx_throw (error, "composefs required, but libostree compiled without support");

  return TRUE;
}

struct OstreeComposefsTarget
{
#ifdef HAVE_COMPOSEFS
  struct lcfs_node_s *dest;
#endif
  int ref_count;
};

/**
 * ostree_composefs_target_new:
 *
 * Creates a #OstreeComposefsTarget which can be used with
 * ostree_repo_checkout_composefs() to create a composefs image based
 * on a set of checkouts.
 *
 * Returns: (transfer full): a new of #OstreeComposefsTarget
 */
OstreeComposefsTarget *
ostree_composefs_target_new (void)
{
  OstreeComposefsTarget *target;

  target = g_slice_new0 (OstreeComposefsTarget);

#ifdef HAVE_COMPOSEFS
  target->dest = lcfs_node_new ();
  lcfs_node_set_mode (target->dest, 0755 | S_IFDIR);
#endif

  target->ref_count = 1;

  return target;
}

/**
 * ostree_composefs_target_ref:
 * @target: an #OstreeComposefsTarget
 *
 * Increase the reference count on the given @target.
 *
 * Returns: (transfer full): a copy of @target, for convenience
 */
OstreeComposefsTarget *
ostree_composefs_target_ref (OstreeComposefsTarget *target)
{
  gint refcount;
  g_return_val_if_fail (target != NULL, NULL);
  refcount = g_atomic_int_add (&target->ref_count, 1);
  g_assert (refcount > 0);
  return target;
}

/**
 * ostree_composefs_target_unref:
 * @target: (transfer full): an #OstreeComposefsTarget
 *
 * Decrease the reference count on the given @target and free it if the
 * reference count reaches 0.
 */
void
ostree_composefs_target_unref (OstreeComposefsTarget *target)
{
  g_return_if_fail (target != NULL);
  g_return_if_fail (target->ref_count > 0);

  if (g_atomic_int_dec_and_test (&target->ref_count))
    {
#ifdef HAVE_COMPOSEFS
      g_clear_pointer (&target->dest, lcfs_node_unref);
#endif
      g_slice_free (OstreeComposefsTarget, target);
    }
}

G_DEFINE_BOXED_TYPE (OstreeComposefsTarget, ostree_composefs_target, ostree_composefs_target_ref,
                     ostree_composefs_target_unref);

#ifdef HAVE_COMPOSEFS

static ssize_t
_composefs_read_cb (void *_file, void *buf, size_t count)
{
  GInputStream *in = _file;
  gsize bytes_read;

  if (!g_input_stream_read_all (in, buf, count, &bytes_read, NULL, NULL))
    {
      errno = EIO;
      return -1;
    }

  return bytes_read;
}

static ssize_t
_composefs_write_cb (void *file, void *buf, size_t len)
{
  int fd = GPOINTER_TO_INT (file);
  const char *content = buf;
  ssize_t res = 0;

  while (len > 0)
    {
      res = write (fd, content, len);
      if (res < 0 && errno == EINTR)
        continue;

      if (res <= 0)
        {
          if (res == 0) /* Unexpected short write, should not happen when writing to a file */
            errno = ENOSPC;
          return -1;
        }

      break;
    }

  return res;
}

#else /* HAVE_COMPOSEFS */

static gboolean
composefs_not_supported (GError **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "composefs is not supported in this ostree build");
  return FALSE;
}

#endif

/**
 * ostree_composefs_target_write:
 * @target: an #OstreeComposefsTarget
 * @fd: Write image here (or -1 to not write)
 * @out_fsverity_digest: (out) (array fixed-size=32) (nullable): Return location for the fsverity
 * binary digest, or %NULL to not compute it
 * @cancellable: Cancellable
 * @error: Error
 *
 * Writes a composefs image file to the filesystem at the
 * path specified by @destination_dfd and destination_path (if not %NULL)
 * and (optionally) computes the fsverity digest of the image.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
ostree_composefs_target_write (OstreeComposefsTarget *target, int fd, guchar **out_fsverity_digest,
                               GCancellable *cancellable, GError **error)
{
#ifdef HAVE_COMPOSEFS
  g_autoptr (GOutputStream) tmp_out = NULL;
  g_autoptr (GOutputStream) out = NULL;
  struct lcfs_node_s *root;
  g_autofree guchar *fsverity_digest = NULL;
  struct lcfs_write_options_s options = {
    LCFS_FORMAT_EROFS,
  };

  root = lcfs_node_lookup_child (target->dest, "root");
  if (root == NULL)
    root = target->dest; /* Nothing was checked out, use an empty dir */

  if (out_fsverity_digest)
    {
      fsverity_digest = g_malloc (OSTREE_SHA256_DIGEST_LEN);
      options.digest_out = fsverity_digest;
    }

  if (fd != -1)
    {
      options.file = GINT_TO_POINTER (fd);
      options.file_write_cb = _composefs_write_cb;
    }

  if (lcfs_write_to (root, &options) != 0)
    return glnx_throw_errno (error);

  if (out_fsverity_digest)
    *out_fsverity_digest = g_steal_pointer (&fsverity_digest);

  return TRUE;
#else
  return composefs_not_supported (error);
#endif
}

#ifdef HAVE_COMPOSEFS
static gboolean
_ostree_composefs_set_xattrs (struct lcfs_node_s *node, GVariant *xattrs, GCancellable *cancellable,
                              GError **error)
{
  const guint n = g_variant_n_children (xattrs);
  for (guint i = 0; i < n; i++)
    {
      const guint8 *name;
      g_autoptr (GVariant) value = NULL;
      g_variant_get_child (xattrs, i, "(^&ay@ay)", &name, &value);

      gsize value_len;
      const guint8 *value_data = g_variant_get_fixed_array (value, &value_len, 1);

      if (lcfs_node_set_xattr (node, (char *)name, (char *)value_data, value_len) != 0)
        return glnx_throw_errno_prefix (error, "Setting composefs xattrs for %s", name);
    }

  return TRUE;
}

static gboolean
checkout_one_composefs_file_at (OstreeRepo *repo, const char *checksum, struct lcfs_node_s *parent,
                                const char *destination_name, GCancellable *cancellable,
                                GError **error)
{
  g_autoptr (GInputStream) input = NULL;
  g_autoptr (GVariant) xattrs = NULL;
  struct lcfs_node_s *existing;

  /* Validate this up front to prevent path traversal attacks */
  if (!ot_util_filename_validate (destination_name, error))
    return FALSE;

  existing = lcfs_node_lookup_child (parent, destination_name);
  if (existing != NULL)
    return glnx_throw (error, "Target checkout file already exist");

  g_autoptr (GFileInfo) source_info = NULL;
  if (!ostree_repo_load_file (repo, checksum, &input, &source_info, &xattrs, cancellable, error))
    return FALSE;

  const guint32 source_mode = g_file_info_get_attribute_uint32 (source_info, "unix::mode");
  const guint32 source_uid = g_file_info_get_attribute_uint32 (source_info, "unix::uid");
  const guint32 source_gid = g_file_info_get_attribute_uint32 (source_info, "unix::gid");
  const guint64 source_size = g_file_info_get_size (source_info);
  const gboolean is_symlink
      = (g_file_info_get_file_type (source_info) == G_FILE_TYPE_SYMBOLIC_LINK);

  struct lcfs_node_s *node = lcfs_node_new ();
  if (node == NULL)
    return glnx_throw (error, "Out of memory");

  /* Takes ownership on success */
  if (lcfs_node_add_child (parent, node, destination_name) != 0)
    {
      lcfs_node_unref (node);
      return glnx_throw_errno (error);
    }

  lcfs_node_set_mode (node, source_mode);
  lcfs_node_set_uid (node, source_uid);
  lcfs_node_set_gid (node, source_gid);
  lcfs_node_set_size (node, source_size);
  if (is_symlink)
    {
      const char *source_symlink_target = g_file_info_get_symlink_target (source_info);
      if (lcfs_node_set_payload (node, source_symlink_target) != 0)
        return glnx_throw_errno (error);
    }
  else if (source_size != 0)
    {
      char loose_path_buf[_OSTREE_LOOSE_PATH_MAX];
      _ostree_loose_path (loose_path_buf, checksum, OSTREE_OBJECT_TYPE_FILE, OSTREE_REPO_MODE_BARE);
      if (lcfs_node_set_payload (node, loose_path_buf) != 0)
        return glnx_throw_errno (error);

      guchar *known_digest = NULL;

#ifdef HAVE_LINUX_FSVERITY_H
      /* First try to get the digest directly from the bare repo file.
       * This is the typical case when we're pulled into the target
       * system repo with verity on and are recreating the composefs
       * image during deploy. */
      char buf[sizeof (struct fsverity_digest) + OSTREE_SHA256_DIGEST_LEN];

      if (G_IS_UNIX_INPUT_STREAM (input))
        {
          int content_fd = g_unix_input_stream_get_fd (G_UNIX_INPUT_STREAM (input));
          struct fsverity_digest *d = (struct fsverity_digest *)&buf;
          d->digest_size = OSTREE_SHA256_DIGEST_LEN;

          if (ioctl (content_fd, FS_IOC_MEASURE_VERITY, d) == 0
              && d->digest_size == OSTREE_SHA256_DIGEST_LEN
              && d->digest_algorithm == FS_VERITY_HASH_ALG_SHA256)
            known_digest = d->digest;
        }
#endif

      if (known_digest)
        lcfs_node_set_fsverity_digest (node, known_digest);
      else if (lcfs_node_set_fsverity_from_content (node, input, _composefs_read_cb) != 0)
        return glnx_throw_errno (error);
    }

  if (xattrs)
    {
      if (!_ostree_composefs_set_xattrs (node, xattrs, cancellable, error))
        return FALSE;
    }

  g_clear_object (&input);

  return TRUE;
}

static gboolean
checkout_composefs_recurse (OstreeRepo *self, const char *dirtree_checksum,
                            const char *dirmeta_checksum, struct lcfs_node_s *parent,
                            const char *name, GCancellable *cancellable, GError **error)
{
  g_autoptr (GVariant) dirtree = NULL;
  g_autoptr (GVariant) dirmeta = NULL;
  g_autoptr (GVariant) xattrs = NULL;
  struct lcfs_node_s *directory;

  if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_DIR_TREE, dirtree_checksum, &dirtree,
                                 error))
    return FALSE;
  if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_DIR_META, dirmeta_checksum, &dirmeta,
                                 error))
    return FALSE;

  /* Parse OSTREE_OBJECT_TYPE_DIR_META */
  guint32 uid, gid, mode;
  g_variant_get (dirmeta, "(uuu@a(ayay))", &uid, &gid, &mode, &xattrs);
  uid = GUINT32_FROM_BE (uid);
  gid = GUINT32_FROM_BE (gid);
  mode = GUINT32_FROM_BE (mode);

  directory = lcfs_node_lookup_child (parent, name);
  if (directory != NULL && lcfs_node_get_mode (directory) != 0)
    {
      return glnx_throw (error, "Target checkout directory already exist");
    }
  else
    {
      directory = lcfs_node_new ();
      if (directory == NULL)
        return glnx_throw (error, "Out of memory");

      /* Takes ownership on success */
      if (lcfs_node_add_child (parent, directory, name) != 0)
        {
          lcfs_node_unref (directory);
          return glnx_throw_errno (error);
        }
    }

  lcfs_node_set_mode (directory, mode);
  lcfs_node_set_uid (directory, uid);
  lcfs_node_set_gid (directory, gid);

  /* Set the xattrs if we created the dir */
  if (xattrs && !_ostree_composefs_set_xattrs (directory, xattrs, cancellable, error))
    return FALSE;

  /* Process files in this subdir */
  {
    g_autoptr (GVariant) dir_file_contents = g_variant_get_child_value (dirtree, 0);
    GVariantIter viter;
    g_variant_iter_init (&viter, dir_file_contents);
    const char *fname;
    g_autoptr (GVariant) contents_csum_v = NULL;
    while (g_variant_iter_loop (&viter, "(&s@ay)", &fname, &contents_csum_v))
      {
        char tmp_checksum[OSTREE_SHA256_STRING_LEN + 1];
        _ostree_checksum_inplace_from_bytes_v (contents_csum_v, tmp_checksum);

        if (!checkout_one_composefs_file_at (self, tmp_checksum, directory, fname, cancellable,
                                             error))
          return FALSE;
      }
    contents_csum_v = NULL; /* iter_loop freed it */
  }

  /* Process subdirectories */
  {
    g_autoptr (GVariant) dir_subdirs = g_variant_get_child_value (dirtree, 1);
    const char *dname;
    g_autoptr (GVariant) subdirtree_csum_v = NULL;
    g_autoptr (GVariant) subdirmeta_csum_v = NULL;
    GVariantIter viter;
    g_variant_iter_init (&viter, dir_subdirs);
    while (
        g_variant_iter_loop (&viter, "(&s@ay@ay)", &dname, &subdirtree_csum_v, &subdirmeta_csum_v))
      {
        /* Validate this up front to prevent path traversal attacks. Note that
         * we don't validate at the top of this function like we do for
         * checkout_one_file_at() becuase I believe in some cases this function
         * can be called *initially* with user-specified paths for the root
         * directory.
         */
        if (!ot_util_filename_validate (dname, error))
          return FALSE;

        char subdirtree_checksum[OSTREE_SHA256_STRING_LEN + 1];
        _ostree_checksum_inplace_from_bytes_v (subdirtree_csum_v, subdirtree_checksum);
        char subdirmeta_checksum[OSTREE_SHA256_STRING_LEN + 1];
        _ostree_checksum_inplace_from_bytes_v (subdirmeta_csum_v, subdirmeta_checksum);
        if (!checkout_composefs_recurse (self, subdirtree_checksum, subdirmeta_checksum, directory,
                                         dname, cancellable, error))
          return FALSE;
      }
    /* Freed by iter-loop */
    subdirtree_csum_v = NULL;
    subdirmeta_csum_v = NULL;
  }

  return TRUE;
}

/* Begin a checkout process */
static gboolean
checkout_composefs_tree (OstreeRepo *self, OstreeComposefsTarget *target, OstreeRepoFile *source,
                         GFileInfo *source_info, GCancellable *cancellable, GError **error)
{
  if (g_file_info_get_file_type (source_info) != G_FILE_TYPE_DIRECTORY)
    return glnx_throw (error, "Root checkout of composefs must be directory");

  /* Cache any directory metadata we read during this operation;
   * see commit b7afe91e21143d7abb0adde440683a52712aa246
   */
  g_auto (OstreeRepoMemoryCacheRef) memcache_ref;
  _ostree_repo_memory_cache_ref_init (&memcache_ref, self);

  g_assert_cmpint (g_file_info_get_file_type (source_info), ==, G_FILE_TYPE_DIRECTORY);

  const char *dirtree_checksum = ostree_repo_file_tree_get_contents_checksum (source);
  const char *dirmeta_checksum = ostree_repo_file_tree_get_metadata_checksum (source);
  return checkout_composefs_recurse (self, dirtree_checksum, dirmeta_checksum, target->dest, "root",
                                     cancellable, error);
}

static struct lcfs_node_s *
ensure_lcfs_dir (struct lcfs_node_s *parent, const char *name, GError **error)
{
  struct lcfs_node_s *node;

  node = lcfs_node_lookup_child (parent, name);
  if (node != NULL)
    return node;

  node = lcfs_node_new ();
  lcfs_node_set_mode (node, 0755 | S_IFDIR);
  if (lcfs_node_add_child (parent, node, name) != 0)
    {
      lcfs_node_unref (node);
      glnx_throw_errno (error);
      return NULL;
    }

  return node;
}

#endif /* HAVE_COMPOSEFS */

/**
 * ostree_repo_checkout_composefs:
 * @self: Repo
 * @target: A target for the checkout
 * @source: Source tree
 * @cancellable: Cancellable
 * @error: Error
 *
 * Check out @source into @target, which is an in-memory
 * representation of a composefs image. The @target can be reused
 * multiple times to layer multiple checkouts before writing out the
 * image to disk using ostree_composefs_target_write().
 *
 * There are various options specified by @options that affect
 * how the image is created.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
ostree_repo_checkout_composefs (OstreeRepo *self, OstreeComposefsTarget *target,
                                OstreeRepoFile *source, GCancellable *cancellable, GError **error)
{
#ifdef HAVE_COMPOSEFS
  char *root_dirs[] = { "usr", "etc", "boot", "var", "sysroot" };
  int i;
  struct lcfs_node_s *root, *dir;

  g_autoptr (GFileInfo) target_info
      = g_file_query_info (G_FILE (source), OSTREE_GIO_FAST_QUERYINFO,
                           G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
  if (!target_info)
    return FALSE;

  if (!checkout_composefs_tree (self, target, source, target_info, cancellable, error))
    return FALSE;

  /* We need a root dir */
  root = ensure_lcfs_dir (target->dest, "root", error);
  if (root == NULL)
    return FALSE;

  /* To work as a rootfs we need some root directories to use as bind-mounts */
  for (i = 0; i < G_N_ELEMENTS (root_dirs); i++)
    {
      dir = ensure_lcfs_dir (root, root_dirs[i], error);
      if (dir == NULL)
        return FALSE;
    }

  return TRUE;
#else
  return composefs_not_supported (error);
#endif
}

/**
 * ostree_repo_commit_add_composefs_metadata:
 * @self: Repo
 * @format_version: Must be zero
 * @dict: A GVariant builder of type a{sv}
 * @repo_root: the target filesystem tree
 * @cancellable: Cancellable
 * @error: Error
 *
 * Compute the composefs digest for a filesystem tree
 * and insert it into metadata for a commit object.  The composefs
 * digest covers the entire filesystem tree and can be verified by
 * the composefs mount tooling.
 */
_OSTREE_PUBLIC
gboolean
ostree_repo_commit_add_composefs_metadata (OstreeRepo *self, guint format_version,
                                           GVariantDict *dict, OstreeRepoFile *repo_root,
                                           GCancellable *cancellable, GError **error)
{
#ifdef HAVE_COMPOSEFS
  /* For now */
  g_assert (format_version == 0);

  g_autoptr (OstreeComposefsTarget) target = ostree_composefs_target_new ();

  if (!ostree_repo_checkout_composefs (self, target, repo_root, cancellable, error))
    return FALSE;

  g_autofree guchar *fsverity_digest = NULL;
  if (!ostree_composefs_target_write (target, -1, &fsverity_digest, cancellable, error))
    return FALSE;

  g_variant_dict_insert_value (
      dict, OSTREE_COMPOSEFS_DIGEST_KEY_V0,
      ot_gvariant_new_bytearray (fsverity_digest, OSTREE_SHA256_DIGEST_LEN));

  return TRUE;
#else
  return composefs_not_supported (error);
#endif
}
