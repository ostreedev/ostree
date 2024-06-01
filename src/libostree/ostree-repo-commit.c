/*
 * Copyright (C) 2011,2013 Colin Walters <walters@verbum.org>
 * Copyright (C) 2022 Igalia S.L.
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <ext2fs/ext2_fs.h>
#include <gio/gfiledescriptorbased.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib-unix.h>
#include <glib/gprintf.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>

#include "ostree-checksum-input-stream.h"
#include "ostree-core-private.h"
#include "ostree-repo-file-enumerator.h"
#include "ostree-repo-private.h"
#include "ostree-sepolicy-private.h"
#include "ostree-varint.h"
#include "ostree.h"
#include "otutil.h"

/* The standardized version of BTRFS_IOC_CLONE */
#ifndef FICLONE
#define FICLONE _IOW (0x94, 9, int)
#endif

/* Understanding ostree's fsync strategy
 *
 * A long time ago, ostree used to invoke fsync() on each object,
 * then move it into the objects directory.  However, it turned
 * out to be a *lot* faster to write the objects into a separate "staging"
 * directory (letting the filesystem handle writeback how it likes)
 * and then only walk over each of the files, fsync(), then rename()
 * into place.  See also https://lwn.net/Articles/789024/
 *
 * (We also support a "disable fsync entirely" mode, where you don't
 * care about integrity; e.g. test suites using disposable VMs).
 *
 * This "delayed fsync" pattern though is much worse for other concurrent processes
 * like databases because it forces a lot to go through the filesystem
 * journal at once once we do the sync.  So now we support a `per_object_fsync`
 * option that again invokes `fsync()` directly.  This also notably
 * provides "backpressure", ensuring we aren't queuing up a huge amount
 * of I/O at once.
 */

/* The directory where we place content */
static int
commit_dest_dfd (OstreeRepo *self)
{
  if (self->per_object_fsync)
    return self->objects_dir_fd;
  else if (self->in_transaction && !self->disable_fsync)
    return self->commit_stagedir.fd;
  else
    return self->objects_dir_fd;
}

/* If we don't have O_TMPFILE, or for symlinks we'll create temporary
 * files.  If we have a txn, use the staging dir to ensure that
 * things are consistently locked against concurrent cleanup, and
 * in general we have all of our data in one place.
 */
static int
commit_tmp_dfd (OstreeRepo *self)
{
  if (self->in_transaction)
    return self->commit_stagedir.fd;
  else
    return self->tmp_dir_fd;
}

/* The objects/ directory has a two-character directory prefix for checksums
 * to avoid putting lots of files in a single directory.   This technique
 * is quite old, but Git also uses it for example.
 */
gboolean
_ostree_repo_ensure_loose_objdir_at (int dfd, const char *loose_path, GCancellable *cancellable,
                                     GError **error)
{
  char loose_prefix[3];

  loose_prefix[0] = loose_path[0];
  loose_prefix[1] = loose_path[1];
  loose_prefix[2] = '\0';
  if (mkdirat (dfd, loose_prefix, 0777) == -1)
    {
      if (G_UNLIKELY (errno != EEXIST))
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }
  return TRUE;
}

/* This GVariant is the header for content objects (regfiles and symlinks) */
static GVariant *
create_file_metadata (guint32 uid, guint32 gid, guint32 mode, GVariant *xattrs)
{
  GVariant *ret_metadata = NULL;
  g_autoptr (GVariant) tmp_xattrs = NULL;

  if (xattrs == NULL)
    tmp_xattrs = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("(ayay)"), NULL, 0));

  ret_metadata = g_variant_new ("(uuu@a(ayay))", GUINT32_TO_BE (uid), GUINT32_TO_BE (gid),
                                GUINT32_TO_BE (mode), xattrs ? xattrs : tmp_xattrs);
  g_variant_ref_sink (ret_metadata);

  return ret_metadata;
}

/* bare-user repositories store file metadata as a user xattr */
gboolean
_ostree_write_bareuser_metadata (int fd, guint32 uid, guint32 gid, guint32 mode, GVariant *xattrs,
                                 GError **error)
{
  if (xattrs != NULL && !_ostree_validate_structureof_xattrs (xattrs, error))
    return FALSE;
  g_autoptr (GVariant) filemeta = create_file_metadata (uid, gid, mode, xattrs);

  if (TEMP_FAILURE_RETRY (fsetxattr (fd, "user.ostreemeta", (char *)g_variant_get_data (filemeta),
                                     g_variant_get_size (filemeta), 0))
      != 0)
    return glnx_throw_errno_prefix (error, "fsetxattr(user.ostreemeta)");

  return TRUE;
}

/* See https://github.com/ostreedev/ostree/pull/698 */
#ifdef WITH_SMACK
#define XATTR_NAME_SMACK "security.SMACK64"
#endif

static void
ot_security_smack_reset_dfd_name (int dfd, const char *name)
{
#ifdef WITH_SMACK
  char buf[PATH_MAX];
  /* See glnx-xattrs.c */
  snprintf (buf, sizeof (buf), "/proc/self/fd/%d/%s", dfd, name);
  (void)lremovexattr (buf, XATTR_NAME_SMACK);
#endif
}

static void
ot_security_smack_reset_fd (int fd)
{
#ifdef WITH_SMACK
  (void)fremovexattr (fd, XATTR_NAME_SMACK);
#endif
}

/* Given an O_TMPFILE regular file, link it into place. */
gboolean
_ostree_repo_commit_tmpf_final (OstreeRepo *self, const char *checksum, OstreeObjectType objtype,
                                GLnxTmpfile *tmpf, GCancellable *cancellable, GError **error)
{
  char tmpbuf[_OSTREE_LOOSE_PATH_MAX];
  _ostree_loose_path (tmpbuf, checksum, objtype, self->mode);

  int dest_dfd = commit_dest_dfd (self);
  if (!_ostree_repo_ensure_loose_objdir_at (dest_dfd, tmpbuf, cancellable, error))
    return FALSE;

  if (!_ostree_tmpf_fsverity (self, tmpf, NULL, error))
    return FALSE;

  if (!glnx_link_tmpfile_at (tmpf, GLNX_LINK_TMPFILE_NOREPLACE_IGNORE_EXIST, dest_dfd, tmpbuf,
                             error))
    return FALSE;
  /* We're done with the fd */
  glnx_tmpfile_clear (tmpf);
  return TRUE;
}

/* Given a dfd+path combination (may be regular file or symlink),
 * rename it into place.
 */
static gboolean
commit_path_final (OstreeRepo *self, const char *checksum, OstreeObjectType objtype,
                   OtCleanupUnlinkat *tmp_path, GCancellable *cancellable, GError **error)
{
  /* The final renameat() */
  char tmpbuf[_OSTREE_LOOSE_PATH_MAX];
  _ostree_loose_path (tmpbuf, checksum, objtype, self->mode);

  int dest_dfd = commit_dest_dfd (self);
  if (!_ostree_repo_ensure_loose_objdir_at (dest_dfd, tmpbuf, cancellable, error))
    return FALSE;

  if (renameat (tmp_path->dfd, tmp_path->path, dest_dfd, tmpbuf) == -1)
    {
      if (errno != EEXIST)
        return glnx_throw_errno_prefix (error, "Storing file '%s'", tmp_path->path);
      /* Otherwise, the caller's cleanup will unlink+free */
    }
  else
    {
      /* The tmp path was consumed */
      ot_cleanup_unlinkat_clear (tmp_path);
    }

  return TRUE;
}

/* Given either a file or symlink, apply the final metadata to it depending on
 * the repository mode. Note that @checksum is assumed to have been validated by
 * the caller.
 */
static gboolean
commit_loose_regfile_object (OstreeRepo *self, const char *checksum, GLnxTmpfile *tmpf, guint32 uid,
                             guint32 gid, guint32 mode, GVariant *xattrs, GCancellable *cancellable,
                             GError **error)
{
  if (self->mode == OSTREE_REPO_MODE_BARE)
    {
      if (TEMP_FAILURE_RETRY (fchown (tmpf->fd, uid, gid)) < 0)
        return glnx_throw_errno_prefix (error, "fchown");

      if (TEMP_FAILURE_RETRY (fchmod (tmpf->fd, mode)) < 0)
        return glnx_throw_errno_prefix (error, "fchmod");

      if (xattrs)
        {
          ot_security_smack_reset_fd (tmpf->fd);
          if (!glnx_fd_set_all_xattrs (tmpf->fd, xattrs, cancellable, error))
            return FALSE;
        }
    }
  else if (self->mode == OSTREE_REPO_MODE_BARE_USER)
    {
      if (!_ostree_write_bareuser_metadata (tmpf->fd, uid, gid, mode, xattrs, error))
        return FALSE;

      /* Note that previously this path added `| 0755` which made every
       * file executable, see
       * https://github.com/ostreedev/ostree/issues/907
       * We then changed it to mask by 0775, but we always need at least read
       * permission when running as non-root, so explicitly mask that in.
       *
       * Again here, symlinks in bare-user are a hairy special case; only do a
       * chmod for a *real* regular file, otherwise we'll take the default 0644.
       */
      if (S_ISREG (mode))
        {
          const mode_t content_mode = (mode & (S_IFREG | 0775)) | S_IRUSR;
          if (!glnx_fchmod (tmpf->fd, content_mode, error))
            return FALSE;
        }
      else
        g_assert (S_ISLNK (mode));
    }
  else if (self->mode == OSTREE_REPO_MODE_BARE_USER_ONLY)
    {
      if (!_ostree_validate_bareuseronly_mode (mode, checksum, error))
        return FALSE;

      if (!glnx_fchmod (tmpf->fd, mode, error))
        return FALSE;
    }

  if (_ostree_repo_mode_is_bare (self->mode))
    {
      /* To satisfy tools such as guile which compare mtimes
       * to determine whether or not source files need to be compiled,
       * set the modification time to OSTREE_TIMESTAMP.
       */
      const struct timespec times[2]
          = { { OSTREE_TIMESTAMP, UTIME_OMIT }, { OSTREE_TIMESTAMP, 0 } };
      if (TEMP_FAILURE_RETRY (futimens (tmpf->fd, times)) < 0)
        return glnx_throw_errno_prefix (error, "futimens");
    }

  /* Ensure that in case of a power cut, these files have the data we
   * want.   See http://lwn.net/Articles/322823/
   */
  if (!self->disable_fsync && self->per_object_fsync)
    {
      if (fsync (tmpf->fd) == -1)
        return glnx_throw_errno_prefix (error, "fsync");
    }

  if (!_ostree_repo_commit_tmpf_final (self, checksum, OSTREE_OBJECT_TYPE_FILE, tmpf, cancellable,
                                       error))
    return FALSE;

  return TRUE;
}

/* This is used by OSTREE_REPO_COMMIT_MODIFIER_FLAGS_GENERATE_SIZES */
typedef struct
{
  OstreeObjectType objtype;
  goffset unpacked;
  goffset archived;
} OstreeContentSizeCacheEntry;

static OstreeContentSizeCacheEntry *
content_size_cache_entry_new (OstreeObjectType objtype, goffset unpacked, goffset archived)
{
  OstreeContentSizeCacheEntry *entry = g_slice_new0 (OstreeContentSizeCacheEntry);

  entry->objtype = objtype;
  entry->unpacked = unpacked;
  entry->archived = archived;

  return entry;
}

static void
content_size_cache_entry_free (gpointer entry)
{
  if (entry)
    g_slice_free (OstreeContentSizeCacheEntry, entry);
}

void
_ostree_repo_setup_generate_sizes (OstreeRepo *self, OstreeRepoCommitModifier *modifier)
{
  if (modifier && modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_GENERATE_SIZES)
    {
      if (ostree_repo_get_mode (self) == OSTREE_REPO_MODE_ARCHIVE)
        {
          self->generate_sizes = TRUE;

          /* Clear any stale data in the object sizes hash table */
          if (self->object_sizes != NULL)
            g_hash_table_remove_all (self->object_sizes);
        }
      else
        g_debug ("Not generating sizes for non-archive repo");
    }
}

static void
repo_ensure_size_entries (OstreeRepo *self)
{
  if (G_UNLIKELY (self->object_sizes == NULL))
    self->object_sizes
        = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, content_size_cache_entry_free);
}

static gboolean
repo_has_size_entry (OstreeRepo *self, OstreeObjectType objtype, const gchar *checksum)
{
  /* Only file, dirtree and dirmeta objects appropriate for size metadata */
  if (objtype > OSTREE_OBJECT_TYPE_DIR_META)
    return TRUE;

  repo_ensure_size_entries (self);
  return (g_hash_table_lookup (self->object_sizes, checksum) != NULL);
}

static void
repo_store_size_entry (OstreeRepo *self, OstreeObjectType objtype, const gchar *checksum,
                       goffset unpacked, goffset archived)
{
  /* Only file, dirtree and dirmeta objects appropriate for size metadata */
  if (objtype > OSTREE_OBJECT_TYPE_DIR_META)
    return;

  repo_ensure_size_entries (self);
  g_hash_table_replace (self->object_sizes, g_strdup (checksum),
                        content_size_cache_entry_new (objtype, unpacked, archived));
}

static int
compare_ascii_checksums_for_sorting (gconstpointer a_pp, gconstpointer b_pp)
{
  char *a = *((char **)a_pp);
  char *b = *((char **)b_pp);

  return strcmp (a, b);
}

/*
 * Create sizes metadata GVariant and add it to the metadata variant given.
 */
static void
add_size_index_to_metadata (OstreeRepo *self, GVariantBuilder *builder)
{
  if (self->object_sizes && g_hash_table_size (self->object_sizes) > 0)
    {
      GVariantBuilder index_builder;
      g_variant_builder_init (&index_builder,
                              G_VARIANT_TYPE ("a" _OSTREE_OBJECT_SIZES_ENTRY_SIGNATURE));

      /* Sort the checksums so we can bsearch if desired */
      g_autoptr (GPtrArray) sorted_keys = g_ptr_array_new ();
      GLNX_HASH_TABLE_FOREACH (self->object_sizes, const char *, e_checksum)
        g_ptr_array_add (sorted_keys, (gpointer)e_checksum);
      g_ptr_array_sort (sorted_keys, compare_ascii_checksums_for_sorting);

      for (guint i = 0; i < sorted_keys->len; i++)
        {
          guint8 csum[OSTREE_SHA256_DIGEST_LEN];
          const char *e_checksum = sorted_keys->pdata[i];
          g_autoptr (GString) buffer = g_string_new (NULL);

          ostree_checksum_inplace_to_bytes (e_checksum, csum);
          g_string_append_len (buffer, (char *)csum, sizeof (csum));

          OstreeContentSizeCacheEntry *e_size
              = g_hash_table_lookup (self->object_sizes, e_checksum);
          _ostree_write_varuint64 (buffer, e_size->archived);
          _ostree_write_varuint64 (buffer, e_size->unpacked);
          g_string_append_c (buffer, (gchar)e_size->objtype);

          g_variant_builder_add (&index_builder, "@ay",
                                 ot_gvariant_new_bytearray ((guint8 *)buffer->str, buffer->len));
        }

      g_variant_builder_add (builder, "{sv}", "ostree.sizes",
                             g_variant_builder_end (&index_builder));

      /* Clear the object sizes hash table for a subsequent commit. */
      g_hash_table_remove_all (self->object_sizes);
    }
}

static gboolean
throw_min_free_space_error (OstreeRepo *self, guint64 bytes_required, GError **error)
{
  const char *err_msg = NULL;
  g_autofree char *err_msg_owned = NULL;

  if (bytes_required > 0)
    {
      g_autofree char *formatted_required = g_format_size (bytes_required);
      err_msg = err_msg_owned
          = g_strdup_printf ("would be exceeded, at least %s requested", formatted_required);
    }
  else
    err_msg = "would be exceeded";
  (void)err_msg_owned; // Conditional ownership

  if (self->min_free_space_mb > 0)
    return glnx_throw (error, "min-free-space-size %" G_GUINT64_FORMAT "MB %s",
                       self->min_free_space_mb, err_msg);
  else
    return glnx_throw (error, "min-free-space-percent '%u%%' %s", self->min_free_space_percent,
                       err_msg);
}

typedef struct
{
  gboolean initialized;
  GLnxTmpfile tmpf;
  char *expected_checksum;
  OtChecksum checksum;
  guint64 content_len;
  guint64 bytes_written;
  guint uid;
  guint gid;
  guint mode;
  GVariant *xattrs;
} OstreeRealRepoBareContent;
G_STATIC_ASSERT (sizeof (OstreeRepoBareContent) >= sizeof (OstreeRealRepoBareContent));

/* Create a tmpfile for writing a bare file.  Currently just used
 * by the static delta code, but will likely later be extended
 * to be used also by the dfd_iter commit path.
 */
gboolean
_ostree_repo_bare_content_open (OstreeRepo *self, const char *expected_checksum,
                                guint64 content_len, guint uid, guint gid, guint mode,
                                GVariant *xattrs, OstreeRepoBareContent *out_regwrite,
                                GCancellable *cancellable, GError **error)
{
  OstreeRealRepoBareContent *real = (OstreeRealRepoBareContent *)out_regwrite;
  g_assert (!real->initialized);
  real->initialized = TRUE;
  g_assert (S_ISREG (mode));
  if (!glnx_open_tmpfile_linkable_at (commit_tmp_dfd (self), ".", O_WRONLY | O_CLOEXEC, &real->tmpf,
                                      error))
    return FALSE;
  ot_checksum_init (&real->checksum);
  real->expected_checksum = g_strdup (expected_checksum);
  real->content_len = content_len;
  real->bytes_written = 0;
  real->uid = uid;
  real->gid = gid;
  real->mode = mode;
  real->xattrs = xattrs ? g_variant_ref (xattrs) : NULL;

  /* Initialize the checksum with the header info */
  g_autoptr (GFileInfo) finfo = _ostree_mode_uidgid_to_gfileinfo (mode, uid, gid);
  g_autoptr (GBytes) header = _ostree_file_header_new (finfo, xattrs);
  ot_checksum_update_bytes (&real->checksum, header);

  return TRUE;
}

gboolean
_ostree_repo_bare_content_write (OstreeRepo *repo, OstreeRepoBareContent *barewrite,
                                 const guint8 *buf, size_t len, GCancellable *cancellable,
                                 GError **error)
{
  OstreeRealRepoBareContent *real = (OstreeRealRepoBareContent *)barewrite;
  g_assert (real->initialized);
  ot_checksum_update (&real->checksum, buf, len);
  if (glnx_loop_write (real->tmpf.fd, buf, len) < 0)
    return glnx_throw_errno_prefix (error, "write");
  return TRUE;
}

gboolean
_ostree_repo_bare_content_commit (OstreeRepo *self, OstreeRepoBareContent *barewrite,
                                  char *checksum_buf, size_t buflen, GCancellable *cancellable,
                                  GError **error)
{
  OstreeRealRepoBareContent *real = (OstreeRealRepoBareContent *)barewrite;
  g_assert (real->initialized);

  if ((self->min_free_space_percent > 0 || self->min_free_space_mb > 0) && self->in_transaction)
    {
      struct stat st_buf;
      if (!glnx_fstat (real->tmpf.fd, &st_buf, error))
        return FALSE;

      g_mutex_lock (&self->txn_lock);
      g_assert_cmpint (self->txn.blocksize, >, 0);

      const fsblkcnt_t object_blocks = (st_buf.st_size / self->txn.blocksize) + 1;
      if (object_blocks > self->txn.max_blocks)
        {
          self->cleanup_stagedir = TRUE;
          g_mutex_unlock (&self->txn_lock);
          return throw_min_free_space_error (self, st_buf.st_size, error);
        }
      /* This is the main bit that needs mutex protection */
      self->txn.max_blocks -= object_blocks;
      g_mutex_unlock (&self->txn_lock);
    }

  ot_checksum_get_hexdigest (&real->checksum, checksum_buf, buflen);

  if (real->expected_checksum
      && !_ostree_compare_object_checksum (OSTREE_OBJECT_TYPE_FILE, real->expected_checksum,
                                           checksum_buf, error))
    return FALSE;

  if (!commit_loose_regfile_object (self, checksum_buf, &real->tmpf, real->uid, real->gid,
                                    real->mode, real->xattrs, cancellable, error))
    return FALSE;

  /* Let's have a guarantee that after commit the object is cleaned up */
  _ostree_repo_bare_content_cleanup (barewrite);
  return TRUE;
}

void
_ostree_repo_bare_content_cleanup (OstreeRepoBareContent *regwrite)
{
  OstreeRealRepoBareContent *real = (OstreeRealRepoBareContent *)regwrite;
  if (!real->initialized)
    return;
  glnx_tmpfile_clear (&real->tmpf);
  ot_checksum_clear (&real->checksum);
  g_clear_pointer (&real->expected_checksum, g_free);
  g_clear_pointer (&real->xattrs, g_variant_unref);
  real->initialized = FALSE;
}

/* Allocate an O_TMPFILE, write everything from @input to it, but
 * not exceeding @length.
 */
