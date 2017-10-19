/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

#pragma once

#include <sys/stat.h>
#include <gio/gio.h>
#include <ostree-types.h>

G_BEGIN_DECLS

/**
 * OSTREE_MAX_METADATA_SIZE:
 * 
 * Maximum permitted size in bytes of metadata objects.  This is an
 * arbitrary number, but really, no one should be putting humongous
 * data in metadata.
 */
#define OSTREE_MAX_METADATA_SIZE (10 * 1024 * 1024)

/**
 * OSTREE_MAX_METADATA_WARN_SIZE:
 * 
 * Objects committed above this size will be allowed, but a warning
 * will be emitted.
 */
#define OSTREE_MAX_METADATA_WARN_SIZE (7 * 1024 * 1024)

/**
 * OSTREE_SHA256_DIGEST_LEN:
 *
 * Length of a sha256 digest when expressed as raw bytes
 */
#define OSTREE_SHA256_DIGEST_LEN (32)

/**
 * OSTREE_SHA256_STRING_LEN:
 *
 * Length of a sha256 digest when expressed as a hexadecimal string
 */
#define OSTREE_SHA256_STRING_LEN (64)

/**
 * OstreeObjectType:
 * @OSTREE_OBJECT_TYPE_FILE: Content; regular file, symbolic link
 * @OSTREE_OBJECT_TYPE_DIR_TREE: List of children (trees or files), and metadata
 * @OSTREE_OBJECT_TYPE_DIR_META: Directory metadata
 * @OSTREE_OBJECT_TYPE_COMMIT: Toplevel object, refers to tree and dirmeta for root
 * @OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT: Toplevel object, refers to a deleted commit
 * @OSTREE_OBJECT_TYPE_COMMIT_META: Detached metadata for a commit
 *
 * Enumeration for core object types; %OSTREE_OBJECT_TYPE_FILE is for
 * content, the other types are metadata.
 */
typedef enum {
  OSTREE_OBJECT_TYPE_FILE = 1,                /* .file */
  OSTREE_OBJECT_TYPE_DIR_TREE = 2,            /* .dirtree */
  OSTREE_OBJECT_TYPE_DIR_META = 3,            /* .dirmeta */
  OSTREE_OBJECT_TYPE_COMMIT = 4,              /* .commit */
  OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT = 5,    /* .commit-tombstone */
  OSTREE_OBJECT_TYPE_COMMIT_META = 6,         /* .commitmeta */
} OstreeObjectType;

/**
 * OSTREE_OBJECT_TYPE_IS_META:
 * @t: An #OstreeObjectType
 *
 * Returns: %TRUE if object type is metadata
 */
#define OSTREE_OBJECT_TYPE_IS_META(t) (t >= 2 && t <= 6)

/**
 * OSTREE_OBJECT_TYPE_LAST:
 *
 * Last valid object type; use this to validate ranges.
 */
#define OSTREE_OBJECT_TYPE_LAST OSTREE_OBJECT_TYPE_COMMIT_META

/**
 * OSTREE_DIRMETA_GVARIANT_FORMAT:
 *
 * - u - uid (big-endian)
 * - u - gid (big-endian)
 * - u - mode (big-endian)
 * - a(ayay) - xattrs
 */
#define OSTREE_DIRMETA_GVARIANT_STRING "(uuua(ayay))"
#define OSTREE_DIRMETA_GVARIANT_FORMAT G_VARIANT_TYPE (OSTREE_DIRMETA_GVARIANT_STRING)

/**
 * OSTREE_FILEMETA_GVARIANT_FORMAT:
 *
 * This is not a regular object type, but used as an xattr on a .file object
 * in bare-user repositories. This allows us to store metadata information that we
 * can't store in the real filesystem but we can still use a regular .file object
 * that we can hardlink to in the case of a user-mode checkout.
 *
 * - u - uid (big-endian)
 * - u - gid (big-endian)
 * - u - mode (big-endian)
 * - a(ayay) - xattrs
 */
