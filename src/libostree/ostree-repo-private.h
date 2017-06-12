/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
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
 */

#pragma once

#include "ostree-repo.h"
#include "ostree-remote-private.h"
#include "libglnx.h"

G_BEGIN_DECLS

#define OSTREE_DELTAPART_VERSION (0)

#define _OSTREE_OBJECT_SIZES_ENTRY_SIGNATURE "ay"

#define _OSTREE_SUMMARY_CACHE_DIR "summaries"
#define _OSTREE_CACHE_DIR "cache"

#define _OSTREE_MAX_OUTSTANDING_FETCHER_REQUESTS 8
#define _OSTREE_MAX_OUTSTANDING_DELTAPART_REQUESTS 2

/* In most cases, writing to disk should be much faster than
 * fetching from the network, so we shouldn't actually hit
 * this. But if using pipelining and e.g. pulling over LAN
 * (or writing to slow media), we can have a runaway
 * situation towards EMFILE.
 * */
#define _OSTREE_MAX_OUTSTANDING_WRITE_REQUESTS 16

/* Well-known keys for the additional metadata field in a summary file. */
#define OSTREE_SUMMARY_LAST_MODIFIED "ostree.summary.last-modified"
#define OSTREE_SUMMARY_EXPIRES "ostree.summary.expires"

/* Well-known keys for the additional metadata field in a commit in a ref entry
 * in a summary file. */
#define OSTREE_COMMIT_TIMESTAMP "ostree.commit.timestamp"

typedef enum {
  OSTREE_REPO_TEST_ERROR_PRE_COMMIT = (1 << 0)
} OstreeRepoTestErrorFlags;

struct OstreeRepoCommitModifier {
  volatile gint refcount;

  OstreeRepoCommitModifierFlags flags;
  OstreeRepoCommitFilter filter;
  gpointer user_data;
  GDestroyNotify destroy_notify;

  OstreeRepoCommitModifierXattrCallback xattr_callback;
  GDestroyNotify xattr_destroy;
  gpointer xattr_user_data;

  OstreeSePolicy *sepolicy;
  GHashTable *devino_cache;
};

/**
 * OstreeRepo:
 *
 * Private instance structure.
 */
struct OstreeRepo {
  GObject parent;

  char *stagedir_prefix;
  int commit_stagedir_fd;
  char *commit_stagedir_name;
  GLnxLockFile commit_stagedir_lock;

  GFile *repodir;
  int    repo_dir_fd;
  int    tmp_dir_fd;
  int    cache_dir_fd;
  char  *cache_dir;
  int objects_dir_fd;
  int uncompressed_objects_dir_fd;
  GFile *sysroot_dir;
  char *remotes_config_dir;

  GHashTable *txn_refs;
  GMutex txn_stats_lock;
  OstreeRepoTransactionStats txn_stats;

  GMutex cache_lock;
  guint dirmeta_cache_refcount;
  /* char * checksum → GVariant * for dirmeta objects, used in the checkout path */
  GHashTable *dirmeta_cache;

  gboolean inited;
  gboolean writable;
  gboolean is_system; /* Was this repo created via ostree_sysroot_get_repo() ? */
  GError *writable_error;
  gboolean in_transaction;
  gboolean disable_fsync;
  gboolean disable_xattrs;
  guint zlib_compression_level;
  GHashTable *loose_object_devino_hash;
  GHashTable *updated_uncompressed_dirs;
  GHashTable *object_sizes;

  uid_t owner_uid;
  uid_t target_owner_uid;
  gid_t target_owner_gid;

  guint test_error_flags; /* OstreeRepoTestErrorFlags */

  GKeyFile *config;
  GHashTable *remotes;
  GMutex remotes_lock;
  OstreeRepoMode mode;
  gboolean enable_uncompressed_cache;
  gboolean generate_sizes;
  guint64 tmp_expiry_seconds;

  OstreeRepo *parent_repo;
};

typedef struct {
  dev_t dev;
  ino_t ino;
  char checksum[OSTREE_SHA256_STRING_LEN+1];
} OstreeDevIno;

/* A MemoryCacheRef is an in-memory cache of objects (currently just DIRMETA).  This can
 * be used when performing an operation that traverses a repository in someway.  Currently,
 * the primary use case is ostree_repo_checkout_at() avoiding lots of duplicate dirmeta
 * lookups.
 */
typedef struct {
  OstreeRepo *repo;
} OstreeRepoMemoryCacheRef;


void
_ostree_repo_memory_cache_ref_init (OstreeRepoMemoryCacheRef *state,
                                    OstreeRepo               *repo);

void
_ostree_repo_memory_cache_ref_destroy (OstreeRepoMemoryCacheRef *state);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(OstreeRepoMemoryCacheRef, _ostree_repo_memory_cache_ref_destroy);

#define OSTREE_REPO_TMPDIR_STAGING "staging-"
#define OSTREE_REPO_TMPDIR_FETCHER "fetcher-"

gboolean
_ostree_repo_allocate_tmpdir (int           tmpdir_dfd,
                              const char   *tmpdir_prefix,
                              char        **tmpdir_name_out,
                              int          *tmpdir_fd_out,
                              GLnxLockFile *file_lock_out,
                              gboolean *    reusing_dir_out,
                              GCancellable *cancellable,
                              GError      **error);

gboolean
_ostree_repo_is_locked_tmpdir (const char *filename);