static gboolean
create_regular_tmpfile_linkable_with_content (OstreeRepo *self, guint64 length,
                                              GInputStream *original_input, GInputStream *input,
                                              GLnxTmpfile *out_tmpf, GCancellable *cancellable,
                                              GError **error)
{
  g_auto (GLnxTmpfile) tmpf = {
    0,
  };
  if (!glnx_open_tmpfile_linkable_at (commit_tmp_dfd (self), ".", O_WRONLY | O_CLOEXEC, &tmpf,
                                      error))
    return FALSE;

  // Try to do a reflink if possible; if we hit this case we're operating on trusted local input.
  gboolean did_clone = FALSE;
  if (G_IS_FILE_DESCRIPTOR_BASED (original_input))
    {
      int infd = g_file_descriptor_based_get_fd ((GFileDescriptorBased *)original_input);
      if (ioctl (tmpf.fd, FICLONE, infd) == 0)
        {
          did_clone = TRUE;
        }
    }
  else
    {
      if (!glnx_try_fallocate (tmpf.fd, 0, length, error))
        return FALSE;
    }

  /* We used to do a g_output_stream_splice(), but there are two issues with that:
   *  - We want to honor the size provided, to avoid malicious content that says it's
   *    e.g. 10 bytes but is actually gigabytes.
   *  - Due to GLib bugs that pointlessly calls `poll()` on the output fd for every write
   */
  gsize buf_size = MIN (length, 1048576);
  g_autofree gchar *buf = g_malloc (buf_size);
  guint64 remaining = length;
  while (remaining > 0)
    {
      const gssize bytes_read
          = g_input_stream_read (input, buf, MIN (remaining, buf_size), cancellable, error);
      if (bytes_read < 0)
        return FALSE;
      else if (bytes_read == 0)
        return glnx_throw (error,
                           "Unexpected EOF with %" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT
                           " bytes remaining",
                           remaining, length);
      if (!did_clone && glnx_loop_write (tmpf.fd, buf, bytes_read) < 0)
        return glnx_throw_errno_prefix (error, "write");
      remaining -= bytes_read;
    }

  if (!glnx_fchmod (tmpf.fd, 0644, error))
    return FALSE;

  *out_tmpf = tmpf;
  tmpf.initialized = FALSE;
  return TRUE;
}

static gboolean
_check_support_reflink (OstreeRepo *dest, gboolean *supported, GError **error)
{
  /* We have not checked yet if the destination file system supports reflinks, do it here */
  if (g_atomic_int_get (&dest->fs_support_reflink) == 0)
    {
      glnx_autofd int src_fd = -1;
      g_auto (GLnxTmpfile) dest_tmpf = {
        0,
      };

      if (!glnx_openat_rdonly (dest->repo_dir_fd, "config", TRUE, &src_fd, error))
        return FALSE;
      if (!glnx_open_tmpfile_linkable_at (commit_tmp_dfd (dest), ".", O_WRONLY | O_CLOEXEC,
                                          &dest_tmpf, error))
        return FALSE;

      if (ioctl (dest_tmpf.fd, FICLONE, src_fd) == 0)
        g_atomic_int_set (&dest->fs_support_reflink, 1);
      else if (errno
               == EOPNOTSUPP) /* Ignore other kind of errors as they might be temporary failures */
        g_atomic_int_set (&dest->fs_support_reflink, -1);
    }
  *supported = g_atomic_int_get (&dest->fs_support_reflink) >= 0;
  return TRUE;
}