#define OSTREE_FILEMETA_GVARIANT_STRING "(uuua(ayay))"
#define OSTREE_FILEMETA_GVARIANT_FORMAT G_VARIANT_TYPE (OSTREE_FILEMETA_GVARIANT_STRING)

/**
 * OSTREE_TREE_GVARIANT_FORMAT:
 *
 * - a(say) - array of (filename, checksum) for files
 * - a(sayay) - array of (dirname, tree_checksum, meta_checksum) for directories
 */
#define OSTREE_TREE_GVARIANT_STRING "(a(say)a(sayay))"
#define OSTREE_TREE_GVARIANT_FORMAT G_VARIANT_TYPE (OSTREE_TREE_GVARIANT_STRING)

/**
 * OSTREE_COMMIT_GVARIANT_FORMAT:
 *
 * - a{sv} - Metadata
 * - ay - parent checksum (empty string for initial)
 * - a(say) - Related objects
 * - s - subject
 * - s - body
 * - t - Timestamp in seconds since the epoch (UTC, big-endian)
 * - ay - Root tree contents
 * - ay - Root tree metadata
 */
#define OSTREE_COMMIT_GVARIANT_STRING "(a{sv}aya(say)sstayay)"
#define OSTREE_COMMIT_GVARIANT_FORMAT G_VARIANT_TYPE (OSTREE_COMMIT_GVARIANT_STRING)

/**
 * OSTREE_SUMMARY_GVARIANT_FORMAT:
 *
 * - a(s(taya{sv})) - Map of ref name -> (latest commit size, latest commit checksum, additional metadata), sorted by ref name
 * - a{sv} - Additional metadata, at the current time the following are defined:
 *   - key: "ostree.static-deltas", value: a{sv}, static delta name -> 32 bytes of checksum
 *   - key: "ostree.summary.last-modified", value: t, timestamp (seconds since
 *     the Unix epoch in UTC, big-endian) when the summary was last regenerated
 *     (similar to the HTTP `Last-Modified` header)
 *   - key: "ostree.summary.expires", value: t, timestamp (seconds since the
 *     Unix epoch in UTC, big-endian) after which the summary is considered
 *     stale and should be re-downloaded if possible (similar to the HTTP
 *     `Expires` header)
 *
 * The currently defined keys for the `a{sv}` of additional metadata for each commit are:
 *  - key: `ostree.commit.timestamp`, value: `t`, timestamp (seconds since the
 *    Unix epoch in UTC, big-endian) when the commit was committed
 */
#define OSTREE_SUMMARY_GVARIANT_STRING "(a(s(taya{sv}))a{sv})"
#define OSTREE_SUMMARY_GVARIANT_FORMAT G_VARIANT_TYPE (OSTREE_SUMMARY_GVARIANT_STRING)

#define OSTREE_SUMMARY_SIG_GVARIANT_STRING "a{sv}"
#define OSTREE_SUMMARY_SIG_GVARIANT_FORMAT G_VARIANT_TYPE (OSTREE_SUMMARY_SIG_GVARIANT_STRING)

/**
 * OSTREE_TIMESTAMP:
 *
 * The mtime used for stored files.  This was originally 0, changed to 1 for
 * a few releases, then was reverted due to regressions it introduced from
 * users who had been using zero before.
 */
#define OSTREE_TIMESTAMP (0)

/**
 * OstreeRepoMode:
 * @OSTREE_REPO_MODE_BARE: Files are stored as themselves; checkouts are hardlinks; can only be written as root
 * @OSTREE_REPO_MODE_ARCHIVE: Files are compressed, should be owned by non-root.  Can be served via HTTP.  Since: 2017.12
 * @OSTREE_REPO_MODE_ARCHIVE_Z2: Legacy alias for `OSTREE_REPO_MODE_ARCHIVE`
 * @OSTREE_REPO_MODE_BARE_USER: Files are stored as themselves, except ownership; can be written by user. Hardlinks work only in user checkouts.
 * @OSTREE_REPO_MODE_BARE_USER_ONLY: Same as BARE_USER, but all metadata is not stored, so it can only be used for user checkouts. Does not need xattrs.
 *
 * See the documentation of #OstreeRepo for more information about the
 * possible modes.
 */