gboolean
_ostree_repo_try_lock_tmpdir (int            tmpdir_dfd,
                              const char    *tmpdir_name,
                              GLnxLockFile  *file_lock_out,
                              gboolean      *out_did_lock,
                              GError       **error);

gboolean
_ostree_repo_ensure_loose_objdir_at (int             dfd,
                                     const char     *loose_path,
                                     GCancellable   *cancellable,
                                     GError        **error);

gboolean
_ostree_repo_find_object (OstreeRepo           *self,
                          OstreeObjectType      objtype,
                          const char           *checksum,
                          GFile               **out_stored_path,
                          GCancellable         *cancellable,
                          GError             **error);

GFile *
_ostree_repo_get_commit_metadata_loose_path (OstreeRepo        *self,
                                             const char        *checksum);

gboolean
_ostree_repo_has_loose_object (OstreeRepo           *self,
                               const char           *checksum,
                               OstreeObjectType      objtype,
                               gboolean             *out_is_stored,
                               GCancellable         *cancellable,
                               GError             **error);

gboolean
_ostree_repo_write_directory_meta (OstreeRepo   *self,
                                   GFileInfo    *file_info,
                                   GVariant     *xattrs,
                                   guchar      **out_csum,
                                   GCancellable *cancellable,
                                   GError      **error);
gboolean
_ostree_repo_update_refs (OstreeRepo        *self,
                          GHashTable        *refs,
                          GCancellable      *cancellable,
                          GError           **error);

gboolean      
_ostree_repo_file_replace_contents (OstreeRepo    *self,
                                    int            dfd,
                                    const char    *path,
                                    const guint8  *buf,
                                    gsize          len,
                                    GCancellable  *cancellable,
                                    GError       **error);

gboolean      
_ostree_repo_write_ref (OstreeRepo    *self,
                        const char    *remote,
                        const char    *ref,
                        const char    *rev,
                        GCancellable  *cancellable,
                        GError       **error);

OstreeRepoFile *
_ostree_repo_file_new_for_commit (OstreeRepo  *repo,
                                  const char  *commit,
                                  GError     **error);

OstreeRepoFile *
_ostree_repo_file_new_root (OstreeRepo  *repo,
                            const char  *contents_checksum,
                            const char  *metadata_checksum);

gboolean
_ostree_repo_traverse_dirtree_internal (OstreeRepo      *repo,
                                        const char      *dirtree_checksum,
                                        int              recursion_depth,
                                        GHashTable      *inout_reachable,
                                        GHashTable      *inout_content_names,
                                        GCancellable    *cancellable,
                                        GError         **error);

OstreeRepoCommitFilterResult
_ostree_repo_commit_modifier_apply (OstreeRepo               *self,
                                    OstreeRepoCommitModifier *modifier,
                                    const char               *path,
                                    GFileInfo                *file_info,
                                    GFileInfo               **out_modified_info);

gboolean
_ostree_repo_remote_name_is_file (const char *remote_name);

OstreeGpgVerifyResult *
_ostree_repo_gpg_verify_with_metadata (OstreeRepo          *self,
                                       GBytes              *signed_data,
                                       GVariant            *metadata,
                                       const char          *remote_name,
                                       GFile               *keyringdir,
                                       GFile               *extra_keyring,
                                       GCancellable        *cancellable,
                                       GError             **error);

OstreeGpgVerifyResult *
_ostree_repo_verify_commit_internal (OstreeRepo    *self,
                                     const char    *commit_checksum,
                                     const char    *remote_name,
                                     GFile         *keyringdir,
                                     GFile         *extra_keyring,
                                     GCancellable  *cancellable,
                                     GError       **error);

gboolean
_ostree_repo_commit_loose_final (OstreeRepo        *self,
                                 const char        *checksum,
                                 OstreeObjectType   objtype,
                                 int                temp_dfd,
                                 int                fd,
                                 const char        *temp_filename,
                                 GCancellable      *cancellable,
                                 GError           **error);

typedef struct {
  int fd;
  char *temp_filename;
} OstreeRepoContentBareCommit;

gboolean
_ostree_repo_open_content_bare (OstreeRepo          *self,
                                const char          *checksum,
                                guint64              content_len,
                                OstreeRepoContentBareCommit *out_state,
                                GOutputStream      **out_stream,
                                gboolean            *out_have_object,
                                GCancellable        *cancellable,
                                GError             **error);

gboolean
_ostree_repo_commit_trusted_content_bare (OstreeRepo          *self,
                                          const char          *checksum,
                                          OstreeRepoContentBareCommit *state,
                                          guint32              uid,
                                          guint32              gid,
                                          guint32              mode,
                                          GVariant            *xattrs,
                                          GCancellable        *cancellable,
                                          GError             **error);

gboolean
_ostree_repo_read_bare_fd (OstreeRepo           *self,
                           const char           *checksum,
                           int                  *out_fd,
                           GCancellable        *cancellable,
                           GError             **error);

gboolean
_ostree_repo_update_mtime (OstreeRepo        *self,
                           GError           **error);

gboolean
_ostree_repo_add_remote (OstreeRepo   *self,
                         OstreeRemote *remote);
gboolean
_ostree_repo_remove_remote (OstreeRepo   *self,
                            OstreeRemote *remote);
OstreeRemote *
_ostree_repo_get_remote (OstreeRepo  *self,
                         const char  *name,
                         GError     **error);
OstreeRemote *
_ostree_repo_get_remote_inherited (OstreeRepo  *self,
                                   const char  *name,
                                   GError     **error);

G_END_DECLS