static gboolean
_create_payload_link (OstreeRepo *self, const char *checksum, const char *payload_checksum,
                      GFileInfo *file_info, GCancellable *cancellable, GError **error)
{
  gboolean reflinks_supported = FALSE;

  if (!_check_support_reflink (self, &reflinks_supported, error))
    return FALSE;

  if (!reflinks_supported)
    return TRUE;

  if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_REGULAR
      || !G_IN_SET (self->mode, OSTREE_REPO_MODE_BARE, OSTREE_REPO_MODE_BARE_USER,
                    OSTREE_REPO_MODE_BARE_USER_ONLY))
    return TRUE;

  if (payload_checksum == NULL || g_file_info_get_size (file_info) < self->payload_link_threshold)
    return TRUE;

  char target_buf[_OSTREE_LOOSE_PATH_MAX + _OSTREE_PAYLOAD_LINK_PREFIX_LEN];
  strcpy (target_buf, _OSTREE_PAYLOAD_LINK_PREFIX);
  _ostree_loose_path (target_buf + _OSTREE_PAYLOAD_LINK_PREFIX_LEN, checksum,
                      OSTREE_OBJECT_TYPE_FILE, self->mode);

  if (symlinkat (target_buf, commit_tmp_dfd (self), payload_checksum) < 0)
    {
      if (errno != EEXIST)
        return glnx_throw_errno_prefix (error, "symlinkat");
    }
  else
    {
      g_auto (OtCleanupUnlinkat) tmp_unlinker
          = { commit_tmp_dfd (self), g_strdup (payload_checksum) };
      if (!commit_path_final (self, payload_checksum, OSTREE_OBJECT_TYPE_PAYLOAD_LINK,
                              &tmp_unlinker, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
_import_payload_link (OstreeRepo *dest_repo, OstreeRepo *src_repo, const char *checksum,
                      GCancellable *cancellable, GError **error)
{
  gboolean reflinks_supported = FALSE;
  g_autofree char *payload_checksum = NULL;
  g_autoptr (GInputStream) is = NULL;
  glnx_unref_object OtChecksumInstream *checksum_payload = NULL;
  g_autoptr (GFileInfo) file_info = NULL;

  /* The two repositories are on different devices */
  if (src_repo->device != dest_repo->device)
    return TRUE;

  if (!_check_support_reflink (dest_repo, &reflinks_supported, error))
    return FALSE;

  if (!reflinks_supported)
    return TRUE;

  if (!G_IN_SET (dest_repo->mode, OSTREE_REPO_MODE_BARE, OSTREE_REPO_MODE_BARE_USER,
                 OSTREE_REPO_MODE_BARE_USER_ONLY))
    return TRUE;

  if (!ostree_repo_load_file (src_repo, checksum, &is, &file_info, NULL, cancellable, error))
    return FALSE;

  if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_REGULAR
      || g_file_info_get_size (file_info) < dest_repo->payload_link_threshold)
    return TRUE;

  checksum_payload = ot_checksum_instream_new (is, G_CHECKSUM_SHA256);

  guint64 remaining = g_file_info_get_size (file_info);
  while (remaining)
    {
      char buf[8192];
      gssize ret = g_input_stream_read ((GInputStream *)checksum_payload, buf,
                                        MIN (sizeof (buf), remaining), cancellable, error);
      if (ret < 0)
        return FALSE;
      remaining -= ret;
    }
  payload_checksum = ot_checksum_instream_get_string (checksum_payload);

  return _create_payload_link (dest_repo, checksum, payload_checksum, file_info, cancellable,
                               error);
}

static gboolean
_try_clone_from_payload_link (OstreeRepo *self, OstreeRepo *dest_repo, const char *payload_checksum,
                              GFileInfo *file_info, GLnxTmpfile *tmpf, GCancellable *cancellable,
                              GError **error)
{
  gboolean reflinks_supported = FALSE;
  int dfd_searches[] = { -1, self->objects_dir_fd };
  if (self->commit_stagedir.initialized)
    dfd_searches[0] = self->commit_stagedir.fd;

  /* The two repositories are on different devices */
  if (self->device != dest_repo->device)
    return TRUE;

  if (!_check_support_reflink (dest_repo, &reflinks_supported, error))
    return FALSE;

  if (!reflinks_supported)
    return TRUE;

  for (guint i = 0; i < G_N_ELEMENTS (dfd_searches); i++)
    {
      glnx_autofd int fdf = -1;
      char loose_path_buf[_OSTREE_LOOSE_PATH_MAX];
      char loose_path_target_buf[_OSTREE_LOOSE_PATH_MAX];
      char target_buf[_OSTREE_LOOSE_PATH_MAX + _OSTREE_PAYLOAD_LINK_PREFIX_LEN];
      char target_checksum[OSTREE_SHA256_STRING_LEN + 1];
      int dfd = dfd_searches[i];
      ssize_t size;
      if (dfd == -1)
        continue;

      _ostree_loose_path (loose_path_buf, payload_checksum, OSTREE_OBJECT_TYPE_PAYLOAD_LINK,
                          self->mode);

      size = TEMP_FAILURE_RETRY (readlinkat (dfd, loose_path_buf, target_buf, sizeof (target_buf)));
      if (size < 0)
        {
          if (errno == ENOENT)
            continue;
          return glnx_throw_errno_prefix (error, "readlinkat");
        }

      if (size < OSTREE_SHA256_STRING_LEN + _OSTREE_PAYLOAD_LINK_PREFIX_LEN)
        return glnx_throw (error, "invalid data size for %s", loose_path_buf);

      sprintf (target_checksum, "%.2s%.62s", target_buf + _OSTREE_PAYLOAD_LINK_PREFIX_LEN,
               target_buf + _OSTREE_PAYLOAD_LINK_PREFIX_LEN + 3);

      _ostree_loose_path (loose_path_target_buf, target_checksum, OSTREE_OBJECT_TYPE_FILE,
                          self->mode);
      if (!ot_openat_ignore_enoent (dfd, loose_path_target_buf, &fdf, error))
        return FALSE;

      if (fdf < 0)
        {
          /* If the link is referring to an object that doesn't exist anymore in the repository,
           * just unlink it.  */
          if (!glnx_unlinkat (dfd, loose_path_buf, 0, error))
            return FALSE;
        }
      else
        {
          /* This undoes all of the previous writes; we want to generate reflinked data.   */
          if (ioctl (tmpf->fd, FICLONE, fdf) < 0)
            return glnx_throw_errno_prefix (error, "FICLONE");

          return TRUE;
        }
    }
  if (self->parent_repo)
    return _try_clone_from_payload_link (self->parent_repo, dest_repo, payload_checksum, file_info,
                                         tmpf, cancellable, error);

  return TRUE;
}

/* The main driver for writing a content (regfile or symlink) object.
 * There are a variety of tricky cases here; for example, bare-user
 * repos store symlinks as regular files.  Computing checksums
 * is optional; if @out_csum is `NULL`, we assume the caller already
 * knows the checksum.
 */
static gboolean
write_content_object (OstreeRepo *self, const char *expected_checksum, GInputStream *input,
                      GFileInfo *file_info, GVariant *xattrs, guchar **out_csum,
                      GCancellable *cancellable, GError **error)
{
  g_assert (expected_checksum != NULL || out_csum != NULL);

  GLNX_AUTO_PREFIX_ERROR ("Writing content object", error);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  OstreeRepoMode repo_mode = ostree_repo_get_mode (self);
  if (repo_mode == OSTREE_REPO_MODE_BARE_SPLIT_XATTRS
      && g_getenv ("OSTREE_EXP_WRITE_BARE_SPLIT_XATTRS") == NULL)
    return glnx_throw (error, "Not allowed due to repo mode");

  GInputStream *file_input;                         /* Unowned alias */
  g_autoptr (GInputStream) file_input_owned = NULL; /* We need a temporary for bare-user symlinks */
  glnx_unref_object OtChecksumInstream *checksum_input = NULL;
  glnx_unref_object OtChecksumInstream *checksum_payload_input = NULL;
  const GFileType object_file_type = g_file_info_get_file_type (file_info);
  if (out_csum)
    {
      /* Previously we checksummed the input verbatim; now
       * ostree_repo_write_content() parses without checksumming, then we
       * re-synthesize a header here. The data should be identical; if somehow
       * it's not that's not a serious problem because we're still computing a
       * checksum over the data we actually use.
       */
      gboolean reflinks_supported = FALSE;
      g_autoptr (GBytes) header = _ostree_file_header_new (file_info, xattrs);
      size_t len;
      const guint8 *buf = g_bytes_get_data (header, &len);
      /* Give a null input if there's no content */
      g_autoptr (GInputStream) null_input = NULL;
      if (!input)
        {
          null_input = input = g_memory_input_stream_new_from_data ("", 0, NULL);
          (void)null_input; /* quiet static analysis */
        }
      checksum_input = ot_checksum_instream_new_with_start (input, G_CHECKSUM_SHA256, buf, len);

      if (!_check_support_reflink (self, &reflinks_supported, error))
        return FALSE;

      if (xattrs == NULL
          || !G_IN_SET (self->mode, OSTREE_REPO_MODE_BARE, OSTREE_REPO_MODE_BARE_USER,
                        OSTREE_REPO_MODE_BARE_USER_ONLY)
          || object_file_type != G_FILE_TYPE_REGULAR || !reflinks_supported)
        file_input = (GInputStream *)checksum_input;
      else
        {
          /* The payload checksum-input reads from the full object checksum-input; this
           * means it skips the header.
           */
          checksum_payload_input
              = ot_checksum_instream_new ((GInputStream *)checksum_input, G_CHECKSUM_SHA256);
          file_input = (GInputStream *)checksum_payload_input;
        }
    }
  else
    file_input = input;

  (void)file_input_owned; // Conditionally owned

  gboolean phys_object_is_symlink = FALSE;
  switch (object_file_type)
    {
    case G_FILE_TYPE_REGULAR:
      break;
    case G_FILE_TYPE_SYMBOLIC_LINK:
      if (self->mode == OSTREE_REPO_MODE_BARE || self->mode == OSTREE_REPO_MODE_BARE_USER_ONLY)
        phys_object_is_symlink = TRUE;
      break;
    default:
      return glnx_throw (error, "Unsupported file type %u", object_file_type);
    }

  guint64 size;

  /* For bare-user, convert the symlink target to the input stream */
  if (repo_mode == OSTREE_REPO_MODE_BARE_USER && object_file_type == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      const char *target_str = g_file_info_get_symlink_target (file_info);
      g_autoptr (GBytes) target = g_bytes_new (target_str, strlen (target_str) + 1);

      /* Include the terminating zero so we can e.g. mmap this file */
      file_input = file_input_owned = g_memory_input_stream_new_from_bytes (target);
      size = g_bytes_get_size (target);
    }
  else if (!phys_object_is_symlink)
    size = g_file_info_get_size (file_info);
  else
    size = 0;

  (void)file_input_owned; // Conditionally owned

  /* Free space check; only applies during transactions */
  if ((self->min_free_space_percent > 0 || self->min_free_space_mb > 0) && self->in_transaction)
    {
      g_mutex_lock (&self->txn_lock);
      g_assert_cmpint (self->txn.blocksize, >, 0);
      const fsblkcnt_t object_blocks = (size / self->txn.blocksize) + 1;
      if (object_blocks > self->txn.max_blocks)
        {
          guint64 bytes_required = (guint64)object_blocks * self->txn.blocksize;
          self->cleanup_stagedir = TRUE;
          g_mutex_unlock (&self->txn_lock);
          return throw_min_free_space_error (self, bytes_required, error);
        }
      /* This is the main bit that needs mutex protection */
      self->txn.max_blocks -= object_blocks;
      g_mutex_unlock (&self->txn_lock);
    }

  /* For regular files, we create them with default mode, and only
   * later apply any xattrs and setuid bits.  The rationale here
   * is that an attacker on the network with the ability to MITM
   * could potentially cause the system to make a temporary setuid
   * binary with trailing garbage, creating a window on the local
   * system where a malicious setuid binary exists.
   *
   * We use GLnxTmpfile for regular files, and OtCleanupUnlinkat for symlinks.
   */
  g_auto (OtCleanupUnlinkat) tmp_unlinker = { commit_tmp_dfd (self), NULL };
  g_auto (GLnxTmpfile) tmpf = {
    0,
  };
  goffset unpacked_size = 0;
  /* Is it a symlink physically? */
  if (phys_object_is_symlink)
    {
      /* This will not be hit for bare-user or archive */
      g_assert (self->mode == OSTREE_REPO_MODE_BARE
                || self->mode == OSTREE_REPO_MODE_BARE_USER_ONLY);
      if (!_ostree_make_temporary_symlink_at (commit_tmp_dfd (self),
                                              g_file_info_get_symlink_target (file_info),
                                              &tmp_unlinker.path, cancellable, error))
        return FALSE;
    }
  else if (repo_mode != OSTREE_REPO_MODE_ARCHIVE)
    {
      if (!create_regular_tmpfile_linkable_with_content (self, size, input, file_input, &tmpf,
                                                         cancellable, error))
        return FALSE;
    }
  else
    {
      g_autoptr (GConverter) zlib_compressor = NULL;
      g_autoptr (GOutputStream) compressed_out_stream = NULL;
      g_autoptr (GOutputStream) temp_out = NULL;

      g_assert (repo_mode == OSTREE_REPO_MODE_ARCHIVE);

      if (!glnx_open_tmpfile_linkable_at (commit_tmp_dfd (self), ".", O_WRONLY | O_CLOEXEC, &tmpf,
                                          error))
        return FALSE;
      temp_out = g_unix_output_stream_new (tmpf.fd, FALSE);

      g_autoptr (GBytes) file_meta_header = _ostree_zlib_file_header_new (file_info, xattrs);
      gsize file_meta_len;
      const guint8 *file_meta_buf = g_bytes_get_data (file_meta_header, &file_meta_len);

      {
        gsize bytes_written;
        if (!g_output_stream_write_all (temp_out, file_meta_buf, file_meta_len, &bytes_written,
                                        cancellable, error))
          return FALSE;
      }

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
        {
          zlib_compressor = (GConverter *)g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW,
                                                                 self->zlib_compression_level);
          compressed_out_stream = g_converter_output_stream_new (temp_out, zlib_compressor);
          /* Don't close the base; we'll do that later */
          g_filter_output_stream_set_close_base_stream (
              (GFilterOutputStream *)compressed_out_stream, FALSE);

          if (g_output_stream_splice (compressed_out_stream, file_input, 0, cancellable, error) < 0)
            return FALSE;

          unpacked_size = g_file_info_get_size (file_info);
        }
      else
        {
          /* For a symlink, the size is the length of the target */
          unpacked_size = strlen (g_file_info_get_symlink_target (file_info));
        }

      if (!g_output_stream_flush (temp_out, cancellable, error))
        return FALSE;

      if (!glnx_fchmod (tmpf.fd, 0644, error))
        return FALSE;
    }

  const char *actual_checksum = NULL;
  g_autofree char *actual_payload_checksum = NULL;
  g_autofree char *actual_checksum_owned = NULL;
  if (!checksum_input)
    actual_checksum = expected_checksum;
  else
    {
      actual_checksum = actual_checksum_owned = ot_checksum_instream_get_string (checksum_input);
      if (expected_checksum)
        {
          if (!_ostree_compare_object_checksum (OSTREE_OBJECT_TYPE_FILE, expected_checksum,
                                                actual_checksum, error))
            return FALSE;
        }
      (void)actual_checksum_owned; // Just used to autofree

      if (checksum_payload_input)
        actual_payload_checksum = ot_checksum_instream_get_string (checksum_payload_input);
    }

  g_assert (actual_checksum != NULL); /* Pacify static analysis */

  /* Update size metadata if configured and entry missing */
  if (self->generate_sizes && !repo_has_size_entry (self, OSTREE_OBJECT_TYPE_FILE, actual_checksum))
    {
      struct stat stbuf;

      if (!glnx_fstat (tmpf.fd, &stbuf, error))
        return FALSE;

      repo_store_size_entry (self, OSTREE_OBJECT_TYPE_FILE, actual_checksum, unpacked_size,
                             stbuf.st_size);
    }

  /* See whether or not we have the object, now that we know the
   * checksum.
   */
  gboolean have_obj;
  if (!_ostree_repo_has_loose_object (self, actual_checksum, OSTREE_OBJECT_TYPE_FILE, &have_obj,
                                      cancellable, error))
    return FALSE;
  /* If we already have it, just update the stats. */
  if (have_obj)
    {
      g_mutex_lock (&self->txn_lock);
      self->txn.stats.content_objects_total++;
      g_mutex_unlock (&self->txn_lock);

      if (!_create_payload_link (self, actual_checksum, actual_payload_checksum, file_info,
                                 cancellable, error))
        return FALSE;

      if (out_csum)
        *out_csum = ostree_checksum_to_bytes (actual_checksum);
      /* Note early return */
      return TRUE;
    }

  const guint32 uid = g_file_info_get_attribute_uint32 (file_info, "unix::uid");
  const guint32 gid = g_file_info_get_attribute_uint32 (file_info, "unix::gid");
  const guint32 mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
  /* Is it "physically" a symlink? */
  if (phys_object_is_symlink)
    {
      if (self->mode == OSTREE_REPO_MODE_BARE_USER_ONLY)
        {
          /* We don't store the metadata in bare-user-only, so we're done. */
        }
      else if (self->mode == OSTREE_REPO_MODE_BARE)
        {
          /* Now that we know the checksum is valid, apply uid/gid, mode bits,
           * and extended attributes.
           *
           * Note, this does not apply for bare-user repos, as they store symlinks
           * as regular files.
           */
          if (G_UNLIKELY (
                  fchownat (tmp_unlinker.dfd, tmp_unlinker.path, uid, gid, AT_SYMLINK_NOFOLLOW)
                  == -1))
            return glnx_throw_errno_prefix (error, "fchownat");

          if (xattrs != NULL)
            {
              ot_security_smack_reset_dfd_name (tmp_unlinker.dfd, tmp_unlinker.path);
              if (!glnx_dfd_name_set_all_xattrs (tmp_unlinker.dfd, tmp_unlinker.path, xattrs,
                                                 cancellable, error))
                return FALSE;
            }
        }
      else
        {
          /* We don't do symlinks in archive or bare-user */
          g_assert_not_reached ();
        }

      if (!commit_path_final (self, actual_checksum, OSTREE_OBJECT_TYPE_FILE, &tmp_unlinker,
                              cancellable, error))
        return FALSE;
    }
  else
    {
      /* Check if a file with the same payload is present in the repository,
         and in case try to reflink it */
      if (actual_payload_checksum
          && !_try_clone_from_payload_link (self, self, actual_payload_checksum, file_info, &tmpf,
                                            cancellable, error))
        return FALSE;

      /* This path is for regular files */
      if (!commit_loose_regfile_object (self, actual_checksum, &tmpf, uid, gid, mode, xattrs,
                                        cancellable, error))
        return FALSE;

      if (!_create_payload_link (self, actual_checksum, actual_payload_checksum, file_info,
                                 cancellable, error))
        return FALSE;
    }

  /* Update statistics */
  g_mutex_lock (&self->txn_lock);
  self->txn.stats.content_objects_written++;
  if (g_file_info_has_attribute (file_info, "standard::size"))
    self->txn.stats.content_bytes_written += g_file_info_get_size (file_info);
  self->txn.stats.content_objects_total++;
  g_mutex_unlock (&self->txn_lock);

  if (out_csum)
    {
      g_assert (actual_checksum);
      *out_csum = ostree_checksum_to_bytes (actual_checksum);
    }

  return TRUE;
}

/* A fast path for local commits to `bare` or `bare-user-only`
 * repos - we basically checksum the file and do a renameat()
 * into place.
 *
 * This could be enhanced down the line to handle cases where we have a modified
 * stat struct in place; e.g. for `bare` we could do the `chown`, or chmod etc.,
 * and reset the xattrs.
 *
 * We could also do this for bare-user, would just involve adding the xattr (and
 * potentially deleting other ones...not sure if we'd really want e.g. the
 * security.selinux xattr on setuid binaries and the like to live on).
 */
static gboolean
adopt_and_commit_regfile (OstreeRepo *self, int dfd, const char *name, GFileInfo *finfo,
                          GVariant *xattrs, char *out_checksum_buf, GCancellable *cancellable,
                          GError **error)
{
  g_assert (G_IN_SET (self->mode, OSTREE_REPO_MODE_BARE, OSTREE_REPO_MODE_BARE_USER_ONLY));

  GLNX_AUTO_PREFIX_ERROR ("Commit regfile (adopt)", error);

  g_autoptr (GBytes) header = _ostree_file_header_new (finfo, xattrs);

  g_auto (OtChecksum) hasher = {
    0,
  };
  ot_checksum_init (&hasher);
  ot_checksum_update_bytes (&hasher, header);

  glnx_autofd int fd = -1;
  if (!glnx_openat_rdonly (dfd, name, FALSE, &fd, error))
    return FALSE;

  (void)posix_fadvise (fd, 0, 0, POSIX_FADV_SEQUENTIAL);

  /* See also https://gist.github.com/cgwalters/0df0d15199009664549618c2188581f0
   * and https://github.com/coreutils/coreutils/blob/master/src/ioblksize.h
   * Turns out bigger block size is better; down the line we should use their
   * same heuristics.
   */
  char buf[16 * 1024];
  while (TRUE)
    {
      ssize_t bytes_read = read (fd, buf, sizeof (buf));
      if (bytes_read < 0)
        return glnx_throw_errno_prefix (error, "read");
      if (bytes_read == 0)
        break;

      ot_checksum_update (&hasher, (guint8 *)buf, bytes_read);
    }

  ot_checksum_get_hexdigest (&hasher, out_checksum_buf, OSTREE_SHA256_STRING_LEN + 1);
  const char *checksum = out_checksum_buf;

  /* TODO: dedup this with commit_path_final() */
  char loose_path[_OSTREE_LOOSE_PATH_MAX];
  _ostree_loose_path (loose_path, checksum, OSTREE_OBJECT_TYPE_FILE, self->mode);

  const guint32 src_dev = g_file_info_get_attribute_uint32 (finfo, "unix::device");
  const guint64 src_inode = g_file_info_get_attribute_uint64 (finfo, "unix::inode");

  int dest_dfd = commit_dest_dfd (self);
  if (!_ostree_repo_ensure_loose_objdir_at (dest_dfd, loose_path, cancellable, error))
    return FALSE;

  struct stat dest_stbuf;
  if (!glnx_fstatat_allow_noent (dest_dfd, loose_path, &dest_stbuf, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  /* Is the source actually the same device/inode? This can happen with hardlink
   * checkouts, which is a bit overly conservative for bare-user-only right now.
   * If so, we can't use renameat() since from `man 2 renameat`:
   *
   * "If oldpath and newpath are existing hard links referring to the same file,
   * then rename() does nothing, and returns a success status."
   */
  if (errno != ENOENT && src_dev == dest_stbuf.st_dev && src_inode == dest_stbuf.st_ino)
    {
      if (!glnx_unlinkat (dfd, name, 0, error))
        return FALSE;

      /* Early return */
      return TRUE;
    }

  /* For bare-user-only we need to canonicalize perms */
  if (self->mode == OSTREE_REPO_MODE_BARE_USER_ONLY)
    {
      const guint32 src_mode = g_file_info_get_attribute_uint32 (finfo, "unix::mode");
      if (fchmod (fd, src_mode & 0755) < 0)
        return glnx_throw_errno_prefix (error, "fchmod");
    }
  if (renameat (dfd, name, dest_dfd, loose_path) == -1)
    {
      if (errno != EEXIST)
        return glnx_throw_errno_prefix (error, "Storing file '%s'", name);
      /* We took ownership here, so delete it */
      if (!glnx_unlinkat (dfd, name, 0, error))
        return FALSE;
    }

  return TRUE;
}

/* Main driver for writing a metadata (non-content) object. */
static gboolean
write_metadata_object (OstreeRepo *self, OstreeObjectType objtype, const char *expected_checksum,
                       GBytes *buf, guchar **out_csum, GCancellable *cancellable, GError **error)
{
  g_assert (expected_checksum != NULL || out_csum != NULL);

  GLNX_AUTO_PREFIX_ERROR ("Writing metadata object", error);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  /* In the metadata case, we're not streaming, so we don't bother creating a
   * tempfile until we compute the checksum. Some metadata like dirmeta is
   * commonly duplicated, and computing the checksum is going to be cheaper than
   * making a tempfile.
   *
   * However, tombstone commit types don't make sense to checksum, because for
   * historical reasons we used ostree_repo_write_metadata_trusted() with the
   * *original* sha256 to say what commit was being killed.
   */
  const gboolean is_tombstone = (objtype == OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT);
  char actual_checksum[OSTREE_SHA256_STRING_LEN + 1];
  if (is_tombstone)
    {
      g_assert (expected_checksum != NULL);
      memcpy (actual_checksum, expected_checksum, sizeof (actual_checksum));
    }
  else
    {
      g_auto (OtChecksum) checksum = {
        0,
      };
      ot_checksum_init (&checksum);
      gsize len;
      const guint8 *bufdata = g_bytes_get_data (buf, &len);
      ot_checksum_update (&checksum, bufdata, len);
      ot_checksum_get_hexdigest (&checksum, actual_checksum, sizeof (actual_checksum));
      gboolean have_obj;
      if (!_ostree_repo_has_loose_object (self, actual_checksum, objtype, &have_obj, cancellable,
                                          error))
        return FALSE;
      /* If we already have the object, we just need to update the tried-to-commit
       * stat for metadata and be done here.
       */
      if (have_obj)
        {
          /* Update size metadata if needed */
          if (self->generate_sizes && !repo_has_size_entry (self, objtype, actual_checksum))
            repo_store_size_entry (self, objtype, actual_checksum, len, len);

          g_mutex_lock (&self->txn_lock);
          self->txn.stats.metadata_objects_total++;
          g_mutex_unlock (&self->txn_lock);

          if (out_csum)
            *out_csum = ostree_checksum_to_bytes (actual_checksum);
          /* Note early return */
          return TRUE;
        }

      if (expected_checksum)
        {
          if (!_ostree_compare_object_checksum (objtype, expected_checksum, actual_checksum, error))
            return FALSE;
        }
    }

  /* Ok, checksum is known, let's get the data */
  gsize len;
  const guint8 *bufp = g_bytes_get_data (buf, &len);

  /* Update size metadata if needed */
  if (self->generate_sizes && !repo_has_size_entry (self, objtype, actual_checksum))
    repo_store_size_entry (self, objtype, actual_checksum, len, len);

  /* Write the metadata to a temporary file */
  g_auto (GLnxTmpfile) tmpf = {
    0,
  };
  if (!glnx_open_tmpfile_linkable_at (commit_tmp_dfd (self), ".", O_WRONLY | O_CLOEXEC, &tmpf,
                                      error))
    return FALSE;
  if (!glnx_try_fallocate (tmpf.fd, 0, len, error))
    return FALSE;
  if (glnx_loop_write (tmpf.fd, bufp, len) < 0)
    return glnx_throw_errno_prefix (error, "write()");
  if (!glnx_fchmod (tmpf.fd, 0644, error))
    return FALSE;

  /* And commit it into place */
  if (!_ostree_repo_commit_tmpf_final (self, actual_checksum, objtype, &tmpf, cancellable, error))
    return FALSE;

  if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
    {
      GError *local_error = NULL;
      /* If we are writing a commit, be sure there is no tombstone for it.
         We may have deleted the commit and now we are trying to pull it again.  */
      if (!ostree_repo_delete_object (self, OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT, actual_checksum,
                                      cancellable, &local_error))
        {
          if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            g_clear_error (&local_error);
          else
            {
              g_propagate_error (error, local_error);
              return FALSE;
            }
        }
    }

  /* Update the stats, note we both wrote one and add to total */
  g_mutex_lock (&self->txn_lock);
  self->txn.stats.metadata_objects_written++;
  self->txn.stats.metadata_objects_total++;
  g_mutex_unlock (&self->txn_lock);

  if (out_csum)
    *out_csum = ostree_checksum_to_bytes (actual_checksum);
  return TRUE;
}

/* Look in a single subdirectory of objects/, building up the
 * (device,inode) → checksum map.
 */
static gboolean
scan_one_loose_devino (OstreeRepo *self, int object_dir_fd, GHashTable *devino_cache,
                       GCancellable *cancellable, GError **error)
{
  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };
  if (!glnx_dirfd_iterator_init_at (object_dir_fd, ".", FALSE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent;
      g_auto (GLnxDirFdIterator) child_dfd_iter = {
        0,
      };

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      /* All object directories only have two character entries */
      if (strlen (dent->d_name) != 2)
        continue;

      if (!glnx_dirfd_iterator_init_at (dfd_iter.fd, dent->d_name, FALSE, &child_dfd_iter, error))
        return FALSE;

      while (TRUE)
        {
          struct dirent *child_dent;

          if (!glnx_dirfd_iterator_next_dent (&child_dfd_iter, &child_dent, cancellable, error))
            return FALSE;
          if (child_dent == NULL)
            break;

          const char *name = child_dent->d_name;

          gboolean skip;
          switch (self->mode)
            {
            case OSTREE_REPO_MODE_ARCHIVE:
            case OSTREE_REPO_MODE_BARE:
            case OSTREE_REPO_MODE_BARE_USER:
            case OSTREE_REPO_MODE_BARE_USER_ONLY:
              skip = !g_str_has_suffix (name, ".file");
              break;
            default:
              g_assert_not_reached ();
            }
          if (skip)
            continue;

          const char *dot = strrchr (name, '.');
          g_assert (dot);

          /* Skip anything that doesn't look like a 64 character checksum */
          if ((dot - name) != 62)
            continue;

          struct stat stbuf;
          if (!glnx_fstatat (child_dfd_iter.fd, child_dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW,
                             error))
            return FALSE;

          OstreeDevIno *key = g_new (OstreeDevIno, 1);
          key->dev = stbuf.st_dev;
          key->ino = stbuf.st_ino;
          memcpy (key->checksum, dent->d_name, 2);
          memcpy (key->checksum + 2, name, 62);
          key->checksum[sizeof (key->checksum) - 1] = '\0';
          g_hash_table_add (devino_cache, key);
        }
    }

  return TRUE;
}

/* Used by ostree_repo_scan_hardlinks(); see that function for more information. */
static gboolean
scan_loose_devino (OstreeRepo *self, GHashTable *devino_cache, GCancellable *cancellable,
                   GError **error)
{
  if (self->parent_repo)
    {
      if (!scan_loose_devino (self->parent_repo, devino_cache, cancellable, error))
        return FALSE;
    }

  if (self->mode == OSTREE_REPO_MODE_ARCHIVE && self->uncompressed_objects_dir_fd != -1)
    {
      if (!scan_one_loose_devino (self, self->uncompressed_objects_dir_fd, devino_cache,
                                  cancellable, error))
        return FALSE;
    }

  if (!scan_one_loose_devino (self, self->objects_dir_fd, devino_cache, cancellable, error))
    return FALSE;

  return TRUE;
}

/* Loook up a (device,inode) pair in our cache, and see if it maps to a known
 * checksum. */
static const char *
devino_cache_lookup (OstreeRepo *self, OstreeRepoCommitModifier *modifier, guint32 device,
                     guint64 inode)
{
  OstreeDevIno dev_ino_key;
  OstreeDevIno *dev_ino_val;
  GHashTable *cache;

  if (self->loose_object_devino_hash)
    cache = self->loose_object_devino_hash;
  else if (modifier && modifier->devino_cache)
    cache = modifier->devino_cache;
  else
    return NULL;

  dev_ino_key.dev = device;
  dev_ino_key.ino = inode;
  dev_ino_val = g_hash_table_lookup (cache, &dev_ino_key);
  if (!dev_ino_val)
    return NULL;
  return dev_ino_val->checksum;
}

/**
 * ostree_repo_scan_hardlinks:
 * @self: An #OstreeRepo
 * @cancellable: Cancellable
 * @error: Error
 *
 * This function is deprecated in favor of using ostree_repo_devino_cache_new(),
 * which allows a precise mapping to be built up between hardlink checkout files
 * and their checksums between `ostree_repo_checkout_at()` and
 * `ostree_repo_write_directory_to_mtree()`.
 *
 * When invoking ostree_repo_write_directory_to_mtree(), it has to compute the
 * checksum of all files. If your commit contains hardlinks from a checkout,
 * this functions builds a mapping of device numbers and inodes to their
 * checksum.
 *
 * There is an upfront cost to creating this mapping, as this will scan the
 * entire objects directory. If your commit is composed of mostly hardlinks to
 * existing ostree objects, then this will speed up considerably, so call it
 * before you call ostree_repo_write_directory_to_mtree() or similar.  However,
 * ostree_repo_devino_cache_new() is better as it avoids scanning all objects.
 *
 * Multithreading: This function is *not* MT safe.
 */
gboolean
ostree_repo_scan_hardlinks (OstreeRepo *self, GCancellable *cancellable, GError **error)
{
  g_assert (self != NULL);
  g_assert (OSTREE_IS_REPO (self));

  if (!self->in_transaction)
    return glnx_throw (error, "Failed to scan hardlinks, not in a transaction");

  if (!self->loose_object_devino_hash)
    self->loose_object_devino_hash = (GHashTable *)ostree_repo_devino_cache_new ();
  g_hash_table_remove_all (self->loose_object_devino_hash);
  return scan_loose_devino (self, self->loose_object_devino_hash, cancellable, error);
}

/**
 * ostree_repo_prepare_transaction:
 * @self: An #OstreeRepo
 * @out_transaction_resume: (allow-none) (out): Whether this transaction
 * is resuming from a previous one.  This is a legacy state, now OSTree
 * pulls use per-commit `state/.commitpartial` files.
 * @cancellable: Cancellable
 * @error: Error
 *
 * Starts or resumes a transaction. In order to write to a repo, you
 * need to start a transaction. You can complete the transaction with
 * ostree_repo_commit_transaction(), or abort the transaction with
 * ostree_repo_abort_transaction().
 *
 * Currently, transactions may result in partial commits or data in the target
 * repository if interrupted during ostree_repo_commit_transaction(), and
 * further writing refs is also not currently atomic.
 *
 * There can be at most one transaction active on a repo at a time per instance
 * of `OstreeRepo`; however, it is safe to have multiple threads writing objects
 * on a single `OstreeRepo` instance as long as their lifetime is bounded by the
 * transaction.
 *
 * Locking: Acquires a `shared` lock; release via commit or abort
 * Multithreading: This function is *not* MT safe; only one transaction can be
 * active at a time.
 */
gboolean
ostree_repo_prepare_transaction (OstreeRepo *self, gboolean *out_transaction_resume,
                                 GCancellable *cancellable, GError **error)
{
  g_assert (self != NULL);
  g_assert (OSTREE_IS_REPO (self));

  if (self->in_transaction)
    return glnx_throw (error, "Failed to prepare transaction, another transaction is in progress");

  g_debug ("Preparing transaction in repository %p", self);

  /* Set up to abort the transaction if we return early from this function.
   * We can't call _ostree_repo_auto_transaction_start() here, because that
   * would be a circular dependency; use the lower-level version instead. */
  g_autoptr (OstreeRepoAutoTransaction) txn = _ostree_repo_auto_transaction_new (self);
  g_assert (txn != NULL);

  memset (&self->txn.stats, 0, sizeof (OstreeRepoTransactionStats));

  self->txn_locked = ostree_repo_lock_push (self, OSTREE_REPO_LOCK_SHARED, cancellable, error);
  if (!self->txn_locked)
    return FALSE;

  self->in_transaction = TRUE;
  self->cleanup_stagedir = FALSE;

  struct statvfs stvfsbuf;
  if (TEMP_FAILURE_RETRY (fstatvfs (self->repo_dir_fd, &stvfsbuf)) < 0)
    return glnx_throw_errno_prefix (error, "fstatvfs");

  g_mutex_lock (&self->txn_lock);
  self->txn.blocksize = stvfsbuf.f_bsize;
  guint64 reserved_bytes = 0;
  if (!ostree_repo_get_min_free_space_bytes (self, &reserved_bytes, error))
    {
      g_mutex_unlock (&self->txn_lock);
      return FALSE;
    }
  self->reserved_blocks = reserved_bytes / self->txn.blocksize;

  /* Use the appropriate free block count if we're unprivileged */
  guint64 bfree = (getuid () != 0 ? stvfsbuf.f_bavail : stvfsbuf.f_bfree);
  if (bfree > self->reserved_blocks)
    self->txn.max_blocks = bfree - self->reserved_blocks;
  else
    {
      self->txn.max_blocks = 0;
      /* Don't throw_min_free_space_error here; reason being that
       * this transaction could be just committing metadata objects
       * which are relatively small in size and we do not really
       * want to block them via min-free-space-* value. Metadata
       * objects helps in housekeeping and hence should be kept
       * out of the strict min-free-space values.
       *
       * The main drivers for writing content objects will always honor
       * the min-free-space value and throw_min_free_space_error in
       * case of overstepping the number of reserved blocks.
       */
    }
  g_mutex_unlock (&self->txn_lock);

  gboolean ret_transaction_resume = FALSE;
  if (!_ostree_repo_allocate_tmpdir (self->tmp_dir_fd, self->stagedir_prefix,
                                     &self->commit_stagedir, &self->commit_stagedir_lock,
                                     &ret_transaction_resume, cancellable, error))
    return FALSE;

  /* Success: do not abort the transaction when returning. */
  g_clear_object (&txn->repo);
  (void)txn;

  if (out_transaction_resume)
    *out_transaction_resume = ret_transaction_resume;
  return TRUE;
}

/* Synchronize the directories holding the objects */
static gboolean
fsync_object_dirs (OstreeRepo *self, GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("fsync objdirs", error);
  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };

  if (self->disable_fsync)
    return TRUE; /* No fsync?  Nothing to do then. */

  if (!glnx_dirfd_iterator_init_at (self->objects_dir_fd, ".", FALSE, &dfd_iter, error))
    return FALSE;
  while (TRUE)
    {
      struct dirent *dent;
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;
      if (dent->d_type != DT_DIR)
        continue;
      /* All object directories only have two character entries */
      if (strlen (dent->d_name) != 2)
        continue;

      glnx_autofd int target_dir_fd = -1;
      if (!glnx_opendirat (self->objects_dir_fd, dent->d_name, FALSE, &target_dir_fd, error))
        return FALSE;
      /* This synchronizes the directory to ensure all the objects we wrote
       * are there.  We need to do this before removing the .commitpartial
       * stamp (or have a ref point to the commit).
       */
      if (fsync (target_dir_fd) == -1)
        return glnx_throw_errno_prefix (error, "fsync");
    }

  /* In case we created any loose object subdirs, make sure they are on disk */
  if (fsync (self->objects_dir_fd) == -1)
    return glnx_throw_errno_prefix (error, "fsync");

  return TRUE;
}

/* Called for commit, to iterate over the "staging" directory and rename all the
 * objects into the primary objects/ location. Notably this is called only after
 * syncfs() has potentially been invoked to ensure that all objects have been
 * written to disk.  In the future we may enhance this; see
 * https://github.com/ostreedev/ostree/issues/1184
 */
static gboolean
rename_pending_loose_objects (OstreeRepo *self, GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("rename pending", error);
  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };

  if (!glnx_dirfd_iterator_init_at (self->commit_stagedir.fd, ".", FALSE, &dfd_iter, error))
    return FALSE;

  /* Iterate over the outer checksum dir */
  while (TRUE)
    {
      struct dirent *dent;
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (dent->d_type != DT_DIR)
        continue;

      /* All object directories only have two character entries */
      if (strlen (dent->d_name) != 2)
        continue;

      g_auto (GLnxDirFdIterator) child_dfd_iter = {
        0,
      };
      if (!glnx_dirfd_iterator_init_at (dfd_iter.fd, dent->d_name, FALSE, &child_dfd_iter, error))
        return FALSE;

      char loose_objpath[_OSTREE_LOOSE_PATH_MAX];
      loose_objpath[0] = dent->d_name[0];
      loose_objpath[1] = dent->d_name[1];
      loose_objpath[2] = '/';

      /* Iterate over inner checksum dir */
      while (TRUE)
        {
          struct dirent *child_dent;

          if (!glnx_dirfd_iterator_next_dent (&child_dfd_iter, &child_dent, cancellable, error))
            return FALSE;
          if (child_dent == NULL)
            break;

          g_strlcpy (loose_objpath + 3, child_dent->d_name, sizeof (loose_objpath) - 3);

          if (!_ostree_repo_ensure_loose_objdir_at (self->objects_dir_fd, loose_objpath,
                                                    cancellable, error))
            return FALSE;

          if (!glnx_renameat (child_dfd_iter.fd, loose_objpath + 3, self->objects_dir_fd,
                              loose_objpath, error))
            return FALSE;
        }
    }

  return TRUE;
}

/* Try to lock and delete a transaction stage directory created by
 * ostree_repo_prepare_transaction().
 */
static gboolean
cleanup_txn_dir (OstreeRepo *self, int dfd, const char *path, GCancellable *cancellable,
                 GError **error)
{
  const char *errprefix = glnx_strjoina ("Cleaning up txn dir ", path);
  GLNX_AUTO_PREFIX_ERROR (errprefix, error);

  g_auto (GLnxLockFile) lockfile = {
    0,
  };
  gboolean did_lock;

  /* Try to lock, but if we don't get it, move on */
  if (!_ostree_repo_try_lock_tmpdir (dfd, path, &lockfile, &did_lock, error))
    return FALSE;
  if (!did_lock)
    return TRUE; /* Note early return */

  /* If however this is the staging directory for the *current*
   * boot, then don't delete it now - we may end up reusing it, as
   * is the point. Delete *only if* we have hit min-free-space* checks
   * as we don't want to hold onto caches in that case.
   */
  if (g_str_has_prefix (path, self->stagedir_prefix) && !self->cleanup_stagedir)
    return TRUE; /* Note early return */

  /* But, crucially we can now clean up staging directories
   * from *other* boots.
   */
  if (!glnx_shutil_rm_rf_at (dfd, path, cancellable, error))
    return glnx_prefix_error (error, "Removing %s", path);

  return TRUE;
}

/* Look in repo/tmp and delete files that are older than a day (by default).
 * This used to be primarily used by the libsoup fetcher which stored partially
 * written objects.  In practice now that that isn't done anymore, we should
 * use different logic here.  Some more information in
 * https://github.com/ostreedev/ostree/issues/713
 */
static gboolean
cleanup_tmpdir (OstreeRepo *self, GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("tmpdir cleanup", error);
  const guint64 curtime_secs = g_get_real_time () / 1000000;

  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };
  if (!glnx_dirfd_iterator_init_at (self->tmp_dir_fd, ".", TRUE, &dfd_iter, error))
    return FALSE;

  while (TRUE)
    {
      struct dirent *dent;
      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      /* Special case this; we create it when opening, and don't want
       * to blow it away.
       */
      if (strcmp (dent->d_name, "cache") == 0)
        continue;

      struct stat stbuf;
      if (!glnx_fstatat_allow_noent (dfd_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW, error))
        return FALSE;
      if (errno == ENOENT) /* Did another cleanup win? */
        continue;

      /* Handle transaction tmpdirs */
      if (_ostree_repo_has_staging_prefix (dent->d_name) && S_ISDIR (stbuf.st_mode))
        {
          if (!cleanup_txn_dir (self, dfd_iter.fd, dent->d_name, cancellable, error))
            return FALSE;
          continue; /* We've handled this, move on */
        }

      /* At this point we're looking at an unknown-origin file or directory in
       * the tmpdir. This could be something like a temporary checkout dir (used
       * by rpm-ostree), or (from older versions of libostree) a tempfile if we
       * don't have O_TMPFILE for commits.
       */

      /* Ignore files from the future */
      if (stbuf.st_mtime > curtime_secs)
        continue;

      /* We're pruning content based on the expiry, which
       * defaults to a day.  That's what we were doing before we
       * had locking...but in future we can be smarter here.
       */
      guint64 delta = curtime_secs - stbuf.st_mtime;
      if (delta > self->tmp_expiry_seconds)
        {
          if (!glnx_shutil_rm_rf_at (dfd_iter.fd, dent->d_name, cancellable, error))
            return glnx_prefix_error (error, "Removing %s", dent->d_name);
        }
    }

  return TRUE;
}