typedef enum {
  OSTREE_REPO_MODE_BARE,
  OSTREE_REPO_MODE_ARCHIVE,
  OSTREE_REPO_MODE_ARCHIVE_Z2 = OSTREE_REPO_MODE_ARCHIVE,
  OSTREE_REPO_MODE_BARE_USER,
  OSTREE_REPO_MODE_BARE_USER_ONLY,
} OstreeRepoMode;

/**
 * OSTREE_COMMIT_META_KEY_VERSION:
 *
 * GVariant type `s`. This metadata key is used for version numbers. A freeform
 * string; the intention is that systems using ostree do not interpret this
 * semantically as traditional package managers do.
 *
 * This is the only ostree-defined metadata key that does not start with `ostree.`.
 * Since: 2014.9
 */
#define OSTREE_COMMIT_META_KEY_VERSION "version"
/**
 * OSTREE_COMMIT_META_KEY_ENDOFLIFE_REBASE:
 *
 * GVariant type `s`.  Should contain a refspec defining a new target branch;
 * `ostree admin upgrade` and `OstreeSysrootUpgrader` will automatically initiate
 * a rebase upon encountering this metadata key.
 *
 * Since: 2017.7
 */
#define OSTREE_COMMIT_META_KEY_ENDOFLIFE_REBASE "ostree.endoflife-rebase"
/**
 * OSTREE_COMMIT_META_KEY_ENDOFLIFE:
 *
 * GVariant type `s`. This metadata key is used to display vendor's message
 * when an update stream for a particular branch ends. It usually provides
 * update instructions for the users.
 *
 * Since: 2017.7
 */
#define OSTREE_COMMIT_META_KEY_ENDOFLIFE "ostree.endoflife"
/**
 * OSTREE_COMMIT_META_KEY_SOURCE_TITLE:
 *
 * GVariant type `s`. This should hold a relatively short single line value
 * containing a human-readable "source" for a commit, intended to be displayed
 * near the origin ref.  This is particularly useful for systems that inject
 * content into an OSTree commit from elsewhere - for example, generating from
 * an OCI or qcow2 image. Or if generating from packages, the enabled repository
 * names and their versions.
 *
 * Try to keep this key short (e.g. < 80 characters) and human-readable; if you
 * desire machine readable data, consider injecting separate metadata keys.
 *
 * Since: 2017.13
 */
#define OSTREE_COMMIT_META_KEY_SOURCE_TITLE "ostree.source-title"

/**
 * OSTREE_COMMIT_META_KEY_REF_BINDING:
 *
 * GVariant type `as`; each element is a branch name. If this is added to a
 * commit, `ostree_repo_pull()` will enforce that the commit was retrieved from
 * one of the branch names in this array.  This prevents "sidegrade" attacks.
 * The rationale for having this support multiple branch names is that it helps
 * support a "promotion" model of taking a commit and moving it between development
 * and production branches.
 *
 * Since: 2017.9
 */
#define OSTREE_COMMIT_META_KEY_REF_BINDING "ostree.ref-binding"
/**
 * OSTREE_COMMIT_META_KEY_COLLECTION_BINDING:
 *
 * GVariant type `s`.  If this is added to a commit, `ostree_repo_pull()`
 * will enforce that the commit was retrieved from a repository which has
 * the same collection ID.  See `ostree_repo_set_collection_id()`.
 * This is most useful in concert with `OSTREE_COMMIT_META_KEY_REF_BINDING`,
 * as it more strongly binds the commit to the repository and branch.
 *
 * Since: 2017.9
 */
#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
#define OSTREE_COMMIT_META_KEY_COLLECTION_BINDING "ostree.collection-binding"
#endif

_OSTREE_PUBLIC
const GVariantType *ostree_metadata_variant_type (OstreeObjectType objtype);

_OSTREE_PUBLIC
gboolean ostree_validate_checksum_string (const char *sha256,
                                          GError    **error);