static void
ensure_txn_refs (OstreeRepo *self)
{
  if (self->txn.refs == NULL)
    self->txn.refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  if (self->txn.collection_refs == NULL)
    self->txn.collection_refs
        = g_hash_table_new_full (ostree_collection_ref_hash, ostree_collection_ref_equal,
                                 (GDestroyNotify)ostree_collection_ref_free, g_free);
}

/**
 * ostree_repo_mark_commit_partial_reason:
 * @self: Repo
 * @checksum: Commit SHA-256
 * @is_partial: Whether or not this commit is partial
 * @in_state: Reason bitmask for partial commit
 * @error: Error
 *
 * Allows the setting of a reason code for a partial commit. Presently
 * it only supports setting reason bitmask to
 * OSTREE_REPO_COMMIT_STATE_FSCK_PARTIAL, or
 * OSTREE_REPO_COMMIT_STATE_NORMAL.  This will allow successive ostree
 * fsck operations to exit properly with an error code if the
 * repository has been truncated as a result of fsck trying to repair
 * it.
 *
 * Since: 2019.4
 */
gboolean
ostree_repo_mark_commit_partial_reason (OstreeRepo *self, const char *checksum, gboolean is_partial,
                                        OstreeRepoCommitState in_state, GError **error)
{
  g_autofree char *commitpartial_path = _ostree_get_commitpartial_path (checksum);
  if (is_partial)
    {
      glnx_autofd int fd = openat (self->repo_dir_fd, commitpartial_path,
                                   O_EXCL | O_CREAT | O_WRONLY | O_CLOEXEC | O_NOCTTY, 0644);
      if (fd == -1)
        {
          if (errno != EEXIST)
            return glnx_throw_errno_prefix (error, "open(%s)", commitpartial_path);
        }
      else
        {
          if (in_state & OSTREE_REPO_COMMIT_STATE_FSCK_PARTIAL)
            if (glnx_loop_write (fd, "f", 1) < 0)
              return glnx_throw_errno_prefix (error, "write(%s)", commitpartial_path);
        }
    }
  else
    {
      if (!ot_ensure_unlinked_at (self->repo_dir_fd, commitpartial_path, 0))
        return FALSE;
    }

  return TRUE;
}

/**
 * ostree_repo_mark_commit_partial:
 * @self: Repo
 * @checksum: Commit SHA-256
 * @is_partial: Whether or not this commit is partial
 * @error: Error
 *
 * Commits in the "partial" state do not have all their child objects
 * written.  This occurs in various situations, such as during a pull,
 * but also if a "subpath" pull is used, as well as "commit only"
 * pulls.
 *
 * This function is used by ostree_repo_pull_with_options(); you
 * should use this if you are implementing a different type of transport.
 *
 * Since: 2017.15
 */
gboolean
ostree_repo_mark_commit_partial (OstreeRepo *self, const char *checksum, gboolean is_partial,
                                 GError **error)
{
  return ostree_repo_mark_commit_partial_reason (self, checksum, is_partial,
                                                 OSTREE_REPO_COMMIT_STATE_NORMAL, error);
}

/**
 * ostree_repo_transaction_set_refspec:
 * @self: An #OstreeRepo
 * @refspec: The refspec to write
 * @checksum: (nullable): The checksum to point it to
 *
 * Like ostree_repo_transaction_set_ref(), but takes concatenated
 * @refspec format as input instead of separate remote and name
 * arguments.
 *
 * Multithreading: Since v2017.15 this function is MT safe.
 */
void
ostree_repo_transaction_set_refspec (OstreeRepo *self, const char *refspec, const char *checksum)
{
  g_assert (self != NULL);
  g_assert (OSTREE_IS_REPO (self));
  g_assert (self->in_transaction == TRUE);

  g_mutex_lock (&self->txn_lock);
  ensure_txn_refs (self);
  g_hash_table_replace (self->txn.refs, g_strdup (refspec), g_strdup (checksum));
  g_mutex_unlock (&self->txn_lock);
}

/**
 * ostree_repo_transaction_set_ref:
 * @self: An #OstreeRepo
 * @remote: (allow-none): A remote for the ref
 * @ref: The ref to write
 * @checksum: (nullable): The checksum to point it to
 *
 * If @checksum is not %NULL, then record it as the target of ref named
 * @ref; if @remote is provided, the ref will appear to originate from that
 * remote.
 *
 * Otherwise, if @checksum is %NULL, then record that the ref should
 * be deleted.
 *
 * The change will be written when the transaction is completed with
 * ostree_repo_commit_transaction(); that function takes care of writing all of
 * the objects (such as the commit referred to by @checksum) before updating the
 * refs. If the transaction is instead aborted with
 * ostree_repo_abort_transaction(), no changes to the ref will be made to the
 * repository.
 *
 * Note however that currently writing *multiple* refs is not truly atomic; if
 * the process or system is terminated during
 * ostree_repo_commit_transaction(), it is possible that just some of the refs
 * will have been updated. Your application should take care to handle this
 * case.
 *
 * Multithreading: Since v2017.15 this function is MT safe.
 */
void
ostree_repo_transaction_set_ref (OstreeRepo *self, const char *remote, const char *ref,
                                 const char *checksum)
{
  g_assert (self != NULL);
  g_assert (OSTREE_IS_REPO (self));
  g_assert (self->in_transaction == TRUE);

  char *refspec;
  if (remote)
    refspec = g_strdup_printf ("%s:%s", remote, ref);
  else
    refspec = g_strdup (ref);

  g_mutex_lock (&self->txn_lock);
  ensure_txn_refs (self);
  g_hash_table_replace (self->txn.refs, refspec, g_strdup (checksum));
  g_mutex_unlock (&self->txn_lock);
}

/**
 * ostree_repo_transaction_set_collection_ref:
 * @self: An #OstreeRepo
 * @ref: The collection–ref to write
 * @checksum: (nullable): The checksum to point it to
 *
 * If @checksum is not %NULL, then record it as the target of local ref named
 * @ref.
 *
 * Otherwise, if @checksum is %NULL, then record that the ref should
 * be deleted.
 *
 * The change will not be written out immediately, but when the transaction
 * is completed with ostree_repo_commit_transaction(). If the transaction
 * is instead aborted with ostree_repo_abort_transaction(), no changes will
 * be made to the repository.
 *
 * Multithreading: Since v2017.15 this function is MT safe.
 *
 * Since: 2018.6
 */
void
ostree_repo_transaction_set_collection_ref (OstreeRepo *self, const OstreeCollectionRef *ref,
                                            const char *checksum)
{
  g_assert (self != NULL);
  g_assert (OSTREE_IS_REPO (self));
  g_assert (self->in_transaction == TRUE);
  g_assert (ref != NULL);

  // TODO(lucab): introduce a method with error-returning in order to deprecate
  // this one, because it can silently fail.
  g_return_if_fail (checksum == NULL || ostree_validate_checksum_string (checksum, NULL));

  g_mutex_lock (&self->txn_lock);
  ensure_txn_refs (self);
  g_hash_table_replace (self->txn.collection_refs, ostree_collection_ref_dup (ref),
                        g_strdup (checksum));
  g_mutex_unlock (&self->txn_lock);
}

/**
 * ostree_repo_set_ref_immediate:
 * @self: An #OstreeRepo
 * @remote: (allow-none): A remote for the ref
 * @ref: The ref to write
 * @checksum: (allow-none): The checksum to point it to, or %NULL to unset
 * @cancellable: GCancellable
 * @error: GError
 *
 * This is like ostree_repo_transaction_set_ref(), except it may be
 * invoked outside of a transaction.  This is presently safe for the
 * case where we're creating or overwriting an existing ref.
 *
 * Multithreading: This function is MT safe.
 */
gboolean
ostree_repo_set_ref_immediate (OstreeRepo *self, const char *remote, const char *ref,
                               const char *checksum, GCancellable *cancellable, GError **error)
{
  const OstreeCollectionRef _ref = { NULL, (gchar *)ref };
  return _ostree_repo_write_ref (self, remote, &_ref, checksum, NULL, cancellable, error);
}

/**
 * ostree_repo_set_alias_ref_immediate:
 * @self: An #OstreeRepo
 * @remote: (allow-none): A remote for the ref
 * @ref: The ref to write
 * @target: (allow-none): The ref target to point it to, or %NULL to unset
 * @cancellable: GCancellable
 * @error: GError
 *
 * Like ostree_repo_set_ref_immediate(), but creates an alias.
 *
 * Since: 2017.10
 */
gboolean
ostree_repo_set_alias_ref_immediate (OstreeRepo *self, const char *remote, const char *ref,
                                     const char *target, GCancellable *cancellable, GError **error)
{
  const OstreeCollectionRef _ref = { NULL, (gchar *)ref };
  return _ostree_repo_write_ref (self, remote, &_ref, NULL, target, cancellable, error);
}

/**
 * ostree_repo_set_collection_ref_immediate:
 * @self: An #OstreeRepo
 * @ref: The collection–ref to write
 * @checksum: (nullable): The checksum to point it to, or %NULL to unset
 * @cancellable: GCancellable
 * @error: GError
 *
 * This is like ostree_repo_transaction_set_collection_ref(), except it may be
 * invoked outside of a transaction.  This is presently safe for the
 * case where we're creating or overwriting an existing ref.
 *
 * Returns: %TRUE on success, %FALSE otherwise
 * Since: 2018.6
 */
gboolean
ostree_repo_set_collection_ref_immediate (OstreeRepo *self, const OstreeCollectionRef *ref,
                                          const char *checksum, GCancellable *cancellable,
                                          GError **error)
{
  g_assert (self != NULL);
  g_assert (OSTREE_IS_REPO (self));
  g_assert (ref != NULL);

  /* If a checksum was provided, validate it upfront. */
  if (checksum != NULL && !ostree_validate_checksum_string (checksum, error))
    return FALSE;

  return _ostree_repo_write_ref (self, NULL, ref, checksum, NULL, cancellable, error);
}

/**
 * ostree_repo_commit_transaction:
 * @self: An #OstreeRepo
 * @out_stats: (allow-none) (out): A set of statistics of things
 * that happened during this transaction.
 * @cancellable: Cancellable
 * @error: Error
 *
 * Complete the transaction. Any refs set with
 * ostree_repo_transaction_set_ref() or
 * ostree_repo_transaction_set_refspec() will be written out.
 *
 * Note that if multiple threads are performing writes, all such threads must
 * have terminated before this function is invoked.
 *
 * Locking: Releases `shared` lock acquired by `ostree_repo_prepare_transaction()`
 * Multithreading: This function is *not* MT safe; only one transaction can be
 * active at a time.
 */
gboolean
ostree_repo_commit_transaction (OstreeRepo *self, OstreeRepoTransactionStats *out_stats,
                                GCancellable *cancellable, GError **error)
{
  g_assert (self != NULL);
  g_assert (OSTREE_IS_REPO (self));

  if (!self->in_transaction)
    return glnx_throw (error, "Failed to commit transaction, no transaction in progress");

  g_debug ("Committing transaction in repository %p", self);

  if ((self->test_error_flags & OSTREE_REPO_TEST_ERROR_PRE_COMMIT) > 0)
    return glnx_throw (error, "OSTREE_REPO_TEST_ERROR_PRE_COMMIT specified");

  /* FIXME: Added OSTREE_SUPPRESS_SYNCFS since valgrind in el7 doesn't know
   * about `syncfs`...we should delete this later.
   */
  if (!self->disable_fsync && g_getenv ("OSTREE_SUPPRESS_SYNCFS") == NULL)
    {
      if (syncfs (self->tmp_dir_fd) < 0)
        return glnx_throw_errno_prefix (error, "syncfs");
    }

  if (!rename_pending_loose_objects (self, cancellable, error))
    return FALSE;

  if (!fsync_object_dirs (self, cancellable, error))
    return FALSE;

  g_debug ("txn commit %s", glnx_basename (self->commit_stagedir.path));
  if (!glnx_tmpdir_delete (&self->commit_stagedir, cancellable, error))
    return FALSE;
  glnx_release_lock_file (&self->commit_stagedir_lock);

  /* This performs a global cleanup */
  if (!cleanup_tmpdir (self, cancellable, error))
    return FALSE;

  if (self->loose_object_devino_hash)
    g_hash_table_remove_all (self->loose_object_devino_hash);

  if (self->txn.refs)
    if (!_ostree_repo_update_refs (self, self->txn.refs, cancellable, error))
      return FALSE;

  if (self->txn.collection_refs)
    if (!_ostree_repo_update_collection_refs (self, self->txn.collection_refs, cancellable, error))
      return FALSE;

  /* Update the summary if auto-update-summary is set, because doing so was
   * delayed for each ref change during the transaction.
   */
  if (!self->txn.disable_auto_summary && (self->txn.refs || self->txn.collection_refs)
      && !_ostree_repo_maybe_regenerate_summary (self, cancellable, error))
    return FALSE;

  g_clear_pointer (&self->txn.refs, g_hash_table_destroy);
  g_clear_pointer (&self->txn.collection_refs, g_hash_table_destroy);

  self->in_transaction = FALSE;

  if (!ot_ensure_unlinked_at (self->repo_dir_fd, "transaction", 0))
    return FALSE;

  if (self->txn_locked)
    {
      if (!ostree_repo_lock_pop (self, OSTREE_REPO_LOCK_SHARED, cancellable, error))
        return FALSE;
      self->txn_locked = FALSE;
    }

  if (out_stats)
    *out_stats = self->txn.stats;

  return TRUE;
}

/**
 * ostree_repo_abort_transaction:
 * @self: An #OstreeRepo
 * @cancellable: Cancellable
 * @error: Error
 *
 * Abort the active transaction; any staged objects and ref changes will be
 * discarded. You *must* invoke this if you have chosen not to invoke
 * ostree_repo_commit_transaction(). Calling this function when not in a
 * transaction will do nothing and return successfully.
 */
gboolean
ostree_repo_abort_transaction (OstreeRepo *self, GCancellable *cancellable, GError **error)
{
  g_assert (self != NULL);
  g_assert (OSTREE_IS_REPO (self));

  g_autoptr (GError) cleanup_error = NULL;

  /* Always ignore the cancellable to avoid the chance that, if it gets
   * canceled, the transaction may not be fully cleaned up.
   * See https://github.com/ostreedev/ostree/issues/1491 .
   */
  cancellable = NULL;

  /* Note early return */
  if (!self->in_transaction)
    return TRUE;

  g_debug ("Aborting transaction in repository %p", self);

  if (self->loose_object_devino_hash)
    g_hash_table_remove_all (self->loose_object_devino_hash);

  g_clear_pointer (&self->txn.refs, g_hash_table_destroy);
  g_clear_pointer (&self->txn.collection_refs, g_hash_table_destroy);

  glnx_tmpdir_unset (&self->commit_stagedir);
  glnx_release_lock_file (&self->commit_stagedir_lock);

  /* Do not propagate failures from cleanup_tmpdir() immediately, as we want
   * to clean up the rest of the internal transaction state first. */
  cleanup_tmpdir (self, cancellable, &cleanup_error);

  self->in_transaction = FALSE;

  if (self->txn_locked)
    {
      if (!ostree_repo_lock_pop (self, OSTREE_REPO_LOCK_SHARED, cancellable, error))
        return FALSE;
      self->txn_locked = FALSE;
    }

  /* Propagate cleanup_tmpdir() failure. */
  if (cleanup_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&cleanup_error));
      return FALSE;
    }

  return TRUE;
}

/**
 * ostree_repo_write_metadata:
 * @self: Repo
 * @objtype: Object type
 * @expected_checksum: (nullable): If provided, validate content against this checksum
 * @object: Metadata
 * @out_csum: (out) (array fixed-size=32) (optional): Binary checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store the metadata object @object.  Return the checksum
 * as @out_csum.
 *
 * If @expected_checksum is not %NULL, verify it against the
 * computed checksum.
 */
gboolean
ostree_repo_write_metadata (OstreeRepo *self, OstreeObjectType objtype,
                            const char *expected_checksum, GVariant *object, guchar **out_csum,
                            GCancellable *cancellable, GError **error)
{
  g_autoptr (GVariant) normalized = NULL;
  /* First, if we have an expected checksum, see if we already have this
   * object.  This mirrors the same logic in ostree_repo_write_content().
   */
  if (expected_checksum)
    {
      gboolean have_obj;
      if (!_ostree_repo_has_loose_object (self, expected_checksum, objtype, &have_obj, cancellable,
                                          error))
        return FALSE;
      if (have_obj)
        {
          /* Update size metadata if needed */
          if (self->generate_sizes && !repo_has_size_entry (self, objtype, expected_checksum))
            {
              /* Make sure we have a fully serialized object */
              g_autoptr (GVariant) trusted = g_variant_get_normal_form (object);
              gsize size = g_variant_get_size (trusted);
              repo_store_size_entry (self, objtype, expected_checksum, size, size);
            }

          if (out_csum)
            *out_csum = ostree_checksum_to_bytes (expected_checksum);
          return TRUE;
        }
      /* If the caller is giving us an expected checksum, the object really has
       * to be normalized already.  Otherwise, how would they know the checksum?
       * There's no sense in redoing it.
       */
      normalized = g_variant_ref (object);
    }
  else
    {
      normalized = g_variant_get_normal_form (object);
    }

  if (!_ostree_validate_structureof_metadata (objtype, object, error))
    return FALSE;

  g_autoptr (GBytes) vdata = g_variant_get_data_as_bytes (normalized);
  if (!write_metadata_object (self, objtype, expected_checksum, vdata, out_csum, cancellable,
                              error))
    return FALSE;

  return TRUE;
}

/**
 * ostree_repo_write_metadata_stream_trusted:
 * @self: Repo
 * @objtype: Object type
 * @checksum: Store object with this ASCII SHA256 checksum
 * @object_input: Metadata object stream
 * @length: Length, may be 0 for unknown
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store the metadata object @variant; the provided @checksum is
 * trusted.
 */
gboolean
ostree_repo_write_metadata_stream_trusted (OstreeRepo *self, OstreeObjectType objtype,
                                           const char *checksum, GInputStream *object_input,
                                           guint64 length, GCancellable *cancellable,
                                           GError **error)
{
  /* This is all pretty ridiculous, but we're keeping this API for backwards
   * compatibility, it doesn't really need to be fast.
   */
  g_autoptr (GMemoryOutputStream) tmpbuf
      = (GMemoryOutputStream *)g_memory_output_stream_new_resizable ();
  if (g_output_stream_splice ((GOutputStream *)tmpbuf, object_input,
                              G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, cancellable, error)
      < 0)
    return FALSE;
  g_autoptr (GBytes) tmpb = g_memory_output_stream_steal_as_bytes (tmpbuf);

  g_autoptr (GVariant) tmpv
      = g_variant_new_from_bytes (ostree_metadata_variant_type (objtype), tmpb, TRUE);
  return ostree_repo_write_metadata_trusted (self, objtype, checksum, tmpv, cancellable, error);
}

/**
 * ostree_repo_write_metadata_trusted:
 * @self: Repo
 * @objtype: Object type
 * @checksum: Store object with this ASCII SHA256 checksum
 * @variant: Metadata object
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store the metadata object @variant; the provided @checksum is
 * trusted.
 */
gboolean
ostree_repo_write_metadata_trusted (OstreeRepo *self, OstreeObjectType type, const char *checksum,
                                    GVariant *variant, GCancellable *cancellable, GError **error)
{
  return ostree_repo_write_metadata (self, type, checksum, variant, NULL, cancellable, error);
}

typedef struct
{
  OstreeRepo *repo;
  OstreeObjectType objtype;
  char *expected_checksum;
  GVariant *object;
  GCancellable *cancellable;
  guchar *result_csum;
} WriteMetadataAsyncData;

static void
write_metadata_async_data_free (gpointer user_data)
{
  WriteMetadataAsyncData *data = user_data;

  g_clear_object (&data->repo);
  g_clear_object (&data->cancellable);
  g_variant_unref (data->object);
  g_free (data->result_csum);
  g_free (data->expected_checksum);
  g_free (data);
}

static void
write_metadata_thread (GTask *task, GObject *object, gpointer datap, GCancellable *cancellable)
{
  GError *error = NULL;
  WriteMetadataAsyncData *data = datap;

  if (!ostree_repo_write_metadata (data->repo, data->objtype, data->expected_checksum, data->object,
                                   &data->result_csum, cancellable, &error))
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, data, NULL);
}

/**
 * ostree_repo_write_metadata_async:
 * @self: Repo
 * @objtype: Object type
 * @expected_checksum: (nullable): If provided, validate content against this checksum
 * @object: Metadata
 * @cancellable: Cancellable
 * @callback: Invoked when metadata is writed
 * @user_data: Data for @callback
 *
 * Asynchronously store the metadata object @variant.  If provided,
 * the checksum @expected_checksum will be verified.
 */
void
ostree_repo_write_metadata_async (OstreeRepo *self, OstreeObjectType objtype,
                                  const char *expected_checksum, GVariant *object,
                                  GCancellable *cancellable, GAsyncReadyCallback callback,
                                  gpointer user_data)
{
  g_autoptr (GTask) task = NULL;
  WriteMetadataAsyncData *asyncdata;

  asyncdata = g_new0 (WriteMetadataAsyncData, 1);
  asyncdata->repo = g_object_ref (self);
  asyncdata->objtype = objtype;
  asyncdata->expected_checksum = g_strdup (expected_checksum);
  asyncdata->object = g_variant_ref (object);
  asyncdata->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  task = g_task_new (G_OBJECT (self), cancellable, callback, user_data);
  g_task_set_task_data (task, asyncdata, write_metadata_async_data_free);
  g_task_set_source_tag (task, ostree_repo_write_metadata_async);
  g_task_run_in_thread (task, (GTaskThreadFunc)write_metadata_thread);
}

/**
 * ostree_repo_write_metadata_finish:
 * @self: Repo
 * @result: Result
 * @out_csum: (out) (array fixed-size=32) (element-type guint8): Binary checksum value
 * @error: Error
 *
 * Complete a call to ostree_repo_write_metadata_async().
 */
gboolean
ostree_repo_write_metadata_finish (OstreeRepo *self, GAsyncResult *result, guchar **out_csum,
                                   GError **error)
{
  WriteMetadataAsyncData *data;

  g_return_val_if_fail (OSTREE_IS_REPO (self), FALSE);
  g_return_val_if_fail (G_IS_ASYNC_RESULT (result), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, ostree_repo_write_metadata_async), FALSE);

  data = g_task_propagate_pointer (G_TASK (result), error);
  if (data == NULL)
    return FALSE;

  /* Transfer ownership */
  *out_csum = data->result_csum;
  data->result_csum = NULL;
  return TRUE;
}

/* Write an object of type OSTREE_OBJECT_TYPE_DIR_META, using @file_info and @xattrs.
 * Return its (binary) checksum in @out_csum.
 */
gboolean
_ostree_repo_write_directory_meta (OstreeRepo *self, GFileInfo *file_info, GVariant *xattrs,
                                   guchar **out_csum, GCancellable *cancellable, GError **error)
{

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  g_autoptr (GVariant) dirmeta = ostree_create_directory_metadata (file_info, xattrs);
  return ostree_repo_write_metadata (self, OSTREE_OBJECT_TYPE_DIR_META, NULL, dirmeta, out_csum,
                                     cancellable, error);
}

/**
 * ostree_repo_write_content_trusted:
 * @self: Repo
 * @checksum: Store content using this ASCII SHA256 checksum
 * @object_input: Content stream
 * @length: Length of @object_input
 * @cancellable: Cancellable
 * @error: Data for @callback
 *
 * Store the content object streamed as @object_input, with total
 * length @length.  The given @checksum will be treated as trusted.
 *
 * This function should be used when importing file objects from local
 * disk, for example.
 */
gboolean
ostree_repo_write_content_trusted (OstreeRepo *self, const char *checksum,
                                   GInputStream *object_input, guint64 length,
                                   GCancellable *cancellable, GError **error)
{
  return ostree_repo_write_content (self, checksum, object_input, length, NULL, cancellable, error);
}

/**
 * ostree_repo_write_content:
 * @self: Repo
 * @expected_checksum: (allow-none): If provided, validate content against this checksum
 * @object_input: Content object stream
 * @length: Length of @object_input
 * @out_csum: (out) (array fixed-size=32) (optional) (nullable): Binary checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store the content object streamed as @object_input,
 * with total length @length.  The actual checksum will
 * be returned as @out_csum.
 */
gboolean
ostree_repo_write_content (OstreeRepo *self, const char *expected_checksum,
                           GInputStream *object_input, guint64 length, guchar **out_csum,
                           GCancellable *cancellable, GError **error)
{
  /* First, if we have an expected checksum, see if we already have this
   * object.  This mirrors the same logic in ostree_repo_write_metadata().
   *
   * If size metadata is needed, fall through to write_content_object()
   * where the entries are made.
   */
  if (expected_checksum && !self->generate_sizes)
    {
      gboolean have_obj;
      if (!_ostree_repo_has_loose_object (self, expected_checksum, OSTREE_OBJECT_TYPE_FILE,
                                          &have_obj, cancellable, error))
        return FALSE;
      if (have_obj)
        {
          if (out_csum)
            *out_csum = ostree_checksum_to_bytes (expected_checksum);
          return TRUE;
        }
    }

  /* Parse the stream */
  g_autoptr (GInputStream) file_input = NULL;
  g_autoptr (GVariant) xattrs = NULL;
  g_autoptr (GFileInfo) file_info = NULL;
  if (!ostree_content_stream_parse (FALSE, object_input, length, FALSE, &file_input, &file_info,
                                    &xattrs, cancellable, error))
    return FALSE;

  return write_content_object (self, expected_checksum, file_input, file_info, xattrs, out_csum,
                               cancellable, error);
}

/**
 * ostree_repo_write_regfile_inline:
 * @self: repo
 * @expected_checksum: (allow-none): The expected checksum
 * @uid: User id
 * @gid: Group id
 * @mode: File mode
 * @xattrs: (allow-none): Extended attributes, GVariant of type (ayay)
 * @buf: (array length=len) (element-type guint8): File contents
 * @cancellable: Cancellable
 * @error: Error
 *
 * Synchronously create a file object from the provided content.  This API
 * is intended for small files where it is reasonable to buffer the entire
 * content in memory.
 *
 * Unlike `ostree_repo_write_content()`, if @expected_checksum is provided,
 * this function will not check for the presence of the object beforehand.
 *
 * Returns: (transfer full): Checksum (as a hex string) of the committed file
 * Since: 2021.2
 */
_OSTREE_PUBLIC
char *
ostree_repo_write_regfile_inline (OstreeRepo *self, const char *expected_checksum, guint32 uid,
                                  guint32 gid, guint32 mode, GVariant *xattrs, const guint8 *buf,
                                  gsize len, GCancellable *cancellable, GError **error)
{
  g_autoptr (GInputStream) memin = g_memory_input_stream_new_from_data (buf, len, NULL);
  g_autoptr (GFileInfo) finfo = _ostree_mode_uidgid_to_gfileinfo (mode, uid, gid);
  g_file_info_set_size (finfo, len);
  g_autofree guint8 *csum = NULL;
  if (!write_content_object (self, expected_checksum, memin, finfo, xattrs, &csum, cancellable,
                             error))
    return NULL;
  return ostree_checksum_from_bytes (csum);
}

/**
 * ostree_repo_write_symlink:
 * @self: repo
 * @expected_checksum: (allow-none): The expected checksum
 * @uid: User id
 * @gid: Group id
 * @xattrs: (allow-none): Extended attributes, GVariant of type (ayay)
 * @symlink_target: Target of the symbolic link
 * @cancellable: Cancellable
 * @error: Error
 *
 * Synchronously create a symlink object.
 *
 * Unlike `ostree_repo_write_content()`, if @expected_checksum is provided,
 * this function will not check for the presence of the object beforehand.
 *
 * Returns: (transfer full): Checksum (as a hex string) of the committed file
 * Since: 2021.2
 */
char *
ostree_repo_write_symlink (OstreeRepo *self, const char *expected_checksum, guint32 uid,
                           guint32 gid, GVariant *xattrs, const char *symlink_target,
                           GCancellable *cancellable, GError **error)
{
  g_assert (symlink_target != NULL);

  g_autoptr (GFileInfo) finfo = _ostree_mode_uidgid_to_gfileinfo (S_IFLNK | 0777, uid, gid);
  g_file_info_set_attribute_byte_string (finfo, "standard::symlink-target", symlink_target);
  g_autofree guint8 *csum = NULL;
  if (!write_content_object (self, expected_checksum, NULL, finfo, xattrs, &csum, cancellable,
                             error))
    return NULL;
  return ostree_checksum_from_bytes (csum);
}

/**
 * ostree_repo_write_regfile:
 * @self: Repo,
 * @expected_checksum: (allow-none): Expected checksum (SHA-256 hex string)
 * @uid: user id
 * @gid: group id
 * @mode: Unix file mode
 * @content_len: Expected content length
 * @xattrs: (allow-none): Extended attributes (GVariant type `(ayay)`)
 * @error: Error
 *
 * Create an `OstreeContentWriter` that allows streaming output into
 * the repository.
 *
 * Returns: (transfer full): A new writer, or %NULL on error
 * Since: 2021.2
 */
OstreeContentWriter *
ostree_repo_write_regfile (OstreeRepo *self, const char *expected_checksum, guint32 uid,
                           guint32 gid, guint32 mode, guint64 content_len, GVariant *xattrs,
                           GError **error)
{
  if (self->mode == OSTREE_REPO_MODE_ARCHIVE)
    return glnx_null_throw (
        error, "Cannot currently use ostree_repo_write_regfile() on an archive mode repository");

  return _ostree_content_writer_new (self, expected_checksum, uid, gid, mode, content_len, xattrs,
                                     error);
}

typedef struct
{
  OstreeRepo *repo;
  char *expected_checksum;
  GInputStream *object;
  guint64 file_object_length;
  GCancellable *cancellable;

  guchar *result_csum;
} WriteContentAsyncData;

static void
write_content_async_data_free (gpointer user_data)
{
  WriteContentAsyncData *data = user_data;

  g_clear_object (&data->repo);
  g_clear_object (&data->cancellable);
  g_clear_object (&data->object);
  g_free (data->result_csum);
  g_free (data->expected_checksum);
  g_free (data);
}

static void
write_content_thread (GTask *task, GObject *object, gpointer datap, GCancellable *cancellable)
{
  GError *error = NULL;
  WriteContentAsyncData *data = datap;

  if (!ostree_repo_write_content (data->repo, data->expected_checksum, data->object,
                                  data->file_object_length, &data->result_csum, cancellable,
                                  &error))
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, data, NULL);
}

/**
 * ostree_repo_write_content_async:
 * @self: Repo
 * @expected_checksum: (allow-none): If provided, validate content against this checksum
 * @object: Input
 * @length: Length of @object
 * @cancellable: Cancellable
 * @callback: Invoked when content is writed
 * @user_data: User data for @callback
 *
 * Asynchronously store the content object @object.  If provided, the
 * checksum @expected_checksum will be verified.
 */
void
ostree_repo_write_content_async (OstreeRepo *self, const char *expected_checksum,
                                 GInputStream *object, guint64 length, GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (GTask) task = NULL;
  WriteContentAsyncData *asyncdata;

  asyncdata = g_new0 (WriteContentAsyncData, 1);
  asyncdata->repo = g_object_ref (self);
  asyncdata->expected_checksum = g_strdup (expected_checksum);
  asyncdata->object = g_object_ref (object);
  asyncdata->file_object_length = length;
  asyncdata->cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  task = g_task_new (G_OBJECT (self), cancellable, callback, user_data);
  g_task_set_task_data (task, asyncdata, (GDestroyNotify)write_content_async_data_free);
  g_task_set_source_tag (task, ostree_repo_write_content_async);
  g_task_run_in_thread (task, (GTaskThreadFunc)write_content_thread);
}

/**
 * ostree_repo_write_content_finish:
 * @self: a #OstreeRepo
 * @result: a #GAsyncResult
 * @out_csum: (out) (transfer full) (optional): A binary SHA256
 * checksum of the content object
 * @error: a #GError
 *
 * Completes an invocation of ostree_repo_write_content_async().
 */
gboolean
ostree_repo_write_content_finish (OstreeRepo *self, GAsyncResult *result, guchar **out_csum,
                                  GError **error)
{
  WriteContentAsyncData *data;

  g_return_val_if_fail (OSTREE_IS_REPO (self), FALSE);
  g_return_val_if_fail (G_IS_ASYNC_RESULT (result), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, ostree_repo_write_content_async), FALSE);

  data = g_task_propagate_pointer (G_TASK (result), error);
  if (data == NULL)
    return FALSE;

  ot_transfer_out_value (out_csum, &data->result_csum);
  return TRUE;
}

/**
 * ostree_repo_write_commit:
 * @self: Repo
 * @parent: (nullable): ASCII SHA256 checksum for parent, or %NULL for none
 * @subject: (nullable): Subject
 * @body: (nullable): Body
 * @metadata: (nullable): GVariant of type a{sv}, or %NULL for none
 * @root: The tree to point the commit to
 * @out_commit: (out) (optional): Resulting ASCII SHA256 checksum for
 * commit
 * @cancellable: Cancellable
 * @error: Error
 *
 * Write a commit metadata object, referencing @root_contents_checksum
 * and @root_metadata_checksum.
 * This uses the current time as the commit timestamp, but it can be
 * overridden with an explicit timestamp via the
 * [standard](https://reproducible-builds.org/specs/source-date-epoch/)
 * `SOURCE_DATE_EPOCH` environment flag.
 */
gboolean
ostree_repo_write_commit (OstreeRepo *self, const char *parent, const char *subject,
                          const char *body, GVariant *metadata, OstreeRepoFile *root,
                          char **out_commit, GCancellable *cancellable, GError **error)
{
  gint64 timestamp = 0;
  const gchar *env_timestamp = g_getenv ("SOURCE_DATE_EPOCH");
  if (env_timestamp == NULL)
    {
      g_autoptr (GDateTime) now = g_date_time_new_now_utc ();
      timestamp = g_date_time_to_unix (now);
    }
  else
    {
      gchar *ret = NULL;
      errno = 0;
      timestamp = g_ascii_strtoll (env_timestamp, &ret, 10);
      if (errno != 0)
        return glnx_throw_errno_prefix (error, "Parsing SOURCE_DATE_EPOCH");
      if (ret == env_timestamp)
        return glnx_throw (error, "Failed to convert SOURCE_DATE_EPOCH");
    }

  return ostree_repo_write_commit_with_time (self, parent, subject, body, metadata, root, timestamp,
                                             out_commit, cancellable, error);
}

static GVariant *
add_auto_metadata (OstreeRepo *self, GVariant *original_metadata, OstreeRepoFile *repo_root,
                   GCancellable *cancellable, GError **error)
{
  g_autoptr (GVariantBuilder) builder = NULL;

  /* original_metadata may be NULL */
  builder = ot_util_variant_builder_from_variant (original_metadata, G_VARIANT_TYPE ("a{sv}"));

  add_size_index_to_metadata (self, builder);

  return g_variant_ref_sink (g_variant_builder_end (builder));
}

/**
 * ostree_repo_write_commit_with_time:
 * @self: Repo
 * @parent: (nullable): ASCII SHA256 checksum for parent, or %NULL for none
 * @subject: (nullable): Subject
 * @body: (nullable): Body
 * @metadata: (nullable): GVariant of type a{sv}, or %NULL for none
 * @root: The tree to point the commit to
 * @time: The time to use to stamp the commit
 * @out_commit: (out) (optional): Resulting ASCII SHA256 checksum for
 * commit
 * @cancellable: Cancellable
 * @error: Error
 *
 * Write a commit metadata object, referencing @root_contents_checksum
 * and @root_metadata_checksum.
 */
gboolean
ostree_repo_write_commit_with_time (OstreeRepo *self, const char *parent, const char *subject,
                                    const char *body, GVariant *metadata, OstreeRepoFile *root,
                                    guint64 time, char **out_commit, GCancellable *cancellable,
                                    GError **error)
{
  OstreeRepoFile *repo_root = OSTREE_REPO_FILE (root);

  /* Add sizes information to our metadata object */
  g_autoptr (GVariant) new_metadata
      = add_auto_metadata (self, metadata, repo_root, cancellable, error);
  if (new_metadata == NULL)
    return FALSE;

  g_autoptr (GVariant) commit = g_variant_new (
      "(@a{sv}@ay@a(say)sst@ay@ay)", new_metadata,
      parent ? ostree_checksum_to_bytes_v (parent) : ot_gvariant_new_bytearray (NULL, 0),
      g_variant_new_array (G_VARIANT_TYPE ("(say)"), NULL, 0), subject ? subject : "",
      body ? body : "", GUINT64_TO_BE (time),
      ostree_checksum_to_bytes_v (ostree_repo_file_tree_get_contents_checksum (repo_root)),
      ostree_checksum_to_bytes_v (ostree_repo_file_tree_get_metadata_checksum (repo_root)));
  g_variant_ref_sink (commit);
  g_autofree guchar *commit_csum = NULL;
  if (!ostree_repo_write_metadata (self, OSTREE_OBJECT_TYPE_COMMIT, NULL, commit, &commit_csum,
                                   cancellable, error))
    return FALSE;

  g_autofree char *ret_commit = ostree_checksum_from_bytes (commit_csum);
  ot_transfer_out_value (out_commit, &ret_commit);
  return TRUE;
}