_OSTREE_PUBLIC
guchar *ostree_checksum_to_bytes (const char *checksum);
_OSTREE_PUBLIC
GVariant *ostree_checksum_to_bytes_v (const char *checksum);
_OSTREE_PUBLIC
guchar *ostree_checksum_b64_to_bytes (const char *checksum);
_OSTREE_PUBLIC
void ostree_checksum_b64_inplace_to_bytes (const char *checksum,
                                           guint8     *buf);

_OSTREE_PUBLIC
char * ostree_checksum_from_bytes (const guchar *csum);
_OSTREE_PUBLIC
char * ostree_checksum_from_bytes_v (GVariant *csum_v);
_OSTREE_PUBLIC
char * ostree_checksum_b64_from_bytes (const guchar *csum);

_OSTREE_PUBLIC
void ostree_checksum_inplace_from_bytes (const guchar *csum,
                                         char         *buf);
_OSTREE_PUBLIC
void ostree_checksum_b64_inplace_from_bytes (const guchar *csum,
                                             char         *buf);

_OSTREE_PUBLIC
void ostree_checksum_inplace_to_bytes (const char *checksum,
                                       guchar     *buf);

_OSTREE_PUBLIC
const guchar *ostree_checksum_bytes_peek (GVariant *bytes);

_OSTREE_PUBLIC
const guchar *ostree_checksum_bytes_peek_validate (GVariant *bytes, GError **error);

_OSTREE_PUBLIC
int ostree_cmp_checksum_bytes (const guchar *a, const guchar *b);

_OSTREE_PUBLIC
gboolean ostree_validate_rev (const char *rev, GError **error);

#ifdef OSTREE_ENABLE_EXPERIMENTAL_API
_OSTREE_PUBLIC
gboolean ostree_validate_collection_id (const char *collection_id, GError **error);
#endif /* OSTREE_ENABLE_EXPERIMENTAL_API */

_OSTREE_PUBLIC
gboolean ostree_validate_remote_name (const char *remote_name, GError **error);

_OSTREE_PUBLIC
gboolean ostree_parse_refspec (const char *refspec,
                               char      **out_remote,
                               char      **out_ref,
                               GError    **error);

_OSTREE_PUBLIC
const char * ostree_object_type_to_string (OstreeObjectType objtype);

_OSTREE_PUBLIC
OstreeObjectType ostree_object_type_from_string (const char *str);

_OSTREE_PUBLIC
guint ostree_hash_object_name (gconstpointer a);

_OSTREE_PUBLIC
GVariant *ostree_object_name_serialize (const char *checksum,
                                        OstreeObjectType objtype);

_OSTREE_PUBLIC
void ostree_object_name_deserialize (GVariant         *variant,
                                     const char      **out_checksum,
                                     OstreeObjectType *out_objtype);

_OSTREE_PUBLIC
char * ostree_object_to_string (const char *checksum,
                                OstreeObjectType objtype);

_OSTREE_PUBLIC
void ostree_object_from_string (const char *str,
                                gchar     **out_checksum,
                                OstreeObjectType *out_objtype);

_OSTREE_PUBLIC
gboolean
ostree_content_stream_parse (gboolean                compressed,
                             GInputStream           *input,
                             guint64                 input_length,
                             gboolean                trusted,
                             GInputStream          **out_input,
                             GFileInfo             **out_file_info,
                             GVariant              **out_xattrs,
                             GCancellable           *cancellable,
                             GError                **error);

_OSTREE_PUBLIC
gboolean ostree_content_file_parse (gboolean                compressed,
                                    GFile                  *content_path,
                                    gboolean                trusted,
                                    GInputStream          **out_input,
                                    GFileInfo             **out_file_info,
                                    GVariant              **out_xattrs,
                                    GCancellable           *cancellable,
                                    GError                **error);

_OSTREE_PUBLIC
gboolean ostree_content_file_parse_at (gboolean                compressed,
                                       int                     parent_dfd,
                                       const char             *path,
                                       gboolean                trusted,
                                       GInputStream          **out_input,
                                       GFileInfo             **out_file_info,
                                       GVariant              **out_xattrs,
                                       GCancellable           *cancellable,
                                       GError                **error);