/**
 * ostree_repo_read_commit_detached_metadata:
 * @self: Repo
 * @checksum: ASCII SHA256 commit checksum
 * @out_metadata: (out) (nullable) (transfer full): Metadata associated with commit in with format
 * "a{sv}", or %NULL if none exists
 * @cancellable: Cancellable
 * @error: Error
 *
 * OSTree commits can have arbitrary metadata associated; this
 * function retrieves them.  If none exists, @out_metadata will be set
 * to %NULL.
 */
gboolean
ostree_repo_read_commit_detached_metadata (OstreeRepo *self, const char *checksum,
                                           GVariant **out_metadata, GCancellable *cancellable,
                                           GError **error)
{
  g_assert (out_metadata != NULL);

  char buf[_OSTREE_LOOSE_PATH_MAX];
  _ostree_loose_path (buf, checksum, OSTREE_OBJECT_TYPE_COMMIT_META, self->mode);

  if (self->commit_stagedir.initialized)
    {
      glnx_autofd int fd = -1;
      if (!ot_openat_ignore_enoent (self->commit_stagedir.fd, buf, &fd, error))
        return FALSE;
      if (fd != -1)
        return ot_variant_read_fd (fd, 0, G_VARIANT_TYPE ("a{sv}"), TRUE, out_metadata, error);
    }

  glnx_autofd int fd = -1;
  if (!ot_openat_ignore_enoent (self->objects_dir_fd, buf, &fd, error))
    return FALSE;
  if (fd != -1)
    return ot_variant_read_fd (fd, 0, G_VARIANT_TYPE ("a{sv}"), TRUE, out_metadata, error);

  if (self->parent_repo)
    return ostree_repo_read_commit_detached_metadata (self->parent_repo, checksum, out_metadata,
                                                      cancellable, error);
  /* Nothing found */
  *out_metadata = NULL;
  return TRUE;
}

/**
 * ostree_repo_write_commit_detached_metadata:
 * @self: Repo
 * @checksum: ASCII SHA256 commit checksum
 * @metadata: (nullable): Metadata to associate with commit in with format "a{sv}", or %NULL to
 * delete
 * @cancellable: Cancellable
 * @error: Error
 *
 * Replace any existing metadata associated with commit referred to by
 * @checksum with @metadata.  If @metadata is %NULL, then existing
 * data will be deleted.
 */
gboolean
ostree_repo_write_commit_detached_metadata (OstreeRepo *self, const char *checksum,
                                            GVariant *metadata, GCancellable *cancellable,
                                            GError **error)
{
  int dest_dfd;
  if (self->in_transaction)
    dest_dfd = self->commit_stagedir.fd;
  else
    dest_dfd = self->objects_dir_fd;

  if (!_ostree_repo_ensure_loose_objdir_at (dest_dfd, checksum, cancellable, error))
    return FALSE;

  g_autoptr (GVariant) normalized = NULL;
  gsize normalized_size = 0;
  const guint8 *data = NULL;
  if (metadata != NULL)
    {
      normalized = g_variant_get_normal_form (metadata);
      normalized_size = g_variant_get_size (normalized);
      data = g_variant_get_data (normalized);
    }

  if (data == NULL)
    data = (guint8 *)"";

  char pathbuf[_OSTREE_LOOSE_PATH_MAX];
  _ostree_loose_path (pathbuf, checksum, OSTREE_OBJECT_TYPE_COMMIT_META, self->mode);
  if (!glnx_file_replace_contents_at (dest_dfd, pathbuf, data, normalized_size, 0, cancellable,
                                      error))
    {
      g_prefix_error (error, "Unable to write detached metadata: ");
      return FALSE;
    }

  return TRUE;
}

/* This generates an in-memory OSTREE_OBJECT_TYPE_DIR_TREE variant, using the
 * content objects and subdirectories. The input hashes will be sorted
 */
static GVariant *
create_tree_variant_from_hashes (GHashTable *file_checksums, GHashTable *dir_contents_checksums,
                                 GHashTable *dir_metadata_checksums)
{
  GVariantBuilder files_builder;
  g_variant_builder_init (&files_builder, G_VARIANT_TYPE ("a(say)"));
  GVariantBuilder dirs_builder;
  g_variant_builder_init (&dirs_builder, G_VARIANT_TYPE ("a(sayay)"));

  GSList *sorted_filenames = NULL;
  GLNX_HASH_TABLE_FOREACH (file_checksums, const char *, name)
    {
      /* Should have been validated earlier, but be paranoid */
      g_assert (ot_util_filename_validate (name, NULL));

      sorted_filenames = g_slist_prepend (sorted_filenames, (char *)name);
    }
  sorted_filenames = g_slist_sort (sorted_filenames, (GCompareFunc)strcmp);
  for (GSList *iter = sorted_filenames; iter; iter = iter->next)
    {
      const char *name = iter->data;
      const char *value;

      value = g_hash_table_lookup (file_checksums, name);
      g_variant_builder_add (&files_builder, "(s@ay)", name, ostree_checksum_to_bytes_v (value));
    }
  g_slist_free (sorted_filenames);
  sorted_filenames = NULL;

  GLNX_HASH_TABLE_FOREACH (dir_metadata_checksums, const char *, name)
    sorted_filenames = g_slist_prepend (sorted_filenames, (char *)name);
  sorted_filenames = g_slist_sort (sorted_filenames, (GCompareFunc)strcmp);

  for (GSList *iter = sorted_filenames; iter; iter = iter->next)
    {
      const char *name = iter->data;
      const char *content_checksum = g_hash_table_lookup (dir_contents_checksums, name);
      const char *meta_checksum = g_hash_table_lookup (dir_metadata_checksums, name);

      g_variant_builder_add (&dirs_builder, "(s@ay@ay)", name,
                             ostree_checksum_to_bytes_v (content_checksum),
                             ostree_checksum_to_bytes_v (meta_checksum));
    }

  g_slist_free (sorted_filenames);
  sorted_filenames = NULL;

  GVariant *serialized_tree
      = g_variant_new ("(@a(say)@a(sayay))", g_variant_builder_end (&files_builder),
                       g_variant_builder_end (&dirs_builder));
  return g_variant_ref_sink (serialized_tree);
}

/* If any filtering is set up, perform it, and return modified file info in
 * @out_modified_info. Note that if no filtering is applied, @out_modified_info
 * will simply be another reference (with incremented refcount) to @file_info.
 */
OstreeRepoCommitFilterResult
_ostree_repo_commit_modifier_apply (OstreeRepo *self, OstreeRepoCommitModifier *modifier,
                                    const char *path, GFileInfo *file_info,
                                    GFileInfo **out_modified_info)
{
  gboolean canonicalize_perms = FALSE;
  gboolean has_filter = FALSE;
  OstreeRepoCommitFilterResult result = OSTREE_REPO_COMMIT_FILTER_ALLOW;
  GFileInfo *modified_info;

  /* Auto-detect bare-user-only repo, force canonical permissions. */
  if (self->mode == OSTREE_REPO_MODE_BARE_USER_ONLY)
    canonicalize_perms = TRUE;

  if (modifier != NULL)
    {
      if ((modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS) != 0)
        canonicalize_perms = TRUE;
      if (modifier->filter != NULL)
        has_filter = TRUE;
    }

  if (!(canonicalize_perms || has_filter))
    {
      *out_modified_info = g_object_ref (file_info);
      return OSTREE_REPO_COMMIT_FILTER_ALLOW; /* Note: early return (no actions needed) */
    }

  modified_info = g_file_info_dup (file_info);

  if (has_filter)
    result = modifier->filter (self, path, modified_info, modifier->user_data);

  if (canonicalize_perms)
    {
      guint mode = g_file_info_get_attribute_uint32 (modified_info, "unix::mode");
      switch (g_file_info_get_file_type (file_info))
        {
        case G_FILE_TYPE_REGULAR:
          /* In particular, we want to squash the s{ug}id bits, but this also
           * catches the sticky bit for example.
           */
          g_file_info_set_attribute_uint32 (modified_info, "unix::mode", mode & (S_IFREG | 0755));
          break;
        case G_FILE_TYPE_DIRECTORY:
          /* Like the above but for directories */
          g_file_info_set_attribute_uint32 (modified_info, "unix::mode", mode & (S_IFDIR | 0755));
          break;
        case G_FILE_TYPE_SYMBOLIC_LINK:
          break;
        default:
          g_assert_not_reached ();
        }
      g_file_info_set_attribute_uint32 (modified_info, "unix::uid", 0);
      g_file_info_set_attribute_uint32 (modified_info, "unix::gid", 0);
    }

  *out_modified_info = modified_info;

  return result;
}

/* Convert @path into a string */
static char *
ptrarray_path_join (GPtrArray *path)
{
  GString *path_buf = g_string_new ("");

  if (path->len == 0)
    g_string_append_c (path_buf, '/');
  else
    {
      for (guint i = 0; i < path->len; i++)
        {
          const char *elt = path->pdata[i];

          g_string_append_c (path_buf, '/');
          g_string_append (path_buf, elt);
        }
    }

  return g_string_free (path_buf, FALSE);
}

static gboolean
get_final_xattrs (OstreeRepo *self, OstreeRepoCommitModifier *modifier, const char *relpath,
                  GFileInfo *file_info, GFile *path, int dfd, const char *dfd_subpath,
                  GVariant *source_xattrs, GVariant **out_xattrs, gboolean *out_modified,
                  GCancellable *cancellable, GError **error)
{
  /* track whether the returned xattrs differ from the file on disk */
  gboolean modified = TRUE;
  const gboolean skip_xattrs = (modifier
                                && (modifier->flags
                                    & (OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS
                                       | OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS))
                                       > 0)
                               || self->mode == OSTREE_REPO_MODE_BARE_USER_ONLY;

  /* fetch on-disk xattrs if needed & not disabled */
  g_autoptr (GVariant) original_xattrs = NULL;
  if (!skip_xattrs && !self->disable_xattrs)
    {
      if (source_xattrs)
        original_xattrs = g_variant_ref (source_xattrs);
      else if (path && OSTREE_IS_REPO_FILE (path))
        {
          if (!ostree_repo_file_get_xattrs (OSTREE_REPO_FILE (path), &original_xattrs, cancellable,
                                            error))
            return FALSE;
        }
      else if (path)
        {
          if (!glnx_dfd_name_get_all_xattrs (AT_FDCWD, gs_file_get_path_cached (path),
                                             &original_xattrs, cancellable, error))
            return FALSE;
        }
      else if (dfd_subpath == NULL)
        {
          g_assert (dfd != -1);
          original_xattrs = ostree_fs_get_all_xattrs (dfd, cancellable, error);
          if (!original_xattrs)
            return FALSE;
        }
      else
        {
          g_assert (dfd != -1);
          original_xattrs = ostree_fs_get_all_xattrs_at (dfd, dfd_subpath, cancellable, error);
          if (!original_xattrs)
            return FALSE;
        }

      g_assert (original_xattrs);
    }

  g_autoptr (GVariant) ret_xattrs = NULL;
  if (modifier && modifier->xattr_callback)
    {
      ret_xattrs = modifier->xattr_callback (self, relpath, file_info, modifier->xattr_user_data);
    }

  /* if callback returned NULL or didn't exist, default to on-disk state */
  if (!ret_xattrs && original_xattrs)
    ret_xattrs = g_variant_ref (original_xattrs);

  if (modifier && modifier->sepolicy)
    {
      g_autofree char *label = NULL;
      const char *path_for_labeling = relpath;

      bool using_v1 = (modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SELINUX_LABEL_V1) > 0;
      bool is_usretc = g_str_equal (relpath, "/usr/etc") || g_str_has_prefix (relpath, "/usr/etc/");
      if (using_v1 && is_usretc)
        path_for_labeling += strlen ("/usr");

      if (!ostree_sepolicy_get_label (modifier->sepolicy, path_for_labeling,
                                      g_file_info_get_attribute_uint32 (file_info, "unix::mode"),
                                      &label, cancellable, error))
        return FALSE;

      if (!label && (modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_ERROR_ON_UNLABELED) > 0)
        {
          return glnx_throw (error, "Failed to look up SELinux label for '%s'", relpath);
        }
      else if (label)
        {
          g_autoptr (GVariantBuilder) builder = NULL;

          if (ret_xattrs)
            {
              /* drop out any existing SELinux policy from the set, so we don't end up
               * counting it twice in the checksum */
              GVariant *new_ret_xattrs = _ostree_filter_selinux_xattr (ret_xattrs);
              g_variant_unref (ret_xattrs);
              ret_xattrs = new_ret_xattrs;
            }

          /* ret_xattrs may be NULL */
          builder = ot_util_variant_builder_from_variant (ret_xattrs, G_VARIANT_TYPE ("a(ayay)"));

          g_variant_builder_add_value (
              builder, g_variant_new ("(@ay@ay)", g_variant_new_bytestring ("security.selinux"),
                                      g_variant_new_bytestring (label)));
          if (ret_xattrs)
            g_variant_unref (ret_xattrs);

          ret_xattrs = g_variant_builder_end (builder);
          g_variant_ref_sink (ret_xattrs);
        }
    }

  if (original_xattrs && ret_xattrs && g_variant_equal (original_xattrs, ret_xattrs))
    modified = FALSE;

  if (out_xattrs)
    *out_xattrs = g_steal_pointer (&ret_xattrs);
  if (out_modified)
    *out_modified = modified;
  return TRUE;
}

static gboolean write_directory_to_mtree_internal (OstreeRepo *self, GFile *dir,
                                                   OstreeMutableTree *mtree,
                                                   OstreeRepoCommitModifier *modifier,
                                                   GPtrArray *path, GCancellable *cancellable,
                                                   GError **error);
static gboolean write_dfd_iter_to_mtree_internal (OstreeRepo *self, GLnxDirFdIterator *src_dfd_iter,
                                                  OstreeMutableTree *mtree,
                                                  OstreeRepoCommitModifier *modifier,
                                                  GPtrArray *path, GCancellable *cancellable,
                                                  GError **error);

typedef enum
{
  WRITE_DIR_CONTENT_FLAGS_NONE = 0,
  WRITE_DIR_CONTENT_FLAGS_CAN_ADOPT = 1,
} WriteDirContentFlags;

/* Given either a dir_enum or a dfd_iter, writes the directory entry (which is
 * itself a directory) to the mtree. For subdirs, we go back through either
 * write_dfd_iter_to_mtree_internal (dfd_iter case) or
 * write_directory_to_mtree_internal (dir_enum case) which will do the actual
 * dirmeta + dirent iteration. */
static gboolean
write_dir_entry_to_mtree_internal (OstreeRepo *self, OstreeRepoFile *repo_dir,
                                   GFileEnumerator *dir_enum, GLnxDirFdIterator *dfd_iter,
                                   WriteDirContentFlags writeflags, GFileInfo *child_info,
                                   OstreeMutableTree *mtree, OstreeRepoCommitModifier *modifier,
                                   GPtrArray *path, GCancellable *cancellable, GError **error)
{
  g_assert (dir_enum != NULL || dfd_iter != NULL);
  g_assert (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY);

  const char *name = g_file_info_get_name (child_info);

  /* We currently only honor the CONSUME flag in the dfd_iter case to avoid even
   * more complexity in this function, and it'd mostly only be useful when
   * operating on local filesystems anyways.
   */
  const gboolean delete_after_commit
      = dfd_iter && modifier && (modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CONSUME);

  /* Build the full path which we need for callbacks */
  g_ptr_array_add (path, (char *)name);
  g_autofree char *child_relpath = ptrarray_path_join (path);

  /* Call the filter */
  g_autoptr (GFileInfo) modified_info = NULL;
  OstreeRepoCommitFilterResult filter_result = _ostree_repo_commit_modifier_apply (
      self, modifier, child_relpath, child_info, &modified_info);

  if (filter_result != OSTREE_REPO_COMMIT_FILTER_ALLOW)
    {
      g_ptr_array_remove_index (path, path->len - 1);
      if (delete_after_commit)
        {
          g_assert (dfd_iter);
          if (!glnx_shutil_rm_rf_at (dfd_iter->fd, name, cancellable, error))
            return FALSE;
        }
      /* Note: early return */
      return TRUE;
    }

  g_autoptr (GFile) child = NULL;
  if (dir_enum != NULL)
    child = g_file_enumerator_get_child (dir_enum, child_info);

  g_autoptr (OstreeMutableTree) child_mtree = NULL;
  if (!ostree_mutable_tree_ensure_dir (mtree, name, &child_mtree, error))
    return FALSE;

  /* Finally, recurse on the dir */
  if (dir_enum != NULL)
    {
      if (!write_directory_to_mtree_internal (self, child, child_mtree, modifier, path, cancellable,
                                              error))
        return FALSE;
    }
  else if (repo_dir)
    {
      g_assert (dir_enum != NULL);
      g_debug ("Adding: %s", gs_file_get_path_cached (child));
      if (!ostree_mutable_tree_replace_file (
              mtree, name, ostree_repo_file_get_checksum ((OstreeRepoFile *)child), error))
        return FALSE;
    }
  else
    {
      g_assert (dfd_iter != NULL);
      g_auto (GLnxDirFdIterator) child_dfd_iter = {
        0,
      };

      if (!glnx_dirfd_iterator_init_at (dfd_iter->fd, name, FALSE, &child_dfd_iter, error))
        return FALSE;

      if (!write_dfd_iter_to_mtree_internal (self, &child_dfd_iter, child_mtree, modifier, path,
                                             cancellable, error))
        return FALSE;

      if (delete_after_commit)
        {
          if (!glnx_unlinkat (dfd_iter->fd, name, AT_REMOVEDIR, error))
            return FALSE;
        }
    }

  g_ptr_array_remove_index (path, path->len - 1);

  return TRUE;
}

/* Given either a dir_enum or a dfd_iter, writes a non-dir (regfile/symlink) to
 * the mtree.
 */
static gboolean
write_content_to_mtree_internal (OstreeRepo *self, OstreeRepoFile *repo_dir,
                                 GFileEnumerator *dir_enum, GLnxDirFdIterator *dfd_iter,
                                 WriteDirContentFlags writeflags, GFileInfo *child_info,
                                 OstreeMutableTree *mtree, OstreeRepoCommitModifier *modifier,
                                 GPtrArray *path, GCancellable *cancellable, GError **error)
{
  g_assert (dir_enum != NULL || dfd_iter != NULL);

  GFileType file_type = g_file_info_get_file_type (child_info);
  const char *name = g_file_info_get_name (child_info);

  /* Load flags into boolean constants for ease of readability (we also need to
   * NULL-check modifier)
   */
  const gboolean canonical_permissions
      = self->mode == OSTREE_REPO_MODE_BARE_USER_ONLY
        || (modifier
            && (modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CANONICAL_PERMISSIONS));
  const gboolean devino_canonical
      = modifier && (modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_DEVINO_CANONICAL);
  /* We currently only honor the CONSUME flag in the dfd_iter case to avoid even
   * more complexity in this function, and it'd mostly only be useful when
   * operating on local filesystems anyways.
   */
  const gboolean delete_after_commit
      = dfd_iter && modifier && (modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CONSUME);

  /* See if we have a devino hit; this is used below in a few places. */
  const char *loose_checksum = NULL;
  if (dfd_iter != NULL)
    {
      guint32 dev = g_file_info_get_attribute_uint32 (child_info, "unix::device");
      guint64 inode = g_file_info_get_attribute_uint64 (child_info, "unix::inode");
      loose_checksum = devino_cache_lookup (self, modifier, dev, inode);
      if (loose_checksum && devino_canonical)
        {
          /* Go directly to checksum, do not pass Go, do not collect $200.
           * In this mode the app is required to break hardlinks for any
           * files it wants to modify.
           */
          if (!ostree_mutable_tree_replace_file (mtree, name, loose_checksum, error))
            return FALSE;
          if (delete_after_commit)
            {
              if (!glnx_shutil_rm_rf_at (dfd_iter->fd, name, cancellable, error))
                return FALSE;
            }
          g_mutex_lock (&self->txn_lock);
          self->txn.stats.devino_cache_hits++;
          g_mutex_unlock (&self->txn_lock);
          return TRUE; /* Early return */
        }
    }

  /* Build the full path which we need for callbacks */
  g_ptr_array_add (path, (char *)name);
  g_autofree char *child_relpath = ptrarray_path_join (path);

  /* For bare-user repos we'll reload our file info from the object
   * (specifically the ostreemeta xattr), if it was checked out that way (via
   * hardlink). The on-disk state is not normally what we want to commit.
   * Basically we're making sure that we pick up "real" uid/gid and any xattrs
   * there.
   */
  g_autoptr (GVariant) source_xattrs = NULL;
  g_autoptr (GFileInfo) source_child_info = NULL;
  if (loose_checksum && self->mode == OSTREE_REPO_MODE_BARE_USER)
    {
      if (!ostree_repo_load_file (self, loose_checksum, NULL, &source_child_info, &source_xattrs,
                                  cancellable, error))
        return FALSE;
      child_info = source_child_info;
    }

  /* Call the filter */
  g_autoptr (GFileInfo) modified_info = NULL;
  OstreeRepoCommitFilterResult filter_result = _ostree_repo_commit_modifier_apply (
      self, modifier, child_relpath, child_info, &modified_info);
  const gboolean child_info_was_modified = !_ostree_gfileinfo_equal (child_info, modified_info);

  if (filter_result != OSTREE_REPO_COMMIT_FILTER_ALLOW)
    {
      g_ptr_array_remove_index (path, path->len - 1);
      if (delete_after_commit)
        {
          g_assert (dfd_iter);
          if (!glnx_shutil_rm_rf_at (dfd_iter->fd, name, cancellable, error))
            return FALSE;
        }
      /* Note: early return */
      return TRUE;
    }

  switch (file_type)
    {
    case G_FILE_TYPE_SYMBOLIC_LINK:
    case G_FILE_TYPE_REGULAR:
      break;
    default:
      return glnx_throw (error, "Unsupported file type for file: '%s'", child_relpath);
    }

  g_autoptr (GFile) child = NULL;
  if (dir_enum != NULL)
    child = g_file_enumerator_get_child (dir_enum, child_info);

  /* Our filters have passed, etc.; now we prepare to write the content object */
  glnx_autofd int file_input_fd = -1;

  /* Open the file now, since it's better for reading xattrs
   * rather than using the /proc/self/fd links.
   *
   * TODO: Do this lazily, since for e.g. bare-user-only repos
   * we don't have xattrs and don't need to open every file
   * for things that have devino cache hits.
   */
  if (file_type == G_FILE_TYPE_REGULAR && dfd_iter != NULL)
    {
      if (!glnx_openat_rdonly (dfd_iter->fd, name, FALSE, &file_input_fd, error))
        return FALSE;
    }

  g_autoptr (GVariant) xattrs = NULL;
  gboolean xattrs_were_modified;
  if (dir_enum != NULL)
    {
      if (!get_final_xattrs (self, modifier, child_relpath, child_info, child, -1, name,
                             source_xattrs, &xattrs, &xattrs_were_modified, cancellable, error))
        return FALSE;
    }
  else
    {
      /* These contortions are basically so we use glnx_fd_get_all_xattrs()
       * for regfiles, and glnx_dfd_name_get_all_xattrs() for symlinks.
       */
      int xattr_fd_arg = (file_input_fd != -1) ? file_input_fd : dfd_iter->fd;
      const char *xattr_path_arg = (file_input_fd != -1) ? NULL : name;
      if (!get_final_xattrs (self, modifier, child_relpath, child_info, child, xattr_fd_arg,
                             xattr_path_arg, source_xattrs, &xattrs, &xattrs_were_modified,
                             cancellable, error))
        return FALSE;
    }

  /* Used below to see whether we can do a fast path commit */
  const gboolean modified_file_meta = child_info_was_modified || xattrs_were_modified;

  /* A big prerequisite list of conditions for whether or not we can
   * "adopt", i.e. just checksum and rename() into place
   */
  const gboolean can_adopt_basic = file_type == G_FILE_TYPE_REGULAR && dfd_iter != NULL
                                   && delete_after_commit
                                   && ((writeflags & WRITE_DIR_CONTENT_FLAGS_CAN_ADOPT) > 0);
  gboolean can_adopt = can_adopt_basic;
  /* If basic prerquisites are met, check repo mode specific ones */
  if (can_adopt)
    {
      /* For bare repos, we could actually chown/reset the xattrs, but let's
       * do the basic optimizations here first.
       */
      if (self->mode == OSTREE_REPO_MODE_BARE)
        can_adopt = !modified_file_meta;
      else if (self->mode == OSTREE_REPO_MODE_BARE_USER_ONLY)
        can_adopt = canonical_permissions;
      else
        /* This covers bare-user and archive.  See comments in adopt_and_commit_regfile()
         * for notes on adding bare-user later here.
         */
        can_adopt = FALSE;
    }
  gboolean did_adopt = FALSE;

  /* The very fast path - we have a devino cache hit, nothing to write */
  if (loose_checksum && !modified_file_meta)
    {
      if (!ostree_mutable_tree_replace_file (mtree, name, loose_checksum, error))
        return FALSE;

      g_mutex_lock (&self->txn_lock);
      self->txn.stats.devino_cache_hits++;
      g_mutex_unlock (&self->txn_lock);
    }
  /* Next fast path - we can "adopt" the file */
  else if (can_adopt)
    {
      char checksum[OSTREE_SHA256_STRING_LEN + 1];
      if (!adopt_and_commit_regfile (self, dfd_iter->fd, name, modified_info, xattrs, checksum,
                                     cancellable, error))
        return FALSE;
      if (!ostree_mutable_tree_replace_file (mtree, name, checksum, error))
        return FALSE;
      did_adopt = TRUE;
    }
  else
    {
      g_autoptr (GInputStream) file_input = NULL;

      if (file_type == G_FILE_TYPE_REGULAR)
        {
          if (dir_enum != NULL)
            {
              g_assert (child != NULL);
              file_input = (GInputStream *)g_file_read (child, cancellable, error);
              if (!file_input)
                return FALSE;
            }
          else
            {
              /* We already opened the fd above */
              file_input = g_unix_input_stream_new (file_input_fd, FALSE);
            }
        }

      g_autofree guchar *child_file_csum = NULL;
      if (!write_content_object (self, NULL, file_input, modified_info, xattrs, &child_file_csum,
                                 cancellable, error))
        return FALSE;

      char tmp_checksum[OSTREE_SHA256_STRING_LEN + 1];
      ostree_checksum_inplace_from_bytes (child_file_csum, tmp_checksum);
      if (!ostree_mutable_tree_replace_file (mtree, name, tmp_checksum, error))
        return FALSE;
    }

  /* Process delete_after_commit. In the adoption case though, we already
   * took ownership of the file above, usually via a renameat().
   */
  if (delete_after_commit && !did_adopt)
    {
      if (!glnx_unlinkat (dfd_iter->fd, name, 0, error))
        return FALSE;
    }

  g_ptr_array_remove_index (path, path->len - 1);

  return TRUE;
}

/* Handles the dirmeta for the given GFile dir and then calls
 * write_{dir_entry,content}_to_mtree_internal() for each directory entry. */