_OSTREE_PUBLIC
gboolean
ostree_raw_file_to_archive_z2_stream (GInputStream       *input,
                                      GFileInfo          *file_info,
                                      GVariant           *xattrs,
                                      GInputStream      **out_input,
                                      GCancellable       *cancellable,
                                      GError            **error);

_OSTREE_PUBLIC
gboolean
ostree_raw_file_to_archive_z2_stream_with_options (GInputStream       *input,
                                                   GFileInfo          *file_info,
                                                   GVariant           *xattrs,
                                                   GVariant           *options,
                                                   GInputStream      **out_input,
                                                   GCancellable       *cancellable,
                                                   GError            **error);

_OSTREE_PUBLIC
gboolean ostree_raw_file_to_content_stream (GInputStream       *input,
                                            GFileInfo          *file_info,
                                            GVariant           *xattrs,
                                            GInputStream      **out_input,
                                            guint64            *out_length,
                                            GCancellable       *cancellable,
                                            GError            **error);

_OSTREE_PUBLIC
gboolean ostree_checksum_file_from_input (GFileInfo        *file_info,
                                          GVariant         *xattrs,
                                          GInputStream     *in,
                                          OstreeObjectType  objtype,
                                          guchar          **out_csum,
                                          GCancellable     *cancellable,
                                          GError          **error);

_OSTREE_PUBLIC
gboolean ostree_checksum_file (GFile             *f,
                               OstreeObjectType   objtype,
                               guchar           **out_csum,
                               GCancellable      *cancellable,
                               GError           **error);

/**
 * OstreeChecksumFlags:
 *
 * Since: 2017.13
 */
typedef enum {
  OSTREE_CHECKSUM_FLAGS_NONE = 0,
  OSTREE_CHECKSUM_FLAGS_IGNORE_XATTRS = (1 << 0),
} OstreeChecksumFlags;

_OSTREE_PUBLIC
gboolean ostree_checksum_file_at (int               dfd,
                                  const char       *path,
                                  struct stat      *stbuf,
                                  OstreeObjectType  objtype,
                                  OstreeChecksumFlags flags,
                                  char            **out_checksum,
                                  GCancellable     *cancellable,
                                  GError          **error);

_OSTREE_PUBLIC
void ostree_checksum_file_async (GFile                 *f,
                                 OstreeObjectType       objtype,
                                 int                    io_priority,
                                 GCancellable          *cancellable,
                                 GAsyncReadyCallback    callback,
                                 gpointer               user_data);

_OSTREE_PUBLIC
gboolean ostree_checksum_file_async_finish (GFile          *f,
                                            GAsyncResult   *result,
                                            guchar        **out_csum,
                                            GError        **error);

_OSTREE_PUBLIC
GVariant *ostree_create_directory_metadata (GFileInfo *dir_info,
                                            GVariant  *xattrs);

/* VALIDATION */

_OSTREE_PUBLIC
gboolean ostree_validate_structureof_objtype (guchar    objtype,
                                              GError   **error);

_OSTREE_PUBLIC
gboolean ostree_validate_structureof_csum_v (GVariant  *checksum,
                                             GError   **error);

_OSTREE_PUBLIC
gboolean ostree_validate_structureof_checksum_string (const char *checksum,
                                                      GError   **error);

_OSTREE_PUBLIC
gboolean ostree_validate_structureof_file_mode (guint32            mode,
                                                GError           **error);

_OSTREE_PUBLIC
gboolean ostree_validate_structureof_commit (GVariant      *commit,
                                             GError       **error);

_OSTREE_PUBLIC
gboolean ostree_validate_structureof_dirtree (GVariant      *dirtree,
                                              GError       **error);

_OSTREE_PUBLIC
gboolean ostree_validate_structureof_dirmeta (GVariant      *dirmeta,
                                              GError       **error);

_OSTREE_PUBLIC
gchar *  ostree_commit_get_parent            (GVariant  *commit_variant);
_OSTREE_PUBLIC
guint64  ostree_commit_get_timestamp         (GVariant  *commit_variant);

_OSTREE_PUBLIC
gboolean ostree_check_version (guint required_year, guint required_release);

G_END_DECLS