static gboolean
write_directory_to_mtree_internal (OstreeRepo *self, GFile *dir, OstreeMutableTree *mtree,
                                   OstreeRepoCommitModifier *modifier, GPtrArray *path,
                                   GCancellable *cancellable, GError **error)
{
  OstreeRepoCommitFilterResult filter_result;
  OstreeRepoFile *repo_dir = NULL;

  if (dir)
    g_debug ("Examining: %s", gs_file_get_path_cached (dir));

  /* If the directory is already in the repository, we can try to
   * reuse checksums to skip checksumming. */
  if (dir && OSTREE_IS_REPO_FILE (dir) && modifier == NULL)
    repo_dir = (OstreeRepoFile *)dir;

  if (repo_dir)
    {
      if (!ostree_repo_file_ensure_resolved (repo_dir, error))
        return FALSE;

      /* ostree_mutable_tree_fill_from_dirtree returns FALSE if mtree isn't
       * empty: in which case we're responsible for merging the trees. */
      if (ostree_mutable_tree_fill_empty_from_dirtree (
              mtree, ostree_repo_file_get_repo (repo_dir),
              ostree_repo_file_tree_get_contents_checksum (repo_dir),
              ostree_repo_file_get_checksum (repo_dir)))
        return TRUE;

      ostree_mutable_tree_set_metadata_checksum (
          mtree, ostree_repo_file_tree_get_metadata_checksum (repo_dir));

      filter_result = OSTREE_REPO_COMMIT_FILTER_ALLOW;
    }
  else
    {
      g_autoptr (GVariant) xattrs = NULL;

      g_autoptr (GFileInfo) child_info = g_file_query_info (
          dir, OSTREE_GIO_FAST_QUERYINFO, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
      if (!child_info)
        return FALSE;

      g_autofree char *relpath = NULL;
      if (modifier != NULL)
        relpath = ptrarray_path_join (path);

      g_autoptr (GFileInfo) modified_info = NULL;
      filter_result = _ostree_repo_commit_modifier_apply (self, modifier, relpath, child_info,
                                                          &modified_info);

      if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
        {
          if (!get_final_xattrs (self, modifier, relpath, child_info, dir, -1, NULL, NULL, &xattrs,
                                 NULL, cancellable, error))
            return FALSE;

          g_autofree guchar *child_file_csum = NULL;
          if (!_ostree_repo_write_directory_meta (self, modified_info, xattrs, &child_file_csum,
                                                  cancellable, error))
            return FALSE;

          g_autofree char *tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
          ostree_mutable_tree_set_metadata_checksum (mtree, tmp_checksum);
        }
    }

  if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
    {
      g_autoptr (GFileEnumerator) dir_enum = NULL;

      dir_enum
          = g_file_enumerate_children ((GFile *)dir, OSTREE_GIO_FAST_QUERYINFO,
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
      if (!dir_enum)
        return FALSE;

      while (TRUE)
        {
          GFileInfo *child_info;

          if (!g_file_enumerator_iterate (dir_enum, &child_info, NULL, cancellable, error))
            return FALSE;
          if (child_info == NULL)
            break;

          if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
            {
              if (!write_dir_entry_to_mtree_internal (self, repo_dir, dir_enum, NULL,
                                                      WRITE_DIR_CONTENT_FLAGS_NONE, child_info,
                                                      mtree, modifier, path, cancellable, error))
                return FALSE;
            }
          else
            {
              if (!write_content_to_mtree_internal (self, repo_dir, dir_enum, NULL,
                                                    WRITE_DIR_CONTENT_FLAGS_NONE, child_info, mtree,
                                                    modifier, path, cancellable, error))
                return FALSE;
            }
        }
    }

  return TRUE;
}

/* Handles the dirmeta for the dir described by src_dfd_iter and then calls
 * write_{dir_entry,content}_to_mtree_internal() for each directory entry. */
static gboolean
write_dfd_iter_to_mtree_internal (OstreeRepo *self, GLnxDirFdIterator *src_dfd_iter,
                                  OstreeMutableTree *mtree, OstreeRepoCommitModifier *modifier,
                                  GPtrArray *path, GCancellable *cancellable, GError **error)
{
  g_autoptr (GFileInfo) modified_info = NULL;
  g_autoptr (GVariant) xattrs = NULL;
  g_autofree guchar *child_file_csum = NULL;
  g_autofree char *relpath = NULL;
  OstreeRepoCommitFilterResult filter_result;
  struct stat dir_stbuf;

  if (!glnx_fstat (src_dfd_iter->fd, &dir_stbuf, error))
    return FALSE;

  {
    g_autoptr (GFileInfo) child_info = _ostree_stbuf_to_gfileinfo (&dir_stbuf);
    if (modifier != NULL)
      {
        relpath = ptrarray_path_join (path);
        filter_result = _ostree_repo_commit_modifier_apply (self, modifier, relpath, child_info,
                                                            &modified_info);
      }
    else
      {
        filter_result = OSTREE_REPO_COMMIT_FILTER_ALLOW;
        modified_info = g_object_ref (child_info);
      }
  }

  if (filter_result == OSTREE_REPO_COMMIT_FILTER_ALLOW)
    {
      if (!get_final_xattrs (self, modifier, relpath, modified_info, NULL, src_dfd_iter->fd, NULL,
                             NULL, &xattrs, NULL, cancellable, error))
        return FALSE;

      if (!_ostree_repo_write_directory_meta (self, modified_info, xattrs, &child_file_csum,
                                              cancellable, error))
        return FALSE;

      g_autofree char *tmp_checksum = ostree_checksum_from_bytes (child_file_csum);
      ostree_mutable_tree_set_metadata_checksum (mtree, tmp_checksum);
    }

  if (filter_result != OSTREE_REPO_COMMIT_FILTER_ALLOW)
    {
      /* Note - early return */
      return TRUE;
    }

  /* See if this dir is on the same device; if so we can adopt (if enabled) */
  WriteDirContentFlags flags = 0;
  if (dir_stbuf.st_dev == self->device)
    flags |= WRITE_DIR_CONTENT_FLAGS_CAN_ADOPT;

  while (TRUE)
    {
      struct dirent *dent;
      if (!glnx_dirfd_iterator_next_dent (src_dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      struct stat stbuf;
      if (!glnx_fstatat (src_dfd_iter->fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW, error))
        return FALSE;

      g_autoptr (GFileInfo) child_info = _ostree_stbuf_to_gfileinfo (&stbuf);
      g_file_info_set_name (child_info, dent->d_name);

      if (S_ISDIR (stbuf.st_mode))
        {
          if (!write_dir_entry_to_mtree_internal (self, NULL, NULL, src_dfd_iter, flags, child_info,
                                                  mtree, modifier, path, cancellable, error))
            return FALSE;

          /* We handled the dir, move onto the next */
          continue;
        }

      if (S_ISREG (stbuf.st_mode))
        ;
      else if (S_ISLNK (stbuf.st_mode))
        {
          if (!ot_readlinkat_gfile_info (src_dfd_iter->fd, dent->d_name, child_info, cancellable,
                                         error))
            return FALSE;
        }
      else
        {
          return glnx_throw (error, "Not a regular file or symlink: %s", dent->d_name);
        }

      /* Write a content object, we handled directories above */
      if (!write_content_to_mtree_internal (self, NULL, NULL, src_dfd_iter, flags, child_info,
                                            mtree, modifier, path, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/**
 * ostree_repo_write_directory_to_mtree:
 * @self: Repo
 * @dir: Path to a directory
 * @mtree: Overlay directory contents into this tree
 * @modifier: (allow-none): Optional modifier
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store objects for @dir and all children into the repository @self,
 * overlaying the resulting filesystem hierarchy into @mtree.
 */
gboolean
ostree_repo_write_directory_to_mtree (OstreeRepo *self, GFile *dir, OstreeMutableTree *mtree,
                                      OstreeRepoCommitModifier *modifier, GCancellable *cancellable,
                                      GError **error)
{

  /* Short cut local files */
  if (g_file_is_native (dir))
    {
      if (!ostree_repo_write_dfd_to_mtree (self, AT_FDCWD, gs_file_get_path_cached (dir), mtree,
                                           modifier, cancellable, error))
        return FALSE;
    }
  else
    {
      _ostree_repo_setup_generate_sizes (self, modifier);

      g_autoptr (GPtrArray) path = g_ptr_array_new ();
      if (!write_directory_to_mtree_internal (self, dir, mtree, modifier, path, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/**
 * ostree_repo_write_dfd_to_mtree:
 * @self: Repo
 * @dfd: Directory file descriptor
 * @path: Path
 * @mtree: Overlay directory contents into this tree
 * @modifier: (allow-none): Optional modifier
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store as objects all contents of the directory referred to by @dfd
 * and @path all children into the repository @self, overlaying the
 * resulting filesystem hierarchy into @mtree.
 */
gboolean
ostree_repo_write_dfd_to_mtree (OstreeRepo *self, int dfd, const char *path,
                                OstreeMutableTree *mtree, OstreeRepoCommitModifier *modifier,
                                GCancellable *cancellable, GError **error)
{
  _ostree_repo_setup_generate_sizes (self, modifier);

  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };
  if (!glnx_dirfd_iterator_init_at (dfd, path, FALSE, &dfd_iter, error))
    return FALSE;

  g_autoptr (GPtrArray) pathbuilder = g_ptr_array_new ();
  if (!write_dfd_iter_to_mtree_internal (self, &dfd_iter, mtree, modifier, pathbuilder, cancellable,
                                         error))
    return FALSE;

  /* And now finally remove the toplevel; see also the handling for this flag in
   * the write_dfd_iter_to_mtree_internal() function. As a special case we don't
   * try to remove `.` (since we'd get EINVAL); that's what's used in
   * rpm-ostree.
   */
  const gboolean delete_after_commit
      = modifier && (modifier->flags & OSTREE_REPO_COMMIT_MODIFIER_FLAGS_CONSUME);
  if (delete_after_commit && !g_str_equal (path, "."))
    {
      if (!glnx_unlinkat (dfd, path, AT_REMOVEDIR, error))
        return FALSE;
    }

  return TRUE;
}

/**
 * ostree_repo_write_mtree:
 * @self: Repo
 * @mtree: Mutable tree
 * @out_file: (out): An #OstreeRepoFile representing @mtree's root.
 * @cancellable: Cancellable
 * @error: Error
 *
 * Write all metadata objects for @mtree to repo; the resulting
 * @out_file points to the %OSTREE_OBJECT_TYPE_DIR_TREE object that
 * the @mtree represented.
 */
gboolean
ostree_repo_write_mtree (OstreeRepo *self, OstreeMutableTree *mtree, GFile **out_file,
                         GCancellable *cancellable, GError **error)
{
  const char *contents_checksum, *metadata_checksum;
  g_autoptr (GFile) ret_file = NULL;

  if (!ostree_mutable_tree_check_error (mtree, error))
    return glnx_prefix_error (error, "mtree");

  metadata_checksum = ostree_mutable_tree_get_metadata_checksum (mtree);
  if (!metadata_checksum)
    return glnx_throw (error, "Can't commit an empty tree");

  contents_checksum = ostree_mutable_tree_get_contents_checksum (mtree);
  if (contents_checksum)
    {
      ret_file = G_FILE (_ostree_repo_file_new_root (self, contents_checksum, metadata_checksum));
    }
  else
    {
      g_autoptr (GHashTable) dir_metadata_checksums = NULL;
      g_autoptr (GHashTable) dir_contents_checksums = NULL;
      g_autoptr (GVariant) serialized_tree = NULL;
      g_autofree guchar *contents_csum = NULL;
      char contents_checksum_buf[OSTREE_SHA256_STRING_LEN + 1];

      dir_contents_checksums = g_hash_table_new_full (
          g_str_hash, g_str_equal, (GDestroyNotify)g_free, (GDestroyNotify)g_free);
      dir_metadata_checksums = g_hash_table_new_full (
          g_str_hash, g_str_equal, (GDestroyNotify)g_free, (GDestroyNotify)g_free);

      GLNX_HASH_TABLE_FOREACH_KV (ostree_mutable_tree_get_subdirs (mtree), const char *, name,
                                  OstreeMutableTree *, child_dir)
        {
          g_autoptr (GFile) child_file = NULL;
          if (!ostree_repo_write_mtree (self, child_dir, &child_file, cancellable, error))
            return FALSE;

          g_hash_table_replace (dir_contents_checksums, g_strdup (name),
                                g_strdup (ostree_repo_file_tree_get_contents_checksum (
                                    OSTREE_REPO_FILE (child_file))));
          g_hash_table_replace (dir_metadata_checksums, g_strdup (name),
                                g_strdup (ostree_repo_file_tree_get_metadata_checksum (
                                    OSTREE_REPO_FILE (child_file))));
        }

      serialized_tree = create_tree_variant_from_hashes (
          ostree_mutable_tree_get_files (mtree), dir_contents_checksums, dir_metadata_checksums);

      if (!ostree_repo_write_metadata (self, OSTREE_OBJECT_TYPE_DIR_TREE, NULL, serialized_tree,
                                       &contents_csum, cancellable, error))
        return FALSE;

      ostree_checksum_inplace_from_bytes (contents_csum, contents_checksum_buf);
      ostree_mutable_tree_set_contents_checksum (mtree, contents_checksum_buf);

      ret_file
          = G_FILE (_ostree_repo_file_new_root (self, contents_checksum_buf, metadata_checksum));
    }

  if (out_file)
    *out_file = g_steal_pointer (&ret_file);
  return TRUE;
}

/**
 * ostree_repo_commit_modifier_new:
 * @flags: Control options for filter
 * @commit_filter: (allow-none): Function that can inspect individual files
 * @user_data: (allow-none): User data
 * @destroy_notify: A #GDestroyNotify
 *
 * Returns: (transfer full): A new commit modifier.
 */
OstreeRepoCommitModifier *
ostree_repo_commit_modifier_new (OstreeRepoCommitModifierFlags flags,
                                 OstreeRepoCommitFilter commit_filter, gpointer user_data,
                                 GDestroyNotify destroy_notify)
{
  OstreeRepoCommitModifier *modifier = g_new0 (OstreeRepoCommitModifier, 1);

  modifier->refcount = 1;
  modifier->flags = flags;
  modifier->filter = commit_filter;
  modifier->user_data = user_data;
  modifier->destroy_notify = destroy_notify;

  return modifier;
}

OstreeRepoCommitModifier *
ostree_repo_commit_modifier_ref (OstreeRepoCommitModifier *modifier)
{
  gint refcount = g_atomic_int_add (&modifier->refcount, 1);
  g_assert (refcount > 0);
  return modifier;
}

void
ostree_repo_commit_modifier_unref (OstreeRepoCommitModifier *modifier)
{
  if (!modifier)
    return;
  if (!g_atomic_int_dec_and_test (&modifier->refcount))
    return;

  if (modifier->destroy_notify)
    modifier->destroy_notify (modifier->user_data);

  if (modifier->xattr_destroy)
    modifier->xattr_destroy (modifier->xattr_user_data);

  g_clear_pointer (&modifier->devino_cache, g_hash_table_unref);

  g_clear_object (&modifier->sepolicy);

  g_free (modifier);
  return;
}

/**
 * ostree_repo_commit_modifier_set_xattr_callback:
 * @modifier: An #OstreeRepoCommitModifier
 * @callback: Function to be invoked, should return extended attributes for path
 * @destroy: Destroy notification
 * @user_data: Data for @callback:
 *
 * If set, this function should return extended attributes to use for
 * the given path.  This is useful for things like ACLs and SELinux,
 * where a build system can label the files as it's committing to the
 * repository.
 */
void
ostree_repo_commit_modifier_set_xattr_callback (OstreeRepoCommitModifier *modifier,
                                                OstreeRepoCommitModifierXattrCallback callback,
                                                GDestroyNotify destroy, gpointer user_data)
{
  modifier->xattr_callback = callback;
  modifier->xattr_destroy = destroy;
  modifier->xattr_user_data = user_data;
}

/**
 * ostree_repo_commit_modifier_set_sepolicy:
 * @modifier: An #OstreeRepoCommitModifier
 * @sepolicy: (allow-none): Policy to use for labeling
 *
 * If @policy is non-%NULL, use it to look up labels to use for
 * "security.selinux" extended attributes.
 *
 * Note that any policy specified this way operates in addition to any
 * extended attributes provided via
 * ostree_repo_commit_modifier_set_xattr_callback().  However if both
 * specify a value for "security.selinux", then the one from the
 * policy wins.
 */
void
ostree_repo_commit_modifier_set_sepolicy (OstreeRepoCommitModifier *modifier,
                                          OstreeSePolicy *sepolicy)
{
  g_clear_object (&modifier->sepolicy);
  modifier->sepolicy = sepolicy ? g_object_ref (sepolicy) : NULL;
}

/**
 * ostree_repo_commit_modifier_set_sepolicy_from_commit:
 * @modifier: Commit modifier
 * @repo: OSTree repo containing @rev
 * @rev: Find SELinux policy from this base commit
 * @cancellable:
 * @error:
 *
 * In many cases, one wants to create a "derived" commit from base commit.
 * SELinux policy labels are part of that base commit.  This API allows
 * one to easily set up SELinux labeling from a base commit.
 *
 * Since: 2020.4
 */
gboolean
ostree_repo_commit_modifier_set_sepolicy_from_commit (OstreeRepoCommitModifier *modifier,
                                                      OstreeRepo *repo, const char *rev,
                                                      GCancellable *cancellable, GError **error)
{
  g_autoptr (OstreeSePolicy) policy
      = ostree_sepolicy_new_from_commit (repo, rev, cancellable, error);
  if (!policy)
    return FALSE;
  ostree_repo_commit_modifier_set_sepolicy (modifier, policy);
  return TRUE;
}

/**
 * ostree_repo_commit_modifier_set_devino_cache:
 * @modifier: Modifier
 * @cache: A hash table caching device,inode to checksums
 *
 * See the documentation for
 * `ostree_repo_devino_cache_new()`.  This function can
 * then be used for later calls to
 * `ostree_repo_write_directory_to_mtree()` to optimize commits.
 *
 * Note if your process has multiple writers, you should use separate
 * `OSTreeRepo` instances if you want to also use this API.
 *
 * This function will add a reference to @cache without copying - you
 * should avoid further mutation of the cache.
 *
 * Since: 2017.13
 */
void
ostree_repo_commit_modifier_set_devino_cache (OstreeRepoCommitModifier *modifier,
                                              OstreeRepoDevInoCache *cache)
{
  modifier->devino_cache = g_hash_table_ref ((GHashTable *)cache);
}

OstreeRepoDevInoCache *
ostree_repo_devino_cache_ref (OstreeRepoDevInoCache *cache)
{
  g_hash_table_ref ((GHashTable *)cache);
  return cache;
}

void
ostree_repo_devino_cache_unref (OstreeRepoDevInoCache *cache)
{
  g_hash_table_unref ((GHashTable *)cache);
}

G_DEFINE_BOXED_TYPE (OstreeRepoDevInoCache, ostree_repo_devino_cache, ostree_repo_devino_cache_ref,
                     ostree_repo_devino_cache_unref);

G_DEFINE_BOXED_TYPE (OstreeRepoCommitModifier, ostree_repo_commit_modifier,
                     ostree_repo_commit_modifier_ref, ostree_repo_commit_modifier_unref);

/* Special case between bare-user and bare-user-only,
 * mostly for https://github.com/flatpak/flatpak/issues/845
 * see below for any more comments.
 */
static gboolean
import_is_bareuser_only_conversion (OstreeRepo *src_repo, OstreeRepo *dest_repo,
                                    OstreeObjectType objtype)
{
  return src_repo->mode == OSTREE_REPO_MODE_BARE_USER
         && dest_repo->mode == OSTREE_REPO_MODE_BARE_USER_ONLY
         && objtype == OSTREE_OBJECT_TYPE_FILE;
}

/* Returns TRUE if we can potentially just call link() to copy an object;
 * if untrusted the repos must be owned by the same uid.
 */
static gboolean
import_via_reflink_is_possible (OstreeRepo *src_repo, OstreeRepo *dest_repo,
                                OstreeObjectType objtype, gboolean trusted)
{
  /* Untrusted pulls require matching ownership */
  if (!trusted && (src_repo->owner_uid != dest_repo->owner_uid))
    return FALSE;
  /* Equal modes are always compatible, and metadata
   * is identical between all modes.
   */
  if (src_repo->mode == dest_repo->mode || OSTREE_OBJECT_TYPE_IS_META (objtype))
    return TRUE;
  /* And now a special case between bare-user and bare-user-only,
   * mostly for https://github.com/flatpak/flatpak/issues/845
   */
  if (import_is_bareuser_only_conversion (src_repo, dest_repo, objtype))
    return TRUE;
  return FALSE;
}

/* Copy the detached metadata for commit @checksum from @source repo
 * to @self.
 */
static gboolean
copy_detached_metadata (OstreeRepo *self, OstreeRepo *source, const char *checksum,
                        GCancellable *cancellable, GError **error)
{
  g_autoptr (GVariant) detached_meta = NULL;
  if (!ostree_repo_read_commit_detached_metadata (source, checksum, &detached_meta, cancellable,
                                                  error))
    return FALSE;

  if (detached_meta)
    {
      if (!ostree_repo_write_commit_detached_metadata (self, checksum, detached_meta, cancellable,
                                                       error))
        return FALSE;
    }

  return TRUE;
}

/* Try to import an object via reflink or just linkat(); returns a value in
 * @out_was_supported if we were able to do it or not.  In this path
 * we're not verifying the checksum.
 */
static gboolean
import_one_object_direct (OstreeRepo *dest_repo, OstreeRepo *src_repo, const char *checksum,
                          OstreeObjectType objtype, gboolean *out_was_supported,
                          GCancellable *cancellable, GError **error)
{
  const char *errprefix
      = glnx_strjoina ("Importing ", checksum, ".", ostree_object_type_to_string (objtype));
  GLNX_AUTO_PREFIX_ERROR (errprefix, error);
  char loose_path_buf[_OSTREE_LOOSE_PATH_MAX];
  _ostree_loose_path (loose_path_buf, checksum, objtype, dest_repo->mode);

  /* hardlinks require the owner to match and to be on the same device */
  const gboolean can_hardlink
      = src_repo->owner_uid == dest_repo->owner_uid && src_repo->device == dest_repo->device;

  /* Find our target dfd */
  int dest_dfd;
  if (dest_repo->commit_stagedir.initialized)
    dest_dfd = dest_repo->commit_stagedir.fd;
  else
    dest_dfd = dest_repo->objects_dir_fd;

  if (!_ostree_repo_ensure_loose_objdir_at (dest_dfd, loose_path_buf, cancellable, error))
    return FALSE;

  gboolean did_hardlink = FALSE;
  if (can_hardlink)
    {
      if (linkat (src_repo->objects_dir_fd, loose_path_buf, dest_dfd, loose_path_buf, 0) != 0)
        {
          if (errno == EEXIST)
            did_hardlink = TRUE;
          else if (errno == EMLINK || errno == EXDEV || errno == EPERM)
            {
              /* EMLINK, EXDEV and EPERM shouldn't be fatal; we just can't do
               * the optimization of hardlinking instead of copying. Fall
               * through below.
               */
            }
          else
            return glnx_throw_errno_prefix (error, "linkat");
        }
      else
        did_hardlink = TRUE;
    }

  /* If we weren't able to hardlink, fall back to a copy (which might be
   * reflinked).
   */
  if (!did_hardlink)
    {
      struct stat stbuf;

      if (!glnx_fstatat (src_repo->objects_dir_fd, loose_path_buf, &stbuf, AT_SYMLINK_NOFOLLOW,
                         error))
        return FALSE;

      /* Let's punt for symlinks right now, it's more complicated */
      if (!S_ISREG (stbuf.st_mode))
        {
          *out_was_supported = FALSE;
          return TRUE;
        }

      /* This is yet another variation of glnx_file_copy_at()
       * that basically just optionally does chown().  Perhaps
       * in the future we should add flags for those things?
       */
      glnx_autofd int src_fd = -1;
      if (!glnx_openat_rdonly (src_repo->objects_dir_fd, loose_path_buf, FALSE, &src_fd, error))
        return FALSE;

      /* Open a tmpfile for dest */
      g_auto (GLnxTmpfile) tmp_dest = {
        0,
      };
      if (!glnx_open_tmpfile_linkable_at (dest_dfd, ".", O_WRONLY | O_CLOEXEC, &tmp_dest, error))
        return FALSE;

      if (glnx_regfile_copy_bytes (src_fd, tmp_dest.fd, (off_t)-1) < 0)
        return glnx_throw_errno_prefix (error, "regfile copy");

      /* Only chown for true bare repos */
      if (dest_repo->mode == OSTREE_REPO_MODE_BARE)
        {
          if (fchown (tmp_dest.fd, stbuf.st_uid, stbuf.st_gid) != 0)
            return glnx_throw_errno_prefix (error, "fchown");
        }

      /* Don't want to copy xattrs for archive repos, nor for
       * bare-user-only.  We also only do this for content
       * objects.
       */
      const gboolean src_is_bare_or_bare_user
          = G_IN_SET (src_repo->mode, OSTREE_REPO_MODE_BARE, OSTREE_REPO_MODE_BARE_USER);
      if (src_is_bare_or_bare_user && !OSTREE_OBJECT_TYPE_IS_META (objtype))
        {
          if (src_repo->mode == OSTREE_REPO_MODE_BARE)
            {
              g_autoptr (GVariant) xattrs = NULL;
              xattrs = ostree_fs_get_all_xattrs (src_fd, cancellable, error);
              if (!xattrs)
                return FALSE;
              if (!glnx_fd_set_all_xattrs (tmp_dest.fd, xattrs, cancellable, error))
                return FALSE;
            }
          else if (dest_repo->mode == OSTREE_REPO_MODE_BARE_USER_ONLY)
            {
              /* Nothing; this is the "bareuser-only conversion case",
               * we don't need to set any xattrs in the dest repo.
               */
            }
          else
            {
              /* And this case must be bare-user → bare-user */
              g_assert (src_repo->mode == OSTREE_REPO_MODE_BARE_USER);
              g_assert (src_repo->mode == dest_repo->mode);

              /* bare-user; we just want ostree.usermeta */
              g_autoptr (GBytes) bytes = glnx_fgetxattr_bytes (src_fd, "user.ostreemeta", error);
              if (bytes == NULL)
                return FALSE;

              if (TEMP_FAILURE_RETRY (fsetxattr (tmp_dest.fd, "user.ostreemeta",
                                                 (char *)g_bytes_get_data (bytes, NULL),
                                                 g_bytes_get_size (bytes), 0))
                  != 0)
                return glnx_throw_errno_prefix (error, "fsetxattr");
            }
        }

      if (fchmod (tmp_dest.fd, stbuf.st_mode & ~S_IFMT) != 0)
        return glnx_throw_errno_prefix (error, "fchmod");

      /* For archive repos, we just let the timestamps be object creation.
       * Otherwise, copy the ostree timestamp value.
       */
      if (_ostree_repo_mode_is_bare (dest_repo->mode))
        {
          struct timespec ts[2];
          ts[0] = stbuf.st_atim;
          ts[1] = stbuf.st_mtim;
          (void)futimens (tmp_dest.fd, ts);
        }

      if (!_ostree_repo_commit_tmpf_final (dest_repo, checksum, objtype, &tmp_dest, cancellable,
                                           error))
        return FALSE;
    }

  if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
    {
      if (!copy_detached_metadata (dest_repo, src_repo, checksum, cancellable, error))
        return FALSE;
    }
  else if (objtype == OSTREE_OBJECT_TYPE_FILE)
    {
      if (!_import_payload_link (dest_repo, src_repo, checksum, cancellable, error))
        return FALSE;
    }
  *out_was_supported = TRUE;
  return TRUE;
}

/* A version of ostree_repo_import_object_from_with_trust()
 * with flags; may make this public API later.
 */
gboolean
_ostree_repo_import_object (OstreeRepo *self, OstreeRepo *source, OstreeObjectType objtype,
                            const char *checksum, OstreeRepoImportFlags flags,
                            GCancellable *cancellable, GError **error)
{
  const gboolean trusted = (flags & _OSTREE_REPO_IMPORT_FLAGS_TRUSTED) > 0;
  /* Implements OSTREE_REPO_PULL_FLAGS_BAREUSERONLY_FILES which was designed for flatpak */
  const gboolean verify_bareuseronly = (flags & _OSTREE_REPO_IMPORT_FLAGS_VERIFY_BAREUSERONLY) > 0;
  /* A special case between bare-user and bare-user-only,
   * mostly for https://github.com/flatpak/flatpak/issues/845
   */
  const gboolean is_bareuseronly_conversion
      = import_is_bareuser_only_conversion (source, self, objtype);
  gboolean try_direct = TRUE;

  /* If we need to do bareuseronly verification, or we're potentially doing a
   * bareuseronly conversion, let's verify those first so we don't complicate
   * the rest of the code below.
   */
  if ((verify_bareuseronly || is_bareuseronly_conversion) && !OSTREE_OBJECT_TYPE_IS_META (objtype))
    {
      g_autoptr (GFileInfo) src_finfo = NULL;
      if (!ostree_repo_load_file (source, checksum, NULL, &src_finfo, NULL, cancellable, error))
        return FALSE;

      if (verify_bareuseronly)
        {
          if (!_ostree_validate_bareuseronly_mode_finfo (src_finfo, checksum, error))
            return FALSE;
        }

      if (is_bareuseronly_conversion)
        {
          switch (g_file_info_get_file_type (src_finfo))
            {
            case G_FILE_TYPE_REGULAR:
              /* This is OK, we'll try a hardlink */
              break;
            case G_FILE_TYPE_SYMBOLIC_LINK:
              /* Symlinks in bare-user are regular files, we can't
               * hardlink them to another repo mode.
               */
              try_direct = FALSE;
              break;
            default:
              g_assert_not_reached ();
              break;
            }
        }
    }

  /* First, let's see if we can import via reflink/hardlink. */
  if (try_direct && import_via_reflink_is_possible (source, self, objtype, trusted))
    {
      /* For local repositories, if the untrusted flag is set, we verify the
       * checksum first. This assumes then that the files are immutable - the
       * above check verified that the owner uids match.
       */
      if (!trusted)
        {
          if (!ostree_repo_fsck_object (source, objtype, checksum, cancellable, error))
            return FALSE;
        }

      gboolean direct_was_supported = FALSE;
      if (!import_one_object_direct (self, source, checksum, objtype, &direct_was_supported,
                                     cancellable, error))
        return FALSE;

      /* If direct import succeeded, we're done! */
      if (direct_was_supported)
        return TRUE;
    }

  /* The more expensive copy path; involves parsing the object.  For
   * example the input might be an archive repo and the destination bare,
   * or vice versa.  Or we may simply need to verify the checksum.
   */

  /* First, do we have the object already? */
  gboolean has_object;
  if (!ostree_repo_has_object (self, objtype, checksum, &has_object, cancellable, error))
    return FALSE;
  /* If we have it, we're done */
  if (has_object)
    {
      if (objtype == OSTREE_OBJECT_TYPE_FILE)
        {
          if (!_import_payload_link (self, source, checksum, cancellable, error))
            return FALSE;
        }
      return TRUE;
    }

  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    {
      /* Metadata object */
      g_autoptr (GVariant) variant = NULL;

      if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
        {
          /* FIXME - cleanup detached metadata if copy below fails */
          if (!copy_detached_metadata (self, source, checksum, cancellable, error))
            return FALSE;
        }

      if (!ostree_repo_load_variant (source, objtype, checksum, &variant, error))
        return FALSE;

      /* Note this one also now verifies structure in the !trusted case */
      g_autofree guchar *real_csum = NULL;
      if (!ostree_repo_write_metadata (self, objtype, checksum, variant,
                                       trusted ? NULL : &real_csum, cancellable, error))
        return FALSE;
    }
  else
    {
      /* Content object */
      guint64 length;
      g_autoptr (GInputStream) object_stream = NULL;

      if (!ostree_repo_load_object_stream (source, objtype, checksum, &object_stream, &length,
                                           cancellable, error))
        return FALSE;

      g_autofree guchar *real_csum = NULL;
      if (!ostree_repo_write_content (self, checksum, object_stream, length,
                                      trusted ? NULL : &real_csum, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static OstreeRepoTransactionStats *
ostree_repo_transaction_stats_copy (OstreeRepoTransactionStats *stats)
{
  return g_memdup2 (stats, sizeof (OstreeRepoTransactionStats));
}

static void
ostree_repo_transaction_stats_free (OstreeRepoTransactionStats *stats)
{
  return g_free (stats);
}

G_DEFINE_BOXED_TYPE (OstreeRepoTransactionStats, ostree_repo_transaction_stats,
                     ostree_repo_transaction_stats_copy, ostree_repo_transaction_stats_free);

gboolean
_ostree_repo_transaction_write_repo_metadata (OstreeRepo *self, GVariant *additional_metadata,
                                              char **out_checksum, GCancellable *cancellable,
                                              GError **error)
{
  g_assert (self != NULL);
  g_assert (OSTREE_IS_REPO (self));
  g_assert (self->in_transaction == TRUE);

  const char *collection_id = ostree_repo_get_collection_id (self);
  if (collection_id == NULL)
    return glnx_throw (error, "Repository must have collection ID to write repo metadata");

  OstreeCollectionRef collection_ref
      = { (gchar *)collection_id, (gchar *)OSTREE_REPO_METADATA_REF };
  g_autofree char *old_checksum = NULL;
  if (!ostree_repo_resolve_rev (self, OSTREE_REPO_METADATA_REF, TRUE, &old_checksum, error))
    return FALSE;

  /* Add bindings to the commit metadata. */
  g_autoptr (GVariantDict) metadata_dict = g_variant_dict_new (additional_metadata);
  g_variant_dict_insert (metadata_dict, OSTREE_COMMIT_META_KEY_COLLECTION_BINDING, "s",
                         collection_ref.collection_id);
  g_variant_dict_insert_value (
      metadata_dict, OSTREE_COMMIT_META_KEY_REF_BINDING,
      g_variant_new_strv ((const gchar *const *)&collection_ref.ref_name, 1));
  g_autoptr (GVariant) metadata = g_variant_dict_end (metadata_dict);

  /* Set up an empty mtree. */
  g_autoptr (OstreeMutableTree) mtree = ostree_mutable_tree_new ();

  glnx_unref_object GFileInfo *fi = g_file_info_new ();
  g_file_info_set_attribute_uint32 (fi, "unix::uid", 0);
  g_file_info_set_attribute_uint32 (fi, "unix::gid", 0);
  g_file_info_set_attribute_uint32 (fi, "unix::mode", (0755 | S_IFDIR));

  g_autoptr (GVariant) dirmeta = ostree_create_directory_metadata (fi, NULL /* xattrs */);

  g_autofree guchar *csum_raw = NULL;
  if (!ostree_repo_write_metadata (self, OSTREE_OBJECT_TYPE_DIR_META, NULL, dirmeta, &csum_raw,
                                   cancellable, error))
    return FALSE;

  g_autofree char *csum = ostree_checksum_from_bytes (csum_raw);
  ostree_mutable_tree_set_metadata_checksum (mtree, csum);

  g_autoptr (OstreeRepoFile) repo_file = NULL;
  if (!ostree_repo_write_mtree (self, mtree, (GFile **)&repo_file, cancellable, error))
    return FALSE;

  g_autofree gchar *new_checksum = NULL;
  if (!ostree_repo_write_commit (self, old_checksum, NULL /* subject */, NULL /* body */, metadata,
                                 repo_file, &new_checksum, cancellable, error))
    return FALSE;

  ostree_repo_transaction_set_collection_ref (self, &collection_ref, new_checksum);

  if (out_checksum != NULL)
    *out_checksum = g_steal_pointer (&new_checksum);

  return TRUE;
}
