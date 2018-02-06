/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 * Copyright (C) 2015 Red Hat, Inc.
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
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <glib-unix.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <gio/gfiledescriptorbased.h>
#include "libglnx.h"
#include "otutil.h"
#include <glnx-console.h>

#include "ostree-core-private.h"
#include "ostree-sysroot-private.h"
#include "ostree-remote-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-file.h"
#include "ostree-repo-file-enumerator.h"
#include "ostree-gpg-verifier.h"
#include "ostree-repo-static-delta-private.h"
#include "ot-fs-utils.h"
#include "ostree-autocleanups.h"

#include <locale.h>
#include <glib/gstdio.h>
#include <sys/file.h>
#include <sys/statvfs.h>

#define REPO_LOCK_DISABLED (-2)
#define REPO_LOCK_BLOCKING (-1)

/* ABI Size checks for ostree-repo.h, only for LP64 systems;
 * https://en.wikipedia.org/wiki/64-bit_computing#64-bit_data_models
 *
 * To generate this data, I used `pahole` from gdb. More concretely, `gdb --args
 * /usr/bin/ostree`, then `start`, (to ensure debuginfo was loaded), then e.g.
 * `$ pahole OstreeRepoTransactionStats`.
 */
#if __SIZEOF_POINTER__ == 8 && __SIZEOF_LONG__ == 8 && __SIZEOF_INT__ == 4
G_STATIC_ASSERT(sizeof(OstreeRepoTransactionStats) == sizeof(int) * 4 + 8 * 5);
G_STATIC_ASSERT(sizeof(OstreeRepoImportArchiveOptions) == sizeof(int) * 9 + 4 + sizeof(void*) * 8);
G_STATIC_ASSERT(sizeof(OstreeRepoExportArchiveOptions) == sizeof(int) * 9 + 4 + 8 + sizeof(void*) * 8);
G_STATIC_ASSERT(sizeof(OstreeRepoCheckoutAtOptions) ==
                sizeof(OstreeRepoCheckoutMode) + sizeof(OstreeRepoCheckoutOverwriteMode) +
                sizeof(int)*6 +
                sizeof(int)*5 +
                sizeof(int) +
                sizeof(void*)*2 +
                sizeof(int)*6 +
                sizeof(void*)*7);
G_STATIC_ASSERT(sizeof(OstreeRepoCommitTraverseIter) ==
                sizeof(int) + sizeof(int) +
                sizeof(void*) * 10 +
                130 + 6);  /* 6 byte hole */
G_STATIC_ASSERT(sizeof(OstreeRepoPruneOptions) ==
                sizeof(OstreeRepoPruneFlags) +
                4 +
                sizeof(void*) +
                sizeof(int) * 12 +
                sizeof(void*) * 7);
#endif

/**
 * SECTION:ostree-repo
 * @title: OstreeRepo: Content-addressed object store
 * @short_description: A git-like storage system for operating system binaries
 *
 * The #OstreeRepo is like git, a content-addressed object store.
 * Unlike git, it records uid, gid, and extended attributes.
 *
 * There are three possible "modes" for an #OstreeRepo;
 * %OSTREE_REPO_MODE_BARE is very simple - content files are
 * represented exactly as they are, and checkouts are just hardlinks.
 * %OSTREE_REPO_MODE_BARE_USER is similar, except the uid/gids are not
 * set on the files, and checkouts as hardlinks hardlinks work only for user checkouts.
 * A %OSTREE_REPO_MODE_ARCHIVE_Z2 repository in contrast stores
 * content files zlib-compressed.  It is suitable for non-root-owned
 * repositories that can be served via a static HTTP server.
 *
 * Creating an #OstreeRepo does not invoke any file I/O, and thus needs
 * to be initialized, either from an existing contents or with a new
 * repository. If you have an existing repo, use ostree_repo_open()
 * to load it from disk and check its validity. To initialize a new
 * repository in the given filepath, use ostree_repo_create() instead.
 *
 * To store content in the repo, first start a transaction with
 * ostree_repo_prepare_transaction().  Then create a
 * #OstreeMutableTree, and apply functions such as
 * ostree_repo_write_directory_to_mtree() to traverse a physical
 * filesystem and write content, possibly multiple times.
 *
 * Once the #OstreeMutableTree is complete, write all of its metadata
 * with ostree_repo_write_mtree(), and finally create a commit with
 * ostree_repo_write_commit().
 *
 * ## Collection IDs
 *
 * A collection ID is a globally unique identifier which, if set, is used to
 * identify refs from a repository which are mirrored elsewhere, such as in
 * mirror repositories or peer to peer networks.
 *
 * This is separate from the `collection-id` configuration key for a remote, which
 * is used to store the collection ID of the repository that remote points to.
 *
 * The collection ID should only be set on an #OstreeRepo if it is the canonical
 * collection for some refs.
 *
 * A collection ID must be a reverse DNS name, where the domain name is under the
 * control of the curator of the collection, so they can demonstrate ownership
 * of the collection. The later elements in the reverse DNS name can be used to
 * disambiguate between multiple collections from the same curator. For example,
 * `org.exampleos.Main` and `org.exampleos.Apps`. For the complete format of
 * collection IDs, see ostree_validate_collection_id().
 */
typedef struct {
  GObjectClass parent_class;

  void (*gpg_verify_result) (OstreeRepo *self,
                             const char *checksum,
                             OstreeGpgVerifyResult *result);
} OstreeRepoClass;

enum {
  PROP_0,

  PROP_PATH,
  PROP_REMOTES_CONFIG_DIR,
  PROP_SYSROOT_PATH
};

enum {
  GPG_VERIFY_RESULT,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (OstreeRepo, ostree_repo, G_TYPE_OBJECT)

#define SYSCONF_REMOTES SHORTENED_SYSCONFDIR "/ostree/remotes.d"

/* Repository locking
 *
 * To guard against objects being deleted (e.g., prune) while they're in
 * use by another operation is accessing them (e.g., commit), the
 * repository must be locked by concurrent writers.
 *
 * The locking is implemented by maintaining a thread local table of
 * lock stacks per repository. This allows thread safe locking since
 * each thread maintains its own lock stack. See the OstreeRepoLock type
 * below.
 *
 * The actual locking is done using either open file descriptor locks or
 * flock locks. This allows the locking to work with concurrent
 * processes. The lock file is held on the ".lock" file within the
 * repository.
 *
 * The intended usage is to take a shared lock when writing objects or
 * reading objects in critical sections. Exclusive locks are taken when
 * deleting objects.
 *
 * To allow fine grained locking within libostree, the lock is
 * maintained as a stack. The core APIs then push or pop from the stack.
 * When pushing or popping a lock state identical to the existing or
 * next state, the stack is simply updated. Only when upgrading or
 * downgrading the lock (changing to/from unlocked, pushing exclusive on
 * shared or popping exclusive to shared) are actual locking operations
 * performed.
 */

static void
free_repo_lock_table (gpointer data)
{
  GHashTable *lock_table = data;

  if (lock_table != NULL)
    {
      g_debug ("Free lock table");
      g_hash_table_destroy (lock_table);
    }
}

static GPrivate repo_lock_table = G_PRIVATE_INIT (free_repo_lock_table);

typedef struct {
  int fd;
  GQueue stack;
} OstreeRepoLock;

typedef struct {
  guint len;
  int state;
  const char *name;
} OstreeRepoLockInfo;

static void
repo_lock_info (OstreeRepoLock *lock, OstreeRepoLockInfo *out_info)
{
  g_assert (lock != NULL);
  g_assert (out_info != NULL);

  OstreeRepoLockInfo info;
  info.len = g_queue_get_length (&lock->stack);
  if (info.len == 0)
    {
      info.state = LOCK_UN;
      info.name = "unlocked";
    }
  else
    {
      info.state = GPOINTER_TO_INT (g_queue_peek_head (&lock->stack));
      info.name = (info.state == LOCK_EX) ? "exclusive" : "shared";
    }

  *out_info = info;
}

static void
free_repo_lock (gpointer data)
{
  OstreeRepoLock *lock = data;

  if (lock != NULL)
    {
      OstreeRepoLockInfo info;
      repo_lock_info (lock, &info);

      g_debug ("Free lock: state=%s, depth=%u", info.name, info.len);
      g_queue_clear (&lock->stack);
      if (lock->fd >= 0)
        {
          g_debug ("Closing repo lock file");
          (void) close (lock->fd);
        }
      g_free (lock);
    }
}

/* Wrapper to handle flock vs OFD locking based on GLnxLockFile */
static gboolean
do_repo_lock (int fd,
              int flags)
{
  int res;

#ifdef F_OFD_SETLK
  struct flock fl = {
    .l_type = (flags & ~LOCK_NB) == LOCK_EX ? F_WRLCK : F_RDLCK,
    .l_whence = SEEK_SET,
    .l_start = 0,
    .l_len = 0,
  };

  res = TEMP_FAILURE_RETRY (fcntl (fd, (flags & LOCK_NB) ? F_OFD_SETLK : F_OFD_SETLKW, &fl));
#else
  res = -1;
  errno = EINVAL;
#endif

  /* Fallback to flock when OFD locks not available */
  if (res < 0)
    {
      if (errno == EINVAL)
        res = TEMP_FAILURE_RETRY (flock (fd, flags));
      if (res < 0)
        return FALSE;
    }

  return TRUE;
}

/* Wrapper to handle flock vs OFD unlocking based on GLnxLockFile */
static gboolean
do_repo_unlock (int fd,
                int flags)
{
  int res;

#ifdef F_OFD_SETLK
  struct flock fl = {
    .l_type = F_UNLCK,
    .l_whence = SEEK_SET,
    .l_start = 0,
    .l_len = 0,
  };

  res = TEMP_FAILURE_RETRY (fcntl (fd, (flags & LOCK_NB) ? F_OFD_SETLK : F_OFD_SETLKW, &fl));
#else
  res = -1;
  errno = EINVAL;
#endif

  /* Fallback to flock when OFD locks not available */
  if (res < 0)
    {
      if (errno == EINVAL)
        res = TEMP_FAILURE_RETRY (flock (fd, LOCK_UN | flags));
      if (res < 0)
        return FALSE;
    }

  return TRUE;
}

static gboolean
push_repo_lock (OstreeRepo          *self,
                OstreeRepoLockType   lock_type,
                gboolean             blocking,
                GError             **error)
{
  int flags = (lock_type == OSTREE_REPO_LOCK_EXCLUSIVE) ? LOCK_EX : LOCK_SH;
  if (!blocking)
    flags |= LOCK_NB;

  GHashTable *lock_table = g_private_get (&repo_lock_table);
  if (lock_table == NULL)
    {
      g_debug ("Creating repo lock table");
      lock_table = g_hash_table_new_full (NULL, NULL, NULL,
                                          (GDestroyNotify)free_repo_lock);
      g_private_set (&repo_lock_table, lock_table);
    }

  OstreeRepoLock *lock = g_hash_table_lookup (lock_table, self);
  if (lock == NULL)
    {
      lock = g_new0 (OstreeRepoLock, 1);
      g_queue_init (&lock->stack);
      g_debug ("Opening repo lock file");
      lock->fd = TEMP_FAILURE_RETRY (openat (self->repo_dir_fd, ".lock",
                                             O_CREAT | O_RDWR | O_CLOEXEC,
                                             0600));
      if (lock->fd < 0)
        {
          free_repo_lock (lock);
          return glnx_throw_errno_prefix (error,
                                          "Opening lock file %s/.lock failed",
                                          gs_file_get_path_cached (self->repodir));
        }
      g_hash_table_insert (lock_table, self, lock);
    }

  OstreeRepoLockInfo info;
  repo_lock_info (lock, &info);
  g_debug ("Push lock: state=%s, depth=%u", info.name, info.len);

  if (info.state == LOCK_EX)
    {
      g_debug ("Repo already locked exclusively, extending stack");
      g_queue_push_head (&lock->stack, GINT_TO_POINTER (LOCK_EX));
    }
  else
    {
      int next_state = (flags & LOCK_EX) ? LOCK_EX : LOCK_SH;
      const char *next_state_name = (flags & LOCK_EX) ? "exclusive" : "shared";

      g_debug ("Locking repo %s", next_state_name);
      if (!do_repo_lock (lock->fd, flags))
        return glnx_throw_errno_prefix (error, "Locking repo %s failed",
                                        next_state_name);

      g_queue_push_head (&lock->stack, GINT_TO_POINTER (next_state));
    }

  return TRUE;
}

static gboolean
pop_repo_lock (OstreeRepo  *self,
               gboolean     blocking,
               GError     **error)
{
  int flags = blocking ? 0 : LOCK_NB;

  GHashTable *lock_table = g_private_get (&repo_lock_table);
  g_return_val_if_fail (lock_table != NULL, FALSE);

  OstreeRepoLock *lock = g_hash_table_lookup (lock_table, self);
  g_return_val_if_fail (lock != NULL, FALSE);
  g_return_val_if_fail (lock->fd != -1, FALSE);

  OstreeRepoLockInfo info;
  repo_lock_info (lock, &info);
  g_return_val_if_fail (info.len > 0, FALSE);

  g_debug ("Pop lock: state=%s, depth=%u", info.name, info.len);
  if (info.len > 1)
    {
      int next_state = GPOINTER_TO_INT (g_queue_peek_nth (&lock->stack, 1));

      /* Drop back to the previous lock state if it differs */
      if (next_state != info.state)
        {
          /* We should never drop from shared to exclusive */
          g_return_val_if_fail (next_state == LOCK_SH, FALSE);
          g_debug ("Returning lock state to shared");
          if (!do_repo_lock (lock->fd, next_state | flags))
            return glnx_throw_errno_prefix (error,
                                            "Setting repo lock to shared failed");
        }
      else
        g_debug ("Maintaining lock state as %s", info.name);
    }
  else
    {
      /* Lock stack will be empty, unlock */
      g_debug ("Unlocking repo");
      if (!do_repo_unlock (lock->fd, flags))
        return glnx_throw_errno_prefix (error, "Unlocking repo failed");
    }

  g_queue_pop_head (&lock->stack);

  return TRUE;
}

/**
 * ostree_repo_lock_push:
 * @self: a #OstreeRepo
 * @lock_type: the type of lock to acquire
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Takes a lock on the repository and adds it to the lock stack. If @lock_type
 * is %OSTREE_REPO_LOCK_SHARED, a shared lock is taken. If @lock_type is
 * %OSTREE_REPO_LOCK_EXCLUSIVE, an exclusive lock is taken. The actual lock
 * state is only changed when locking a previously unlocked repository or
 * upgrading the lock from shared to exclusive. If the requested lock state is
 * unchanged or would represent a downgrade (exclusive to shared), the lock
 * state is not changed and the stack is simply updated.
 *
 * ostree_repo_lock_push() waits for the lock depending on the repository's
 * lock-timeout configuration. When lock-timeout is -1, a blocking lock is
 * attempted. Otherwise, the lock is taken non-blocking and
 * ostree_repo_lock_push() will sleep synchronously up to lock-timeout seconds
 * attempting to acquire the lock. If the lock cannot be acquired within the
 * timeout, a %G_IO_ERROR_WOULD_BLOCK error is returned.
 *
 * If @self is not writable by the user, then no locking is attempted and
 * %TRUE is returned.
 *
 * Returns: %TRUE on success, otherwise %FALSE with @error set
 * Since: 2017.14
 */
gboolean
ostree_repo_lock_push (OstreeRepo          *self,
                       OstreeRepoLockType   lock_type,
                       GCancellable        *cancellable,
                       GError             **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (OSTREE_IS_REPO (self), FALSE);
  g_return_val_if_fail (self->inited, FALSE);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!self->writable)
    return TRUE;

  g_assert (self->lock_timeout_seconds >= REPO_LOCK_DISABLED);
  if (self->lock_timeout_seconds == REPO_LOCK_DISABLED)
    return TRUE; /* No locking */
  else if (self->lock_timeout_seconds == REPO_LOCK_BLOCKING)
    {
      g_debug ("Pushing lock blocking");
      return push_repo_lock (self, lock_type, TRUE, error);
    }
  else
    {
      /* Convert to unsigned to guard against negative values */
      guint lock_timeout_seconds = self->lock_timeout_seconds;
      guint waited = 0;
      g_debug ("Pushing lock non-blocking with timeout %u",
               lock_timeout_seconds);
      for (;;)
        {
          if (g_cancellable_set_error_if_cancelled (cancellable, error))
            return FALSE;

          g_autoptr(GError) local_error = NULL;
          if (push_repo_lock (self, lock_type, FALSE, &local_error))
            return TRUE;

          if (!g_error_matches (local_error, G_IO_ERROR,
                                G_IO_ERROR_WOULD_BLOCK))
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }

          if (waited >= lock_timeout_seconds)
            {
              g_debug ("Push lock: Could not acquire lock within %u seconds",
                       lock_timeout_seconds);
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }

          /* Sleep 1 second and try again */
          if (waited % 60 == 0)
            {
              guint remaining = lock_timeout_seconds - waited;
              g_debug ("Push lock: Waiting %u more second%s to acquire lock",
                       remaining, (remaining == 1) ? "" : "s");
            }
          waited++;
          sleep (1);
        }
    }
}

/**
 * ostree_repo_lock_pop:
 * @self: a #OstreeRepo
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Remove the current repository lock state from the lock stack. If the lock
 * stack becomes empty, the repository is unlocked. Otherwise, the lock state
 * only changes when transitioning from an exclusive lock back to a shared
 * lock.
 *
 * ostree_repo_lock_pop() waits for the lock depending on the repository's
 * lock-timeout configuration. When lock-timeout is -1, a blocking lock is
 * attempted. Otherwise, the lock is removed non-blocking and
 * ostree_repo_lock_pop() will sleep synchronously up to lock-timeout seconds
 * attempting to remove the lock. If the lock cannot be removed within the
 * timeout, a %G_IO_ERROR_WOULD_BLOCK error is returned.
 *
 * If @self is not writable by the user, then no unlocking is attempted and
 * %TRUE is returned.
 *
 * Returns: %TRUE on success, otherwise %FALSE with @error set
 * Since: 2017.14
 */
gboolean
ostree_repo_lock_pop (OstreeRepo    *self,
                      GCancellable  *cancellable,
                      GError       **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (OSTREE_IS_REPO (self), FALSE);
  g_return_val_if_fail (self->inited, FALSE);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!self->writable)
    return TRUE;

  g_assert (self->lock_timeout_seconds >= REPO_LOCK_DISABLED);
  if (self->lock_timeout_seconds == REPO_LOCK_DISABLED)
    return TRUE;
  else if (self->lock_timeout_seconds == REPO_LOCK_BLOCKING)
    {
      g_debug ("Popping lock blocking");
      return pop_repo_lock (self, TRUE, error);
    }
  else
    {
      /* Convert to unsigned to guard against negative values */
      guint lock_timeout_seconds = self->lock_timeout_seconds;
      guint waited = 0;
      g_debug ("Popping lock non-blocking with timeout %u",
               lock_timeout_seconds);
      for (;;)
        {
          if (g_cancellable_set_error_if_cancelled (cancellable, error))
            return FALSE;

          g_autoptr(GError) local_error = NULL;
          if (pop_repo_lock (self, FALSE, &local_error))
            return TRUE;

          if (!g_error_matches (local_error, G_IO_ERROR,
                                G_IO_ERROR_WOULD_BLOCK))
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }

          if (waited >= lock_timeout_seconds)
            {
              g_debug ("Pop lock: Could not remove lock within %u seconds",
                       lock_timeout_seconds);
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }

          /* Sleep 1 second and try again */
          if (waited % 60 == 0)
            {
              guint remaining = lock_timeout_seconds - waited;
              g_debug ("Pop lock: Waiting %u more second%s to remove lock",
                       remaining, (remaining == 1) ? "" : "s");
            }
          waited++;
          sleep (1);
        }
    }
}

/**
 * ostree_repo_auto_lock_push: (skip)
 * @self: a #OstreeRepo
 * @lock_type: the type of lock to acquire
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Like ostree_repo_lock_push(), but for usage with #OstreeRepoAutoLock.
 * The intended usage is to declare the #OstreeRepoAutoLock with
 * g_autoptr() so that ostree_repo_auto_lock_cleanup() is called when it
 * goes out of scope. This will automatically pop the lock status off
 * the stack if it was acquired successfully.
 *
 * |[<!-- language="C" -->
 * g_autoptr(OstreeRepoAutoLock) lock = NULL;
 * lock = ostree_repo_auto_lock_push (repo, lock_type, cancellable, error);
 * if (!lock)
 *   return FALSE;
 * ]|
 *
 * Returns: @self on success, otherwise %NULL with @error set
 * Since: 2017.14
 */
OstreeRepoAutoLock *
ostree_repo_auto_lock_push (OstreeRepo          *self,
                            OstreeRepoLockType   lock_type,
                            GCancellable        *cancellable,
                            GError             **error)
{
  if (!ostree_repo_lock_push (self, lock_type, cancellable, error))
    return NULL;
  return (OstreeRepoAutoLock *)self;
}

/**
 * ostree_repo_auto_lock_cleanup: (skip)
 * @lock: a #OstreeRepoAutoLock
 *
 * A cleanup handler for use with ostree_repo_auto_lock_push(). If @lock is
 * not %NULL, ostree_repo_lock_pop() will be called on it. If
 * ostree_repo_lock_pop() fails, a critical warning will be emitted.
 *
 * Since: 2017.14
 */
void
ostree_repo_auto_lock_cleanup (OstreeRepoAutoLock *lock)
{
  OstreeRepo *repo = lock;
  if (repo)
    {
      g_autoptr(GError) error = NULL;
      int errsv = errno;

      if (!ostree_repo_lock_pop (repo, NULL, &error))
        g_critical ("Cleanup repo lock failed: %s", error->message);

      errno = errsv;
    }
}

static GFile *
get_remotes_d_dir (OstreeRepo          *self,
                   GFile               *sysroot);

OstreeRemote *
_ostree_repo_get_remote (OstreeRepo  *self,
                         const char  *name,
                         GError     **error)
{
  OstreeRemote *remote = NULL;

  g_return_val_if_fail (name != NULL, NULL);

  g_mutex_lock (&self->remotes_lock);

  remote = g_hash_table_lookup (self->remotes, name);

  if (remote != NULL)
    ostree_remote_ref (remote);
  else
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                 "Remote \"%s\" not found", name);

  g_mutex_unlock (&self->remotes_lock);

  return remote;
}

OstreeRemote *
_ostree_repo_get_remote_inherited (OstreeRepo  *self,
                                   const char  *name,
                                   GError     **error)
{
  g_autoptr(OstreeRemote) remote = NULL;
  g_autoptr(GError) temp_error = NULL;

  remote = _ostree_repo_get_remote (self, name, &temp_error);
  if (remote == NULL)
    {
      if (self->parent_repo != NULL)
        return _ostree_repo_get_remote_inherited (self->parent_repo, name, error);

      g_propagate_error (error, g_steal_pointer (&temp_error));
      return NULL;
    }

  return g_steal_pointer (&remote);
}

gboolean
_ostree_repo_add_remote (OstreeRepo   *self,
                         OstreeRemote *remote)
{
  gboolean already_existed;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (remote != NULL, FALSE);
  g_return_val_if_fail (remote->name != NULL, FALSE);

  g_mutex_lock (&self->remotes_lock);

  already_existed = !g_hash_table_replace (self->remotes, remote->name, ostree_remote_ref (remote));

  g_mutex_unlock (&self->remotes_lock);

  return already_existed;
}

gboolean
_ostree_repo_remove_remote (OstreeRepo   *self,
                            OstreeRemote *remote)
{
  gboolean removed;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (remote != NULL, FALSE);
  g_return_val_if_fail (remote->name != NULL, FALSE);

  g_mutex_lock (&self->remotes_lock);

  removed = g_hash_table_remove (self->remotes, remote->name);

  g_mutex_unlock (&self->remotes_lock);

  return removed;
}

gboolean
_ostree_repo_remote_name_is_file (const char *remote_name)
{
  return g_str_has_prefix (remote_name, "file://");
}

/**
 * ostree_repo_get_remote_option:
 * @self: A OstreeRepo
 * @remote_name: Name
 * @option_name: Option
 * @default_value: (allow-none): Value returned if @option_name is not present
 * @out_value: (out): Return location for value
 * @error: Error
 *
 * OSTree remotes are represented by keyfile groups, formatted like:
 * `[remote "remotename"]`. This function returns a value named @option_name
 * underneath that group, or @default_value if the remote exists but not the
 * option name.  If an error is returned, @out_value will be set to %NULL.
 *
 * Returns: %TRUE on success, otherwise %FALSE with @error set
 */
gboolean
ostree_repo_get_remote_option (OstreeRepo  *self,
                               const char  *remote_name,
                               const char  *option_name,
                               const char  *default_value,
                               char       **out_value,
                               GError     **error)
{
  g_autoptr(OstreeRemote) remote = NULL;
  gboolean ret = FALSE;
  g_autoptr(GError) temp_error = NULL;
  g_autofree char *value = NULL;

  if (_ostree_repo_remote_name_is_file (remote_name))
    {
      *out_value = g_strdup (default_value);
      return TRUE;
    }

  remote = _ostree_repo_get_remote (self, remote_name, &temp_error);
  if (remote != NULL)
    {
      value = g_key_file_get_string (remote->options, remote->group, option_name, &temp_error);
      if (value == NULL)
        {
          if (g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
            {
              /* Note: We ignore errors on the parent because the parent config may not
                 specify this remote, causing a "remote not found" error, but we found
                 the remote at some point, so we need to instead return the default */
              if (self->parent_repo != NULL &&
                  ostree_repo_get_remote_option (self->parent_repo,
                                                 remote_name, option_name,
                                                 default_value,
                                                 out_value,
                                                 NULL))
                return TRUE;

              value = g_strdup (default_value);
              ret = TRUE;
            }
          else
            g_propagate_error (error, g_steal_pointer (&temp_error));
        }
      else
        ret = TRUE;
    }
  else if (self->parent_repo != NULL)
    return ostree_repo_get_remote_option (self->parent_repo,
                                          remote_name, option_name,
                                          default_value,
                                          out_value,
                                          error);
  else
    g_propagate_error (error, g_steal_pointer (&temp_error));

  *out_value = g_steal_pointer (&value);
  return ret;
}

/**
 * ostree_repo_get_remote_list_option:
 * @self: A OstreeRepo
 * @remote_name: Name
 * @option_name: Option
 * @out_value: (out) (array zero-terminated=1): location to store the list
 *            of strings. The list should be freed with
 *            g_strfreev().
 * @error: Error
 *
 * OSTree remotes are represented by keyfile groups, formatted like:
 * `[remote "remotename"]`. This function returns a value named @option_name
 * underneath that group, and returns it as a zero terminated array of strings.
 * If the option is not set, or if an error is returned, @out_value will be set
 * to %NULL.
 *
 * Returns: %TRUE on success, otherwise %FALSE with @error set
 */
gboolean
ostree_repo_get_remote_list_option (OstreeRepo   *self,
                                    const char   *remote_name,
                                    const char   *option_name,
                                    char       ***out_value,
                                    GError      **error)
{
  g_autoptr(OstreeRemote) remote = NULL;
  gboolean ret = FALSE;
  g_autoptr(GError) temp_error = NULL;
  g_auto(GStrv) value = NULL;

  if (_ostree_repo_remote_name_is_file (remote_name))
    {
      *out_value = NULL;
      return TRUE;
    }

  remote = _ostree_repo_get_remote (self, remote_name, &temp_error);
  if (remote != NULL)
    {
      value = g_key_file_get_string_list (remote->options,
                                          remote->group,
                                          option_name,
                                          NULL, &temp_error);

      /* Default value if key not found is always NULL. */
      if (g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
        {
          /* Note: We ignore errors on the parent because the parent config may not
             specify this remote, causing a "remote not found" error, but we found
             the remote at some point, so we need to instead return the default */
          if (self->parent_repo != NULL &&
              ostree_repo_get_remote_list_option (self->parent_repo,
                                                  remote_name, option_name,
                                                  out_value,
                                                  NULL))
            return TRUE;

          ret = TRUE;
        }
      else if (temp_error)
        g_propagate_error (error, g_steal_pointer (&temp_error));
      else
        ret = TRUE;
    }
  else if (self->parent_repo != NULL)
    return ostree_repo_get_remote_list_option (self->parent_repo,
                                               remote_name, option_name,
                                               out_value,
                                               error);
  else
    g_propagate_error (error, g_steal_pointer (&temp_error));

  *out_value = g_steal_pointer (&value);
  return ret;
}

/**
 * ostree_repo_get_remote_boolean_option:
 * @self: A OstreeRepo
 * @remote_name: Name
 * @option_name: Option
 * @default_value: Value returned if @option_name is not present
 * @out_value: (out) : location to store the result.
 * @error: Error
 *
 * OSTree remotes are represented by keyfile groups, formatted like:
 * `[remote "remotename"]`. This function returns a value named @option_name
 * underneath that group, and returns it as a boolean.
 * If the option is not set, @out_value will be set to @default_value. If an
 * error is returned, @out_value will be set to %FALSE.
 *
 * Returns: %TRUE on success, otherwise %FALSE with @error set
 */
gboolean
ostree_repo_get_remote_boolean_option (OstreeRepo  *self,
                                       const char  *remote_name,
                                       const char  *option_name,
                                       gboolean     default_value,
                                       gboolean    *out_value,
                                       GError     **error)
{
  g_autoptr(OstreeRemote) remote = NULL;
  g_autoptr(GError) temp_error = NULL;
  gboolean ret = FALSE;
  gboolean value = FALSE;

  if (_ostree_repo_remote_name_is_file (remote_name))
    {
      *out_value = default_value;
      return TRUE;
    }

  remote = _ostree_repo_get_remote (self, remote_name, &temp_error);
  if (remote != NULL)
    {
      value = g_key_file_get_boolean (remote->options, remote->group, option_name, &temp_error);
      if (temp_error != NULL)
        {
          if (g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
            {
              /* Note: We ignore errors on the parent because the parent config may not
                 specify this remote, causing a "remote not found" error, but we found
                 the remote at some point, so we need to instead return the default */
              if (self->parent_repo != NULL &&
                  ostree_repo_get_remote_boolean_option (self->parent_repo,
                                                         remote_name, option_name,
                                                         default_value,
                                                         out_value,
                                                         NULL))
                return TRUE;

              value = default_value;
              ret = TRUE;
            }
          else
            g_propagate_error (error, g_steal_pointer (&temp_error));
        }
      else
        ret = TRUE;
    }
  else if (self->parent_repo != NULL)
    return ostree_repo_get_remote_boolean_option (self->parent_repo,
                                                  remote_name, option_name,
                                                  default_value,
                                                  out_value,
                                                  error);
  else
    g_propagate_error (error, g_steal_pointer (&temp_error));

  *out_value = value;
  return ret;
}

static void
ostree_repo_finalize (GObject *object)
{
  OstreeRepo *self = OSTREE_REPO (object);

  g_clear_object (&self->parent_repo);

  g_free (self->stagedir_prefix);
  g_clear_object (&self->repodir_fdrel);
  g_clear_object (&self->repodir);
  glnx_close_fd (&self->repo_dir_fd);
  glnx_tmpdir_unset (&self->commit_stagedir);
  glnx_release_lock_file (&self->commit_stagedir_lock);
  glnx_close_fd (&self->tmp_dir_fd);
  glnx_close_fd (&self->cache_dir_fd);
  glnx_close_fd (&self->objects_dir_fd);
  glnx_close_fd (&self->uncompressed_objects_dir_fd);
  g_clear_object (&self->sysroot_dir);
  g_weak_ref_clear (&self->sysroot);
  g_free (self->remotes_config_dir);

  if (self->loose_object_devino_hash)
    g_hash_table_destroy (self->loose_object_devino_hash);
  if (self->updated_uncompressed_dirs)
    g_hash_table_destroy (self->updated_uncompressed_dirs);
  if (self->config)
    g_key_file_free (self->config);
  g_clear_pointer (&self->txn.refs, g_hash_table_destroy);
  g_clear_pointer (&self->txn.collection_refs, g_hash_table_destroy);
  g_clear_error (&self->writable_error);
  g_clear_pointer (&self->object_sizes, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&self->dirmeta_cache, (GDestroyNotify) g_hash_table_unref);
  g_mutex_clear (&self->cache_lock);
  g_mutex_clear (&self->txn_lock);
  g_free (self->collection_id);

  g_clear_pointer (&self->remotes, g_hash_table_destroy);
  g_mutex_clear (&self->remotes_lock);

  GHashTable *lock_table = g_private_get (&repo_lock_table);
  if (lock_table)
    {
      g_hash_table_remove (lock_table, self);
      if (g_hash_table_size (lock_table) == 0)
        g_private_replace (&repo_lock_table, NULL);
    }

  G_OBJECT_CLASS (ostree_repo_parent_class)->finalize (object);
}

static void
ostree_repo_set_property(GObject         *object,
			   guint            prop_id,
			   const GValue    *value,
			   GParamSpec      *pspec)
{
  OstreeRepo *self = OSTREE_REPO (object);

  switch (prop_id)
    {
    case PROP_PATH:
      self->repodir = g_value_dup_object (value);
      break;
    case PROP_SYSROOT_PATH:
      self->sysroot_dir = g_value_dup_object (value);
      break;
    case PROP_REMOTES_CONFIG_DIR:
      self->remotes_config_dir = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_repo_get_property(GObject         *object,
			   guint            prop_id,
			   GValue          *value,
			   GParamSpec      *pspec)
{
  OstreeRepo *self = OSTREE_REPO (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_object (value, self->repodir);
      break;
    case PROP_SYSROOT_PATH:
      g_value_set_object (value, self->sysroot_dir);
      break;
    case PROP_REMOTES_CONFIG_DIR:
      g_value_set_string (value, self->remotes_config_dir);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_repo_class_init (OstreeRepoClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = ostree_repo_get_property;
  object_class->set_property = ostree_repo_set_property;
  object_class->finalize = ostree_repo_finalize;

  /**
   * OstreeRepo:path:
   *
   * Path to repository.  Note that if this repository was created
   * via `ostree_repo_new_at()`, this value will refer to a value in
   * the Linux kernel's `/proc/self/fd` directory.  Generally, you
   * should avoid using this property at all; you can gain a reference
   * to the repository's directory fd via `ostree_repo_get_dfd()` and
   * use file-descriptor relative operations.
   */
  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_object ("path", "Path", "Path",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  /**
   * OstreeRepo:sysroot-path:
   *
   * A system using libostree for the host has a "system" repository; this
   * property will be set for repositories referenced via
   * `ostree_sysroot_repo()` for example.
   *
   * You should avoid using this property; if your code is operating
   * on a system repository, use `OstreeSysroot` and access the repository
   * object via `ostree_sysroot_repo()`.
   */
  g_object_class_install_property (object_class,
                                   PROP_SYSROOT_PATH,
                                   g_param_spec_object ("sysroot-path",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  /**
   * OstreeRepo:remotes-config-dir:
   *
   * Path to directory containing remote definitions.  The default is `NULL`.
   * If a `sysroot-path` property is defined, this value will default to
   * `${sysroot_path}/etc/ostree/remotes.d`.
   *
   * This value will only be used for system repositories.
   */
  g_object_class_install_property (object_class,
                                   PROP_REMOTES_CONFIG_DIR,
                                   g_param_spec_string ("remotes-config-dir",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  /**
   * OstreeRepo::gpg-verify-result:
   * @self: an #OstreeRepo
   * @checksum: checksum of the signed object
   * @result: an #OstreeGpgVerifyResult
   *
   * Emitted during a pull operation upon GPG verification (if enabled).
   * Applications can connect to this signal to output the verification
   * results if desired.
   *
   * The signal will be emitted from whichever #GMainContext is the
   * thread-default at the point when ostree_repo_pull_with_options()
   * is called.
   */
  signals[GPG_VERIFY_RESULT] = g_signal_new ("gpg-verify-result",
                                             OSTREE_TYPE_REPO,
                                             G_SIGNAL_RUN_LAST,
                                             G_STRUCT_OFFSET (OstreeRepoClass, gpg_verify_result),
                                             NULL, NULL, NULL,
                                             G_TYPE_NONE, 2,
                                             G_TYPE_STRING,
                                             OSTREE_TYPE_GPG_VERIFY_RESULT);
}

static void
ostree_repo_init (OstreeRepo *self)
{
  static gsize gpgme_initialized;
  const GDebugKey test_error_keys[] = {
    { "pre-commit", OSTREE_REPO_TEST_ERROR_PRE_COMMIT },
  };

  if (g_once_init_enter (&gpgme_initialized))
    {
      gpgme_check_version (NULL);
      gpgme_set_locale (NULL, LC_CTYPE, setlocale (LC_CTYPE, NULL));
      g_once_init_leave (&gpgme_initialized, 1);
    }

  self->test_error_flags = g_parse_debug_string (g_getenv ("OSTREE_REPO_TEST_ERROR"),
                                                 test_error_keys, G_N_ELEMENTS (test_error_keys));

  g_mutex_init (&self->cache_lock);
  g_mutex_init (&self->txn_lock);

  self->remotes = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         (GDestroyNotify) NULL,
                                         (GDestroyNotify) ostree_remote_unref);
  g_mutex_init (&self->remotes_lock);

  self->repo_dir_fd = -1;
  self->cache_dir_fd = -1;
  self->tmp_dir_fd = -1;
  self->objects_dir_fd = -1;
  self->uncompressed_objects_dir_fd = -1;
  self->sysroot_kind = OSTREE_REPO_SYSROOT_KIND_UNKNOWN;
}

/**
 * ostree_repo_new:
 * @path: Path to a repository
 *
 * Returns: (transfer full): An accessor object for an OSTree repository located at @path
 */
OstreeRepo*
ostree_repo_new (GFile *path)
{
  return g_object_new (OSTREE_TYPE_REPO, "path", path, NULL);
}

static OstreeRepo *
repo_open_at_take_fd (int *dfd,
                      GCancellable *cancellable,
                      GError **error)
{
  g_autoptr(OstreeRepo) repo = g_object_new (OSTREE_TYPE_REPO, NULL);
  repo->repo_dir_fd = glnx_steal_fd (dfd);

  if (!ostree_repo_open (repo, cancellable, error))
    return NULL;
  return g_steal_pointer (&repo);
}

/**
 * ostree_repo_open_at:
 * @dfd: Directory fd
 * @path: Path
 *
 * This combines ostree_repo_new() (but using fd-relative access) with
 * ostree_repo_open().  Use this when you know you should be operating on an
 * already extant repository.  If you want to create one, use ostree_repo_create_at().
 *
 * Returns: (transfer full): An accessor object for an OSTree repository located at @dfd + @path
 */
OstreeRepo*
ostree_repo_open_at (int           dfd,
                     const char   *path,
                     GCancellable *cancellable,
                     GError      **error)
{
  glnx_autofd int repo_dfd = -1;
  if (!glnx_opendirat (dfd, path, TRUE, &repo_dfd, error))
    return NULL;

  return repo_open_at_take_fd (&repo_dfd, cancellable, error);
}

static GFile *
get_default_repo_path (GFile *sysroot_path)
{
  if (sysroot_path == NULL)
    sysroot_path = _ostree_get_default_sysroot_path ();

  return g_file_resolve_relative_path (sysroot_path, "ostree/repo");
}

/**
 * ostree_repo_new_for_sysroot_path:
 * @repo_path: Path to a repository
 * @sysroot_path: Path to the system root
 *
 * Creates a new #OstreeRepo instance, taking the system root path explicitly
 * instead of assuming "/".
 *
 * Returns: (transfer full): An accessor object for the OSTree repository located at @repo_path.
 */
OstreeRepo *
ostree_repo_new_for_sysroot_path (GFile *repo_path,
                                  GFile *sysroot_path)
{
  return g_object_new (OSTREE_TYPE_REPO, "path", repo_path, "sysroot-path", sysroot_path, NULL);
}

/**
 * ostree_repo_new_default:
 *
 * If the current working directory appears to be an OSTree
 * repository, create a new #OstreeRepo object for accessing it.
 * Otherwise use the path in the OSTREE_REPO environment variable
 * (if defined) or else the default system repository located at
 * /ostree/repo.
 *
 * Returns: (transfer full): An accessor object for an OSTree repository located at /ostree/repo
 */
OstreeRepo*
ostree_repo_new_default (void)
{
  if (g_file_test ("objects", G_FILE_TEST_IS_DIR)
      && g_file_test ("config", G_FILE_TEST_IS_REGULAR))
    {
      g_autoptr(GFile) cwd = g_file_new_for_path (".");
      return ostree_repo_new (cwd);
    }
  else
    {
      const char *envvar = g_getenv ("OSTREE_REPO");
      g_autoptr(GFile) repo_path = NULL;

      if (envvar == NULL || *envvar == '\0')
        repo_path = get_default_repo_path (NULL);
      else
        repo_path = g_file_new_for_path (envvar);

      return ostree_repo_new (repo_path);
    }
}

/**
 * ostree_repo_is_system:
 * @repo: Repository
 *
 * Returns: %TRUE if this repository is the root-owned system global repository
 */
gboolean
ostree_repo_is_system (OstreeRepo   *repo)
{
  g_return_val_if_fail (OSTREE_IS_REPO (repo), FALSE);

  /* If we were created via ostree_sysroot_get_repo(), we know the answer is yes
   * without having to compare file paths.
   */
  if (repo->sysroot_kind == OSTREE_REPO_SYSROOT_KIND_VIA_SYSROOT ||
      repo->sysroot_kind == OSTREE_REPO_SYSROOT_KIND_IS_SYSROOT_OSTREE)
    return TRUE;

  /* No sysroot_dir set?  Not a system repo then. */
  if (!repo->sysroot_dir)
    return FALSE;

  /* If we created via ostree_repo_new(), we'll have a repo path.  Compare
   * it to the sysroot path in that case.
   */
  if (repo->repodir)
    {
      g_autoptr(GFile) default_repo_path = get_default_repo_path (repo->sysroot_dir);
      return g_file_equal (repo->repodir, default_repo_path);
    }
  /* Otherwise, not a system repo */
  return FALSE;
}

/**
 * ostree_repo_is_writable:
 * @self: Repo
 * @error: a #GError
 *
 * Returns whether the repository is writable by the current user.
 * If the repository is not writable, the @error indicates why.
 *
 * Returns: %TRUE if this repository is writable
 */
gboolean
ostree_repo_is_writable (OstreeRepo *self,
                         GError **error)
{
  g_return_val_if_fail (self->inited, FALSE);

  if (error != NULL && self->writable_error != NULL)
    *error = g_error_copy (self->writable_error);

  return self->writable;
}

/**
 * _ostree_repo_update_mtime:
 * @self: Repo
 * @error: a #GError
 *
 * Bump the mtime of the repository so that programs
 * can detect that the refs have updated.
 */
gboolean
_ostree_repo_update_mtime (OstreeRepo        *self,
                           GError           **error)
{
  if (futimens (self->repo_dir_fd, NULL) != 0)
    {
      glnx_set_prefix_error_from_errno (error, "%s", "futimens");
      return FALSE;
    }
  return TRUE;
}

/**
 * ostree_repo_get_config:
 * @self:
 *
 * Returns: (transfer none): The repository configuration; do not modify
 */
GKeyFile *
ostree_repo_get_config (OstreeRepo *self)
{
  g_return_val_if_fail (self->inited, NULL);

  return self->config;
}

/**
 * ostree_repo_copy_config:
 * @self:
 *
 * Returns: (transfer full): A newly-allocated copy of the repository config
 */
GKeyFile *
ostree_repo_copy_config (OstreeRepo *self)
{
  GKeyFile *copy;
  char *data;
  gsize len;

  g_return_val_if_fail (self->inited, NULL);

  copy = g_key_file_new ();
  data = g_key_file_to_data (self->config, &len, NULL);
  if (!g_key_file_load_from_data (copy, data, len, 0, NULL))
    g_assert_not_reached ();
  g_free (data);
  return copy;
}

/**
 * ostree_repo_write_config:
 * @self: Repo
 * @new_config: Overwrite the config file with this data.  Do not change later!
 * @error: a #GError
 *
 * Save @new_config in place of this repository's config file.  Note
 * that @new_config should not be modified after - this function
 * simply adds a reference.
 */
gboolean
ostree_repo_write_config (OstreeRepo *self,
                          GKeyFile   *new_config,
                          GError    **error)
{
  g_return_val_if_fail (self->inited, FALSE);

  /* Ensure that any remotes in the new config aren't defined in a
   * separate config file.
   */
  gsize num_groups;
  g_auto(GStrv) groups = g_key_file_get_groups (new_config, &num_groups);
  for (gsize i = 0; i < num_groups; i++)
    {
      g_autoptr(OstreeRemote) new_remote = ostree_remote_new_from_keyfile (new_config, groups[i]);
      if (new_remote != NULL)
        {
          g_autoptr(GError) local_error = NULL;

          g_autoptr(OstreeRemote) cur_remote =
            _ostree_repo_get_remote (self, new_remote->name, &local_error);
          if (cur_remote == NULL)
            {
              if (!g_error_matches (local_error, G_IO_ERROR,
                                    G_IO_ERROR_NOT_FOUND))
                {
                  g_propagate_error (error, g_steal_pointer (&local_error));
                  return FALSE;
                }
            }
          else if (cur_remote->file != NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                           "Remote \"%s\" already defined in %s",
                           new_remote->name,
                           gs_file_get_path_cached (cur_remote->file));
              return FALSE;
            }
        }
    }

  gsize len;
  g_autofree char *data = g_key_file_to_data (new_config, &len, error);
  if (!glnx_file_replace_contents_at (self->repo_dir_fd, "config",
                                      (guint8*)data, len, 0,
                                      NULL, error))
    return FALSE;

  g_key_file_free (self->config);
  self->config = g_key_file_new ();
  if (!g_key_file_load_from_data (self->config, data, len, 0, error))
    return FALSE;

  return TRUE;
}

/* Bind a subset of an a{sv} to options in a given GKeyfile section */
static void
keyfile_set_from_vardict (GKeyFile     *keyfile,
                          const char   *section,
                          GVariant     *vardict)
{
  GVariantIter viter;
  const char *key;
  GVariant *val;

  g_variant_iter_init (&viter, vardict);
  while (g_variant_iter_loop (&viter, "{&s@v}", &key, &val))
    {
      g_autoptr(GVariant) child = g_variant_get_variant (val);
      if (g_variant_is_of_type (child, G_VARIANT_TYPE_STRING))
        g_key_file_set_string (keyfile, section, key, g_variant_get_string (child, NULL));
      else if (g_variant_is_of_type (child, G_VARIANT_TYPE_BOOLEAN))
        g_key_file_set_boolean (keyfile, section, key, g_variant_get_boolean (child));
      else if (g_variant_is_of_type (child, G_VARIANT_TYPE_STRING_ARRAY))
        {
          gsize len;
          g_autofree const gchar **strv_child = g_variant_get_strv (child, &len);
          g_key_file_set_string_list (keyfile, section, key, strv_child, len);
        }
      else
        g_critical ("Unhandled type '%s' in " G_GNUC_FUNCTION,
                    (char*)g_variant_get_type (child));
    }
}

static gboolean
impl_repo_remote_add (OstreeRepo     *self,
                      GFile          *sysroot,
                      gboolean        if_not_exists,
                      const char     *name,
                      const char     *url,
                      GVariant       *options,
                      GCancellable   *cancellable,
                      GError        **error)
{
  g_return_val_if_fail (name != NULL, FALSE);
  g_return_val_if_fail (url != NULL, FALSE);
  g_return_val_if_fail (options == NULL || g_variant_is_of_type (options, G_VARIANT_TYPE ("a{sv}")), FALSE);

  if (!ostree_validate_remote_name (name, error))
    return FALSE;

  g_autoptr(OstreeRemote) remote = _ostree_repo_get_remote (self, name, NULL);
  if (remote != NULL && if_not_exists)
    {
      /* Note early return */
      return TRUE;
    }
  else if (remote != NULL)
    {
      return glnx_throw (error,
                         "Remote configuration for \"%s\" already exists: %s",
                         name, remote->file ? gs_file_get_path_cached (remote->file) : "(in config)");
    }

  remote = ostree_remote_new (name);

  /* Only add repos in remotes.d if the repo option
   * add-remotes-config-dir is true. This is the default for system
   * repos.
   */
  g_autoptr(GFile) etc_ostree_remotes_d = get_remotes_d_dir (self, sysroot);
  if (etc_ostree_remotes_d && self->add_remotes_config_dir)
    {
      g_autoptr(GError) local_error = NULL;

      if (!g_file_make_directory_with_parents (etc_ostree_remotes_d,
                                               cancellable, &local_error))
        {
          if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            {
              g_clear_error (&local_error);
            }
          else
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }
        }

      g_autofree char *basename = g_strconcat (name, ".conf", NULL);
      remote->file = g_file_get_child (etc_ostree_remotes_d, basename);
    }

  if (g_str_has_prefix (url, "metalink="))
    g_key_file_set_string (remote->options, remote->group, "metalink", url + strlen ("metalink="));
  else
    g_key_file_set_string (remote->options, remote->group, "url", url);

  if (options)
    keyfile_set_from_vardict (remote->options, remote->group, options);

  if (remote->file != NULL)
    {
      gsize length;
      g_autofree char *data = g_key_file_to_data (remote->options, &length, NULL);

      if (!g_file_replace_contents (remote->file,
                                    data, length,
                                    NULL, FALSE, 0, NULL,
                                    cancellable, error))
        return FALSE;
    }
  else
    {
      g_autoptr(GKeyFile) config = NULL;

      config = ostree_repo_copy_config (self);
      ot_keyfile_copy_group (remote->options, config, remote->group);

      if (!ostree_repo_write_config (self, config, error))
        return FALSE;
    }

  _ostree_repo_add_remote (self, remote);

  return TRUE;
}

/**
 * ostree_repo_remote_add:
 * @self: Repo
 * @name: Name of remote
 * @url: URL for remote (if URL begins with metalink=, it will be used as such)
 * @options: (allow-none): GVariant of type a{sv}
 * @cancellable: Cancellable
 * @error: Error
 *
 * Create a new remote named @name pointing to @url.  If @options is
 * provided, then it will be mapped to #GKeyFile entries, where the
 * GVariant dictionary key is an option string, and the value is
 * mapped as follows:
 *   * s: g_key_file_set_string()
 *   * b: g_key_file_set_boolean()
 *   * as: g_key_file_set_string_list()
 *
 */
gboolean
ostree_repo_remote_add (OstreeRepo     *self,
                        const char     *name,
                        const char     *url,
                        GVariant       *options,
                        GCancellable   *cancellable,
                        GError        **error)
{
  return impl_repo_remote_add (self, NULL, FALSE, name, url, options,
                               cancellable, error);
}

static gboolean
impl_repo_remote_delete (OstreeRepo     *self,
                         GFile          *sysroot,
                         gboolean        if_exists,
                         const char     *name,
                         GCancellable   *cancellable,
                         GError        **error)
{
  g_return_val_if_fail (name != NULL, FALSE);

  if (!ostree_validate_remote_name (name, error))
    return FALSE;

  g_autoptr(OstreeRemote) remote = NULL;
  if (if_exists)
    {
      remote = _ostree_repo_get_remote (self, name, NULL);
      if (!remote)
        {
          /* Note early return */
          return TRUE;
        }
    }
  else
    remote = _ostree_repo_get_remote (self, name, error);

  if (remote == NULL)
    return FALSE;

  if (remote->file != NULL)
    {
      if (!glnx_unlinkat (AT_FDCWD, gs_file_get_path_cached (remote->file), 0, error))
        return FALSE;
    }
  else
    {
      g_autoptr(GKeyFile) config = ostree_repo_copy_config (self);

      /* XXX Not sure it's worth failing if the group to remove
       *     isn't found.  It's the end result we want, after all. */
      if (g_key_file_remove_group (config, remote->group, NULL))
        {
          if (!ostree_repo_write_config (self, config, error))
            return FALSE;
        }
    }

  /* Delete the remote's keyring file, if it exists. */
  if (!ot_ensure_unlinked_at (self->repo_dir_fd, remote->keyring, error))
    return FALSE;

  _ostree_repo_remove_remote (self, remote);

  return TRUE;
}

/**
 * ostree_repo_remote_delete:
 * @self: Repo
 * @name: Name of remote
 * @cancellable: Cancellable
 * @error: Error
 *
 * Delete the remote named @name.  It is an error if the provided
 * remote does not exist.
 *
 */
gboolean
ostree_repo_remote_delete (OstreeRepo     *self,
                           const char     *name,
                           GCancellable   *cancellable,
                           GError        **error)
{
  return impl_repo_remote_delete (self, NULL, FALSE, name, cancellable, error);
}

/**
 * ostree_repo_remote_change:
 * @self: Repo
 * @sysroot: (allow-none): System root
 * @changeop: Operation to perform
 * @name: Name of remote
 * @url: URL for remote (if URL begins with metalink=, it will be used as such)
 * @options: (allow-none): GVariant of type a{sv}
 * @cancellable: Cancellable
 * @error: Error
 *
 * A combined function handling the equivalent of
 * ostree_repo_remote_add(), ostree_repo_remote_delete(), with more
 * options.
 *
 *
 */
gboolean
ostree_repo_remote_change (OstreeRepo     *self,
                           GFile          *sysroot,
                           OstreeRepoRemoteChange changeop,
                           const char     *name,
                           const char     *url,
                           GVariant       *options,
                           GCancellable   *cancellable,
                           GError        **error)
{
  switch (changeop)
    {
    case OSTREE_REPO_REMOTE_CHANGE_ADD:
      return impl_repo_remote_add (self, sysroot, FALSE, name, url, options,
                                   cancellable, error);
    case OSTREE_REPO_REMOTE_CHANGE_ADD_IF_NOT_EXISTS:
      return impl_repo_remote_add (self, sysroot, TRUE, name, url, options,
                                   cancellable, error);
    case OSTREE_REPO_REMOTE_CHANGE_DELETE:
      return impl_repo_remote_delete (self, sysroot, FALSE, name,
                                      cancellable, error);
    case OSTREE_REPO_REMOTE_CHANGE_DELETE_IF_EXISTS:
      return impl_repo_remote_delete (self, sysroot, TRUE, name,
                                      cancellable, error);
    }
  g_assert_not_reached ();
}

static void
_ostree_repo_remote_list (OstreeRepo *self,
                          GHashTable *out)
{
  GHashTableIter iter;
  gpointer key, value;

  g_mutex_lock (&self->remotes_lock);

  g_hash_table_iter_init (&iter, self->remotes);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_insert (out, g_strdup (key), NULL);

  g_mutex_unlock (&self->remotes_lock);

  if (self->parent_repo)
    _ostree_repo_remote_list (self->parent_repo, out);
}

/**
 * ostree_repo_remote_list:
 * @self: Repo
 * @out_n_remotes: (out) (allow-none): Number of remotes available
 *
 * List available remote names in an #OstreeRepo.  Remote names are sorted
 * alphabetically.  If no remotes are available the function returns %NULL.
 *
 * Returns: (array length=out_n_remotes) (transfer full): a %NULL-terminated
 *          array of remote names
 **/
char **
ostree_repo_remote_list (OstreeRepo *self,
                         guint      *out_n_remotes)
{
  char **remotes = NULL;
  guint n_remotes;
  g_autoptr(GHashTable) remotes_ht = NULL;

  remotes_ht = g_hash_table_new_full (g_str_hash, g_str_equal,
                                      (GDestroyNotify) g_free,
                                      (GDestroyNotify) NULL);

  _ostree_repo_remote_list (self, remotes_ht);

  n_remotes = g_hash_table_size (remotes_ht);

  if (n_remotes > 0)
    {
      GList *list, *link;
      guint ii = 0;

      remotes = g_new (char *, n_remotes + 1);

      list = g_hash_table_get_keys (remotes_ht);
      list = g_list_sort (list, (GCompareFunc) strcmp);

      for (link = list; link != NULL; link = link->next)
        remotes[ii++] = g_strdup (link->data);

      g_list_free (list);

      remotes[ii] = NULL;
    }

  if (out_n_remotes)
    *out_n_remotes = n_remotes;

  return remotes;
}

/**
 * ostree_repo_remote_get_url:
 * @self: Repo
 * @name: Name of remote
 * @out_url: (out) (allow-none): Remote's URL
 * @error: Error
 *
 * Return the URL of the remote named @name through @out_url.  It is an
 * error if the provided remote does not exist.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
ostree_repo_remote_get_url (OstreeRepo  *self,
                            const char  *name,
                            char       **out_url,
                            GError     **error)
{
  g_return_val_if_fail (name != NULL, FALSE);

  g_autofree char *url = NULL;
  if (_ostree_repo_remote_name_is_file (name))
    {
      url = g_strdup (name);
    }
  else
    {
      if (!ostree_repo_get_remote_option (self, name, "url", NULL, &url, error))
        return FALSE;

      if (url == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "No \"url\" option in remote \"%s\"", name);
          return FALSE;
        }
    }

  if (out_url != NULL)
    *out_url = g_steal_pointer (&url);
  return TRUE;
}

/**
 * ostree_repo_remote_get_gpg_verify:
 * @self: Repo
 * @name: Name of remote
 * @out_gpg_verify: (out) (allow-none): Remote's GPG option
 * @error: Error
 *
 * Return whether GPG verification is enabled for the remote named @name
 * through @out_gpg_verify.  It is an error if the provided remote does
 * not exist.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
ostree_repo_remote_get_gpg_verify (OstreeRepo  *self,
                                   const char  *name,
                                   gboolean    *out_gpg_verify,
                                   GError     **error)
{
  g_return_val_if_fail (OSTREE_IS_REPO (self), FALSE);
  g_return_val_if_fail (name != NULL, FALSE);

  /* For compatibility with pull-local, don't GPG verify file:// URIs. */
  if (_ostree_repo_remote_name_is_file (name))
    {
      if (out_gpg_verify != NULL)
        *out_gpg_verify = FALSE;
      return TRUE;
    }

 return ostree_repo_get_remote_boolean_option (self, name, "gpg-verify",
                                               TRUE, out_gpg_verify, error);
}

/**
 * ostree_repo_remote_get_gpg_verify_summary:
 * @self: Repo
 * @name: Name of remote
 * @out_gpg_verify_summary: (out) (allow-none): Remote's GPG option
 * @error: Error
 *
 * Return whether GPG verification of the summary is enabled for the remote
 * named @name through @out_gpg_verify_summary.  It is an error if the provided
 * remote does not exist.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
ostree_repo_remote_get_gpg_verify_summary (OstreeRepo  *self,
                                           const char  *name,
                                           gboolean    *out_gpg_verify_summary,
                                           GError     **error)
{
  return ostree_repo_get_remote_boolean_option (self, name, "gpg-verify-summary",
                                                FALSE, out_gpg_verify_summary, error);
}

/**
 * ostree_repo_remote_gpg_import:
 * @self: Self
 * @name: name of a remote
 * @source_stream: (allow-none): a #GInputStream, or %NULL
 * @key_ids: (array zero-terminated=1) (element-type utf8) (allow-none): a %NULL-terminated array of GPG key IDs, or %NULL
 * @out_imported: (allow-none): return location for the number of imported
 *                              keys, or %NULL
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Imports one or more GPG keys from the open @source_stream, or from the
 * user's personal keyring if @source_stream is %NULL.  The @key_ids array
 * can optionally restrict which keys are imported.  If @key_ids is %NULL,
 * then all keys are imported.
 *
 * The imported keys will be used to conduct GPG verification when pulling
 * from the remote named @name.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
ostree_repo_remote_gpg_import (OstreeRepo         *self,
                               const char         *name,
                               GInputStream       *source_stream,
                               const char * const *key_ids,
                               guint              *out_imported,
                               GCancellable       *cancellable,
                               GError            **error)
{
  OstreeRemote *remote;
  g_auto(gpgme_ctx_t) source_context = NULL;
  g_auto(gpgme_ctx_t) target_context = NULL;
  g_auto(gpgme_data_t) data_buffer = NULL;
  gpgme_import_result_t import_result;
  gpgme_import_status_t import_status;
  g_autofree char *source_tmp_dir = NULL;
  g_autofree char *target_tmp_dir = NULL;
  glnx_autofd int target_temp_fd = -1;
  g_autoptr(GPtrArray) keys = NULL;
  struct stat stbuf;
  gpgme_error_t gpg_error;
  gboolean ret = FALSE;

  g_return_val_if_fail (OSTREE_IS_REPO (self), FALSE);
  g_return_val_if_fail (name != NULL, FALSE);

  /* First make sure the remote name is valid. */

  remote = _ostree_repo_get_remote_inherited (self, name, error);
  if (remote == NULL)
    goto out;

  /* Prepare the source GPGME context.  If reading GPG keys from an input
   * stream, point the OpenPGP engine at a temporary directory and import
   * the keys to a new pubring.gpg file.  If the key data format is ASCII
   * armored, this step will convert them to binary. */

  source_context = ot_gpgme_new_ctx (NULL, error);
  if (!source_context)
    goto out;

  if (source_stream != NULL)
    {
      data_buffer = ot_gpgme_data_input (source_stream);

      if (!ot_gpgme_ctx_tmp_home_dir (source_context, &source_tmp_dir,
                                      NULL, cancellable, error))
        {
          g_prefix_error (error, "Unable to configure context: ");
          goto out;
        }

      gpg_error = gpgme_op_import (source_context, data_buffer);
      if (gpg_error != GPG_ERR_NO_ERROR)
        {
          ot_gpgme_throw (gpg_error, error, "Unable to import keys");
          goto out;
        }

      g_clear_pointer (&data_buffer, (GDestroyNotify) gpgme_data_release);
    }

  /* Retrieve all keys or specific keys from the source GPGME context.
   * Assemble a NULL-terminated array of gpgme_key_t structs to import. */

  /* The keys array will contain a NULL terminator, but it turns out,
   * although not documented, gpgme_key_unref() gracefully handles it. */
  keys = g_ptr_array_new_with_free_func ((GDestroyNotify) gpgme_key_unref);

  if (key_ids != NULL)
    {
      guint ii;

      for (ii = 0; key_ids[ii] != NULL; ii++)
        {
          gpgme_key_t key = NULL;

          gpg_error = gpgme_get_key (source_context, key_ids[ii], &key, 0);
          if (gpg_error != GPG_ERR_NO_ERROR)
            {
              ot_gpgme_throw (gpg_error, error, "Unable to find key \"%s\"", key_ids[ii]);
              goto out;
            }

          /* Transfer ownership. */
          g_ptr_array_add (keys, key);
        }
    }
  else
    {
      gpg_error = gpgme_op_keylist_start (source_context, NULL, 0);

      while (gpg_error == GPG_ERR_NO_ERROR)
        {
          gpgme_key_t key = NULL;

          gpg_error = gpgme_op_keylist_next (source_context, &key);

          if (gpg_error != GPG_ERR_NO_ERROR)
            break;

          /* Transfer ownership. */
          g_ptr_array_add (keys, key);
        }

      if (gpgme_err_code (gpg_error) != GPG_ERR_EOF)
        {
          ot_gpgme_throw (gpg_error, error, "Unable to list keys");
          goto out;
        }
    }

  /* Add the NULL terminator. */
  g_ptr_array_add (keys, NULL);

  /* Prepare the target GPGME context to serve as the import destination.
   * Here the pubring.gpg file in a second temporary directory is a copy
   * of the remote's keyring file.  We'll let the import operation alter
   * the pubring.gpg file, then rename it back to its permanent home. */

  target_context = ot_gpgme_new_ctx (NULL, error);
  if (!target_context)
    goto out;

  /* No need for an output stream since we copy in a pubring.gpg. */
  if (!ot_gpgme_ctx_tmp_home_dir (target_context, &target_tmp_dir,
                                  NULL, cancellable, error))
    {
      g_prefix_error (error, "Unable to configure context: ");
      goto out;
    }

  if (!glnx_opendirat (AT_FDCWD, target_tmp_dir, FALSE, &target_temp_fd, error))
    {
      g_prefix_error (error, "Unable to open directory: ");
      goto out;
    }

  if (fstatat (self->repo_dir_fd, remote->keyring, &stbuf, AT_SYMLINK_NOFOLLOW) == 0)
    {
      if (!glnx_file_copy_at (self->repo_dir_fd, remote->keyring,
                              &stbuf, target_temp_fd, "pubring.gpg",
                              GLNX_FILE_COPY_NOXATTRS, cancellable, error))
        {
          g_prefix_error (error, "Unable to copy remote's keyring: ");
          goto out;
        }
    }
  else if (errno == ENOENT)
    {
      glnx_autofd int fd = -1;

      /* Create an empty pubring.gpg file prior to importing keys.  This
       * prevents gpg2 from creating a pubring.kbx file in the new keybox
       * format [1].  We want to stay with the older keyring format since
       * its performance issues are not relevant here.
       *
       * [1] https://gnupg.org/faq/whats-new-in-2.1.html#keybox
       */
      fd = openat (target_temp_fd, "pubring.gpg",
                   O_WRONLY | O_CREAT | O_CLOEXEC | O_NOCTTY, 0644);
      if (fd == -1)
        {
          glnx_set_prefix_error_from_errno (error, "%s", "Unable to create pubring.gpg");
          goto out;
        }
    }
  else
    {
      glnx_set_prefix_error_from_errno (error, "%s", "Unable to copy remote's keyring");
      goto out;
    }

  /* Export the selected keys from the source context and import them into
   * the target context. */

  gpg_error = gpgme_data_new (&data_buffer);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      ot_gpgme_throw (gpg_error, error, "Unable to create data buffer");
      goto out;
    }

  gpg_error = gpgme_op_export_keys (source_context,
                                    (gpgme_key_t *) keys->pdata, 0,
                                    data_buffer);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      ot_gpgme_throw (gpg_error, error, "Unable to export keys");
      goto out;
    }

  (void) gpgme_data_seek (data_buffer, 0, SEEK_SET);

  gpg_error = gpgme_op_import (target_context, data_buffer);
  if (gpg_error != GPG_ERR_NO_ERROR)
    {
      ot_gpgme_throw (gpg_error, error, "Unable to import keys");
      goto out;
    }

  import_result = gpgme_op_import_result (target_context);
  g_return_val_if_fail (import_result != NULL, FALSE);

  /* Check the status of each import and fail on the first error.
   * All imports must be successful to update the remote's keyring. */
  for (import_status = import_result->imports;
       import_status != NULL;
       import_status = import_status->next)
    {
      if (import_status->result != GPG_ERR_NO_ERROR)
        {
          ot_gpgme_throw (gpg_error, error, "Unable to import key \"%s\"",
                          import_status->fpr);
          goto out;
        }
    }

  /* Import successful; replace the remote's old keyring with the
   * updated keyring in the target context's temporary directory. */
  if (!glnx_file_copy_at (target_temp_fd, "pubring.gpg", NULL,
                          self->repo_dir_fd, remote->keyring,
                          GLNX_FILE_COPY_NOXATTRS | GLNX_FILE_COPY_OVERWRITE,
                          cancellable, error))
    goto out;

  if (out_imported != NULL)
    *out_imported = (guint) import_result->imported;

  ret = TRUE;

out:
  if (remote != NULL)
    ostree_remote_unref (remote);

  if (source_tmp_dir != NULL)
    (void) glnx_shutil_rm_rf_at (AT_FDCWD, source_tmp_dir, NULL, NULL);

  if (target_tmp_dir != NULL)
    (void) glnx_shutil_rm_rf_at (AT_FDCWD, target_tmp_dir, NULL, NULL);

  g_prefix_error (error, "GPG: ");

  return ret;
}

/**
 * ostree_repo_remote_fetch_summary:
 * @self: Self
 * @name: name of a remote
 * @out_summary: (out) (optional): return location for raw summary data, or
 *               %NULL
 * @out_signatures: (out) (optional): return location for raw summary
 *                  signature data, or %NULL
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Tries to fetch the summary file and any GPG signatures on the summary file
 * over HTTP, and returns the binary data in @out_summary and @out_signatures
 * respectively.
 *
 * If no summary file exists on the remote server, @out_summary is set to
 * @NULL.  Likewise if the summary file is not signed, @out_signatures is
 * set to @NULL.  In either case the function still returns %TRUE.
 *
 * This method does not verify the signature of the downloaded summary file.
 * Use ostree_repo_verify_summary() for that.
 *
 * Parse the summary data into a #GVariant using g_variant_new_from_bytes()
 * with #OSTREE_SUMMARY_GVARIANT_FORMAT as the format string.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
ostree_repo_remote_fetch_summary (OstreeRepo    *self,
                                  const char    *name,
                                  GBytes       **out_summary,
                                  GBytes       **out_signatures,
                                  GCancellable  *cancellable,
                                  GError       **error)
{
  return ostree_repo_remote_fetch_summary_with_options (self,
                                                        name,
                                                        NULL,
                                                        out_summary,
                                                        out_signatures,
                                                        cancellable,
                                                        error);
}

static gboolean
ostree_repo_mode_to_string (OstreeRepoMode   mode,
                            const char     **out_mode,
                            GError         **error)
{
  const char *ret_mode;

  switch (mode)
    {
    case OSTREE_REPO_MODE_BARE:
      ret_mode = "bare";
      break;
    case OSTREE_REPO_MODE_BARE_USER:
      ret_mode = "bare-user";
      break;
    case OSTREE_REPO_MODE_BARE_USER_ONLY:
      ret_mode = "bare-user-only";
      break;
    case OSTREE_REPO_MODE_ARCHIVE:
      /* Legacy alias */
      ret_mode ="archive-z2";
      break;
    default:
      return glnx_throw (error, "Invalid mode '%d'", mode);
    }

  *out_mode = ret_mode;
  return TRUE;
}

gboolean
ostree_repo_mode_from_string (const char      *mode,
                              OstreeRepoMode  *out_mode,
                              GError         **error)
{
  OstreeRepoMode ret_mode;

  if (strcmp (mode, "bare") == 0)
    ret_mode = OSTREE_REPO_MODE_BARE;
  else if (strcmp (mode, "bare-user") == 0)
    ret_mode = OSTREE_REPO_MODE_BARE_USER;
  else if (strcmp (mode, "bare-user-only") == 0)
    ret_mode = OSTREE_REPO_MODE_BARE_USER_ONLY;
  else if (strcmp (mode, "archive-z2") == 0 ||
           strcmp (mode, "archive") == 0)
    ret_mode = OSTREE_REPO_MODE_ARCHIVE;
  else
    return glnx_throw (error, "Invalid mode '%s' in repository configuration", mode);

  *out_mode = ret_mode;
  return TRUE;
}

#define DEFAULT_CONFIG_CONTENTS ("[core]\n" \
                                 "repo_version=1\n")

/* Just write the dirs to disk, return a dfd */
static gboolean
repo_create_at_internal (int             dfd,
                         const char     *path,
                         OstreeRepoMode  mode,
                         GVariant       *options,
                         int            *out_dfd,
                         GCancellable   *cancellable,
                         GError        **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Creating repo", error);
   struct stat stbuf;
  /* We do objects/ last - if it exists we do nothing and exit successfully */
  const char *state_dirs[] = { "tmp", "extensions", "state",
                               "refs", "refs/heads", "refs/mirrors",
                               "refs/remotes", "objects" };

  /* Early return if we have an existing repo */
  { g_autofree char *objects_path = g_build_filename (path, "objects", NULL);

    if (!glnx_fstatat_allow_noent (dfd, objects_path, &stbuf, 0, error))
      return FALSE;
    if (errno == 0)
      {
        glnx_autofd int repo_dfd = -1;
        if (!glnx_opendirat (dfd, path, TRUE, &repo_dfd, error))
          return FALSE;

        /* Note early return */
        *out_dfd = glnx_steal_fd (&repo_dfd);
        return TRUE;
      }
  }

  if (mkdirat (dfd, path, 0755) != 0)
    {
      if (G_UNLIKELY (errno != EEXIST))
        return glnx_throw_errno_prefix (error, "mkdirat");
    }

  glnx_autofd int repo_dfd = -1;
  if (!glnx_opendirat (dfd, path, TRUE, &repo_dfd, error))
    return FALSE;

  if (!glnx_fstatat_allow_noent (repo_dfd, "config", &stbuf, 0, error))
    return FALSE;
  if (errno == ENOENT)
    {
      const char *mode_str = NULL;
      g_autoptr(GString) config_data = g_string_new (DEFAULT_CONFIG_CONTENTS);

      if (!ostree_repo_mode_to_string (mode, &mode_str, error))
        return FALSE;
      g_assert (mode_str);

      g_string_append_printf (config_data, "mode=%s\n", mode_str);

      const char *collection_id = NULL;
      if (options)
        g_variant_lookup (options, "collection-id", "&s", &collection_id);
      if (collection_id != NULL)
        g_string_append_printf (config_data, "collection-id=%s\n", collection_id);

      if (!glnx_file_replace_contents_at (repo_dfd, "config",
                                          (guint8*)config_data->str, config_data->len,
                                          0, cancellable, error))
        return FALSE;
    }

  for (guint i = 0; i < G_N_ELEMENTS (state_dirs); i++)
    {
      const char *elt = state_dirs[i];
      if (mkdirat (repo_dfd, elt, 0755) == -1)
        {
          if (G_UNLIKELY (errno != EEXIST))
            return glnx_throw_errno_prefix (error, "mkdirat");
        }
    }

  /* Test that the fs supports user xattrs now, so we get an error early rather
   * than during an object write later.
   */
  if (mode == OSTREE_REPO_MODE_BARE_USER)
    {
      g_auto(GLnxTmpfile) tmpf = { 0, };

      if (!glnx_open_tmpfile_linkable_at (repo_dfd, ".", O_RDWR|O_CLOEXEC, &tmpf, error))
        return FALSE;
      if (!_ostree_write_bareuser_metadata (tmpf.fd, 0, 0, 644, NULL, error))
        return FALSE;
  }

  *out_dfd = glnx_steal_fd (&repo_dfd);
  return TRUE;
}

/**
 * ostree_repo_create:
 * @self: An #OstreeRepo
 * @mode: The mode to store the repository in
 * @cancellable: Cancellable
 * @error: Error
 *
 * Create the underlying structure on disk for the repository, and call
 * ostree_repo_open() on the result, preparing it for use.

 * Since version 2016.8, this function will succeed on an existing
 * repository, and finish creating any necessary files in a partially
 * created repository.  However, this function cannot change the mode
 * of an existing repository, and will silently ignore an attempt to
 * do so.
 *
 * Since 2017.9, "existing repository" is defined by the existence of an
 * `objects` subdirectory.
 *
 * This function predates ostree_repo_create_at(). It is an error to call
 * this function on a repository initialized via ostree_repo_open_at().
 */
gboolean
ostree_repo_create (OstreeRepo     *self,
                    OstreeRepoMode  mode,
                    GCancellable   *cancellable,
                    GError        **error)
{
  g_return_val_if_fail (self->repodir, FALSE);
  const char *repopath = gs_file_get_path_cached (self->repodir);
  g_autoptr(GVariantBuilder) builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
  if (self->collection_id)
    g_variant_builder_add (builder, "{s@v}", "collection-id",
                           g_variant_new_variant (g_variant_new_string (self->collection_id)));

  glnx_autofd int repo_dir_fd = -1;
  g_autoptr(GVariant) options = g_variant_ref_sink (g_variant_builder_end (builder));
  if (!repo_create_at_internal (AT_FDCWD, repopath, mode,
                                options,
                                &repo_dir_fd,
                                cancellable, error))
    return FALSE;
  self->repo_dir_fd = glnx_steal_fd (&repo_dir_fd);
  if (!ostree_repo_open (self, cancellable, error))
    return FALSE;
  return TRUE;
}

/**
 * ostree_repo_create_at:
 * @dfd: Directory fd
 * @path: Path
 * @mode: The mode to store the repository in
 * @options: a{sv}: See below for accepted keys
 * @cancellable: Cancellable
 * @error: Error
 *
 * This is a file-descriptor relative version of ostree_repo_create().
 * Create the underlying structure on disk for the repository, and call
 * ostree_repo_open_at() on the result, preparing it for use.
 *
 * If a repository already exists at @dfd + @path (defined by an `objects/`
 * subdirectory existing), then this function will simply call
 * ostree_repo_open_at().  In other words, this function cannot be used to change
 * the mode or configuration (`repo/config`) of an existing repo.
 *
 * The @options dict may contain:
 *
 *   - collection-id: s: Set as collection ID in repo/config (Since 2017.9)
 *
 * Returns: (transfer full): A new OSTree repository reference
 */
OstreeRepo *
ostree_repo_create_at (int             dfd,
                       const char     *path,
                       OstreeRepoMode  mode,
                       GVariant       *options,
                       GCancellable   *cancellable,
                       GError        **error)
{
  glnx_autofd int repo_dfd = -1;
  if (!repo_create_at_internal (dfd, path, mode, options, &repo_dfd,
                                cancellable, error))
    return NULL;
  return repo_open_at_take_fd (&repo_dfd, cancellable, error);
}

static gboolean
enumerate_directory_allow_noent (GFile               *dirpath,
                                 const char          *queryargs,
                                 GFileQueryInfoFlags  queryflags,
                                 GFileEnumerator    **out_direnum,
                                 GCancellable        *cancellable,
                                 GError             **error)
{
  g_autoptr(GError) temp_error = NULL;
  g_autoptr(GFileEnumerator) ret_direnum = NULL;

  ret_direnum = g_file_enumerate_children (dirpath, queryargs, queryflags,
                                           cancellable, &temp_error);
  if (!ret_direnum)
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_clear_error (&temp_error);
      else
        {
          g_propagate_error (error, g_steal_pointer (&temp_error));
          return FALSE;
        }
    }

  if (out_direnum)
    *out_direnum = g_steal_pointer (&ret_direnum);
  return TRUE;
}

static gboolean
add_remotes_from_keyfile (OstreeRepo *self,
                          GKeyFile   *keyfile,
                          GFile      *file,
                          GError    **error)
{
  GQueue queue = G_QUEUE_INIT;
  g_auto(GStrv) groups = NULL;
  gsize length, ii;
  gboolean ret = FALSE;

  g_mutex_lock (&self->remotes_lock);

  groups = g_key_file_get_groups (keyfile, &length);

  for (ii = 0; ii < length; ii++)
    {
      OstreeRemote *remote;

      remote = ostree_remote_new_from_keyfile (keyfile, groups[ii]);

      if (remote != NULL)
        {
          /* Make sure all the remotes in the key file are
           * acceptable before adding any to the OstreeRepo. */
          g_queue_push_tail (&queue, remote);

          if (g_hash_table_contains (self->remotes, remote->name))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Multiple specifications found for remote \"%s\"",
                           remote->name);
              goto out;
            }

          if (file != NULL)
            remote->file = g_object_ref (file);
        }
    }

  while (!g_queue_is_empty (&queue))
    {
      OstreeRemote *remote = g_queue_pop_head (&queue);
      g_hash_table_replace (self->remotes, remote->name, remote);
    }

  ret = TRUE;

 out:
  while (!g_queue_is_empty (&queue))
    ostree_remote_unref (g_queue_pop_head (&queue));

  g_mutex_unlock (&self->remotes_lock);

  return ret;
}

static gboolean
append_one_remote_config (OstreeRepo      *self,
                          GFile           *path,
                          GCancellable    *cancellable,
                          GError         **error)
{
  g_autoptr(GKeyFile) remotedata = g_key_file_new ();
  if (!g_key_file_load_from_file (remotedata, gs_file_get_path_cached (path),
                                  0, error))
    return FALSE;

  return add_remotes_from_keyfile (self, remotedata, path, error);
}

static GFile *
get_remotes_d_dir (OstreeRepo          *self,
                   GFile               *sysroot)
{
  g_autoptr(GFile) sysroot_owned = NULL;
  /* Very complicated sysroot logic; this bit breaks the otherwise mostly clean
   * layering between OstreeRepo and OstreeSysroot. First, If a sysroot was
   * provided, use it. Otherwise, check to see whether we reference
   * /ostree/repo, or if not that, see if we have a ref to a sysroot (and it's
   * physical).
   */
  g_autoptr(OstreeSysroot) sysroot_ref = NULL;
  if (sysroot == NULL)
    {
      /* No explicit sysroot?  Let's see if we have a kind */
      switch (self->sysroot_kind)
        {
        case OSTREE_REPO_SYSROOT_KIND_UNKNOWN:
          g_assert_not_reached ();
          break;
        case OSTREE_REPO_SYSROOT_KIND_NO:
          break;
        case OSTREE_REPO_SYSROOT_KIND_IS_SYSROOT_OSTREE:
          sysroot = sysroot_owned = g_file_new_for_path ("/");
          break;
        case OSTREE_REPO_SYSROOT_KIND_VIA_SYSROOT:
          sysroot_ref = (OstreeSysroot*)g_weak_ref_get (&self->sysroot);
          /* Only write to /etc/ostree/remotes.d if we are pointed at a deployment */
          if (sysroot_ref != NULL && !sysroot_ref->is_physical)
            sysroot = ostree_sysroot_get_path (sysroot_ref);
          break;
        }
    }
  /* For backwards compat, also fall back to the sysroot-path variable, which we
   * don't set anymore internally, and I hope no one else uses.
   */
  if (sysroot == NULL && sysroot_ref == NULL)
    sysroot = self->sysroot_dir;

  /* Was the config directory specified? If so, use that with the
   * optional sysroot prepended. If not, return the path in /etc if the
   * sysroot was found and NULL otherwise to use the repo config.
   */
  if (self->remotes_config_dir != NULL)
    {
      if (sysroot == NULL)
        return g_file_new_for_path (self->remotes_config_dir);
      else
        return g_file_resolve_relative_path (sysroot, self->remotes_config_dir);
    }
  else if (sysroot == NULL)
    return NULL;
  else
    return g_file_resolve_relative_path (sysroot, SYSCONF_REMOTES);
}

static gboolean
reload_core_config (OstreeRepo          *self,
                    GCancellable        *cancellable,
                    GError             **error)
{
  g_autofree char *version = NULL;
  g_autofree char *mode = NULL;
  g_autofree char *contents = NULL;
  g_autofree char *parent_repo_path = NULL;
  gboolean is_archive;
  gsize len;

  g_clear_pointer (&self->config, (GDestroyNotify)g_key_file_unref);
  self->config = g_key_file_new ();

  contents = glnx_file_get_contents_utf8_at (self->repo_dir_fd, "config", &len,
                                             NULL, error);
  if (!contents)
    return FALSE;
  if (!g_key_file_load_from_data (self->config, contents, len, 0, error))
    {
      g_prefix_error (error, "Couldn't parse config file: ");
      return FALSE;
    }

  version = g_key_file_get_value (self->config, "core", "repo_version", error);
  if (!version)
    return FALSE;

  if (strcmp (version, "1") != 0)
    return glnx_throw (error, "Invalid repository version '%s'", version);

  if (!ot_keyfile_get_boolean_with_default (self->config, "core", "archive",
                                            FALSE, &is_archive, error))
    return FALSE;
  if (is_archive)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "This version of OSTree no longer supports \"archive\" repositories; use archive-z2 instead");
      return FALSE;
    }

  if (!ot_keyfile_get_value_with_default (self->config, "core", "mode",
                                          "bare", &mode, error))
    return FALSE;
  if (!ostree_repo_mode_from_string (mode, &self->mode, error))
    return FALSE;

  if (self->writable)
    {
      if (!ot_keyfile_get_boolean_with_default (self->config, "core", "enable-uncompressed-cache",
                                                TRUE, &self->enable_uncompressed_cache, error))
        return FALSE;
    }
  else
    self->enable_uncompressed_cache = FALSE;

  {
    gboolean do_fsync;

    if (!ot_keyfile_get_boolean_with_default (self->config, "core", "fsync",
                                              TRUE, &do_fsync, error))
      return FALSE;

    if (!do_fsync)
      ostree_repo_set_disable_fsync (self, TRUE);
  }

  /* See https://github.com/ostreedev/ostree/issues/758 */
  if (!ot_keyfile_get_boolean_with_default (self->config, "core", "disable-xattrs",
                                            FALSE, &self->disable_xattrs, error))
    return FALSE;

  { g_autofree char *tmp_expiry_seconds = NULL;

    /* 86400 secs = one day */
    if (!ot_keyfile_get_value_with_default (self->config, "core", "tmp-expiry-secs", "86400",
                                            &tmp_expiry_seconds, error))
      return FALSE;

    self->tmp_expiry_seconds = g_ascii_strtoull (tmp_expiry_seconds, NULL, 10);
  }

  /* Disable locking by default for now */
  { gboolean locking;
    if (!ot_keyfile_get_boolean_with_default (self->config, "core", "locking",
                                              FALSE, &locking, error))
      return FALSE;
    if (!locking)
      {
        self->lock_timeout_seconds = REPO_LOCK_DISABLED;
      }
    else
      {
        g_autofree char *lock_timeout_seconds = NULL;

        if (!ot_keyfile_get_value_with_default (self->config, "core", "lock-timeout-secs", "30",
                                                &lock_timeout_seconds, error))
          return FALSE;

        self->lock_timeout_seconds = g_ascii_strtoull (lock_timeout_seconds, NULL, 10);
      }
  }

  { g_autofree char *compression_level_str = NULL;

    /* gzip defaults to 6 */
    (void)ot_keyfile_get_value_with_default (self->config, "archive", "zlib-level", NULL,
                                             &compression_level_str, NULL);

    if (compression_level_str)
      /* Ensure level is in [1,9] */
      self->zlib_compression_level = MAX (1, MIN (9, g_ascii_strtoull (compression_level_str, NULL, 10)));
    else
      self->zlib_compression_level = OSTREE_ARCHIVE_DEFAULT_COMPRESSION_LEVEL;
  }

  { g_autofree char *min_free_space_percent_str = NULL;
    /* If changing this, be sure to change the man page too */
    const char *default_min_free_space = "3";

    if (!ot_keyfile_get_value_with_default (self->config, "core", "min-free-space-percent",
                                            default_min_free_space,
                                            &min_free_space_percent_str, error))
      return FALSE;

    self->min_free_space_percent = g_ascii_strtoull (min_free_space_percent_str, NULL, 10);
    if (self->min_free_space_percent > 99)
      return glnx_throw (error, "Invalid min-free-space-percent '%s'", min_free_space_percent_str);
  }

  {
    g_clear_pointer (&self->collection_id, g_free);
    if (!ot_keyfile_get_value_with_default (self->config, "core", "collection-id",
                                            NULL, &self->collection_id, NULL))
      return FALSE;
  }

  if (!ot_keyfile_get_value_with_default (self->config, "core", "parent",
                                          NULL, &parent_repo_path, error))
    return FALSE;

  if (parent_repo_path && parent_repo_path[0])
    {
      g_autoptr(GFile) parent_repo_f = g_file_new_for_path (parent_repo_path);

      g_clear_object (&self->parent_repo);
      self->parent_repo = ostree_repo_new (parent_repo_f);

      if (!ostree_repo_open (self->parent_repo, cancellable, error))
        {
          g_prefix_error (error, "While checking parent repository '%s': ",
                          gs_file_get_path_cached (parent_repo_f));
          return FALSE;
        }
    }

  /* By default, only add remotes in a remotes config directory for
   * system repos. This is to preserve legacy behavior for non-system
   * repos that specify a remotes config dir (flatpak).
   */
  { gboolean is_system = ostree_repo_is_system (self);

    if (!ot_keyfile_get_boolean_with_default (self->config, "core", "add-remotes-config-dir",
                                              is_system, &self->add_remotes_config_dir, error))
      return FALSE;
  }

  return TRUE;
}

static gboolean
reload_remote_config (OstreeRepo          *self,
                      GCancellable        *cancellable,
                      GError             **error)
{

  g_mutex_lock (&self->remotes_lock);
  g_hash_table_remove_all (self->remotes);
  g_mutex_unlock (&self->remotes_lock);

  if (!add_remotes_from_keyfile (self, self->config, NULL, error))
    return FALSE;

  g_autoptr(GFile) remotes_d = get_remotes_d_dir (self, NULL);
  if (remotes_d == NULL)
    return TRUE;

  g_autoptr(GFileEnumerator) direnum = NULL;
  if (!enumerate_directory_allow_noent (remotes_d, OSTREE_GIO_FAST_QUERYINFO, 0,
                                        &direnum,
                                        cancellable, error))
    return FALSE;
  if (direnum)
    {
      while (TRUE)
        {
          GFileInfo *file_info;
          GFile *path;
          const char *name;
          guint32 type;

          if (!g_file_enumerator_iterate (direnum, &file_info, &path,
                                          NULL, error))
            return FALSE;
          if (file_info == NULL)
            break;

          name = g_file_info_get_attribute_byte_string (file_info, "standard::name");
          type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

          if (type == G_FILE_TYPE_REGULAR &&
              g_str_has_suffix (name, ".conf"))
            {
              if (!append_one_remote_config (self, path, cancellable, error))
                return FALSE;
            }
        }
    }

  return TRUE;
}

/**
 * ostree_repo_reload_config:
 * @self: repo
 * @cancellable: cancellable
 * @error: error
 *
 * By default, an #OstreeRepo will cache the remote configuration and its
 * own repo/config data.  This API can be used to reload it.
 */
gboolean
ostree_repo_reload_config (OstreeRepo          *self,
                           GCancellable        *cancellable,
                           GError             **error)
{
  if (!reload_core_config (self, cancellable, error))
    return FALSE;
  if (!reload_remote_config (self, cancellable, error))
    return FALSE;
  return TRUE;
}

gboolean
ostree_repo_open (OstreeRepo    *self,
                  GCancellable  *cancellable,
                  GError       **error)
{
  struct stat stbuf;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->inited)
    return TRUE;

  /* We use a directory of the form `staging-${BOOT_ID}-${RANDOM}`
   * where if the ${BOOT_ID} doesn't match, we know file contents
   * possibly haven't been sync'd to disk and need to be discarded.
   */
  { const char *env_bootid = getenv ("OSTREE_BOOTID");
    g_autofree char *boot_id = NULL;

    if (env_bootid != NULL)
      boot_id = g_strdup (env_bootid);
    else
      {
        if (!g_file_get_contents ("/proc/sys/kernel/random/boot_id",
                                  &boot_id,
                                  NULL,
                                  error))
          return FALSE;
        g_strdelimit (boot_id, "\n", '\0');
      }

    self->stagedir_prefix = g_strconcat (OSTREE_REPO_TMPDIR_STAGING, boot_id, "-", NULL);
  }

  if (self->repo_dir_fd == -1)
    {
      g_assert (self->repodir);
      if (!glnx_opendirat (AT_FDCWD, gs_file_get_path_cached (self->repodir), TRUE,
                           &self->repo_dir_fd, error))
        {
          g_prefix_error (error, "%s: ", gs_file_get_path_cached (self->repodir));
          return FALSE;
        }
    }

  if (!glnx_fstat (self->repo_dir_fd, &stbuf, error))
    return FALSE;
  self->device = stbuf.st_dev;
  self->inode = stbuf.st_ino;

  if (!glnx_opendirat (self->repo_dir_fd, "objects", TRUE,
                       &self->objects_dir_fd, error))
    return FALSE;

  self->writable = faccessat (self->objects_dir_fd, ".", W_OK, 0) == 0;
  if (!self->writable)
    {
      /* This is returned through ostree_repo_is_writable(). */
      glnx_set_error_from_errno (&self->writable_error);
      /* Note - we don't return this error yet! */
    }

  if (!glnx_fstat (self->objects_dir_fd, &stbuf, error))
    return FALSE;
  self->owner_uid = stbuf.st_uid;

  if (stbuf.st_uid != getuid () || stbuf.st_gid != getgid ())
    {
      self->target_owner_uid = stbuf.st_uid;
      self->target_owner_gid = stbuf.st_gid;
    }
  else
    {
      self->target_owner_uid = self->target_owner_gid = -1;
    }

  if (self->writable)
    {
      /* Always try to recreate the tmpdir to be nice to people
       * who are looking to free up space.
       *
       * https://github.com/ostreedev/ostree/issues/1018
       */
      if (mkdirat (self->repo_dir_fd, "tmp", 0755) == -1)
        {
          if (G_UNLIKELY (errno != EEXIST))
            return glnx_throw_errno_prefix (error, "mkdir(tmp)");
        }
    }

  if (!glnx_opendirat (self->repo_dir_fd, "tmp", TRUE, &self->tmp_dir_fd, error))
    return FALSE;

  if (self->writable)
    {
      if (!glnx_shutil_mkdir_p_at (self->tmp_dir_fd, _OSTREE_CACHE_DIR, 0775, cancellable, error))
        return FALSE;

      if (!glnx_opendirat (self->tmp_dir_fd, _OSTREE_CACHE_DIR, TRUE, &self->cache_dir_fd, error))
        return FALSE;
    }

  /* If we weren't created via ostree_sysroot_get_repo(), for backwards
   * compatibility we need to figure out now whether or not we refer to the
   * system repo.  See also ostree-sysroot.c.
   */
  if (self->sysroot_kind == OSTREE_REPO_SYSROOT_KIND_UNKNOWN)
    {
      struct stat system_stbuf;
      /* Ignore any errors if we can't access /ostree/repo */
      if (fstatat (AT_FDCWD, "/ostree/repo", &system_stbuf, 0) == 0)
        {
          /* Are we the same as /ostree/repo? */
          if (self->device == system_stbuf.st_dev &&
              self->inode == system_stbuf.st_ino)
            self->sysroot_kind = OSTREE_REPO_SYSROOT_KIND_IS_SYSROOT_OSTREE;
          else
            self->sysroot_kind = OSTREE_REPO_SYSROOT_KIND_NO;
        }
      else
        self->sysroot_kind = OSTREE_REPO_SYSROOT_KIND_NO;
    }

  if (!ostree_repo_reload_config (self, cancellable, error))
    return FALSE;

  self->inited = TRUE;
  return TRUE;
}

/**
 * ostree_repo_set_disable_fsync:
 * @self: An #OstreeRepo
 * @disable_fsync: If %TRUE, do not fsync
 *
 * Disable requests to fsync() to stable storage during commits.  This
 * option should only be used by build system tools which are creating
 * disposable virtual machines, or have higher level mechanisms for
 * ensuring data consistency.
 */
void
ostree_repo_set_disable_fsync (OstreeRepo    *self,
                               gboolean       disable_fsync)
{
  self->disable_fsync = disable_fsync;
}

/**
 * ostree_repo_set_cache_dir:
 * @self: An #OstreeRepo
 * @dfd: directory fd
 * @path: subpath in @dfd
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Set a custom location for the cache directory used for e.g.
 * per-remote summary caches. Setting this manually is useful when
 * doing operations on a system repo as a user because you don't have
 * write permissions in the repo, where the cache is normally stored.
 */
gboolean
ostree_repo_set_cache_dir (OstreeRepo    *self,
                           int            dfd,
                           const char    *path,
                           GCancellable  *cancellable,
                           GError        **error)
{
  glnx_autofd int fd = -1;
  if (!glnx_opendirat (dfd, path, TRUE, &fd, error))
    return FALSE;

  glnx_close_fd (&self->cache_dir_fd);
  self->cache_dir_fd = glnx_steal_fd (&fd);

  return TRUE;
}

/**
 * ostree_repo_get_disable_fsync:
 * @self: An #OstreeRepo
 *
 * For more information see ostree_repo_set_disable_fsync().
 *
 * Returns: Whether or not fsync() is enabled for this repo.
 */
gboolean
ostree_repo_get_disable_fsync (OstreeRepo    *self)
{
  return self->disable_fsync;
}

/* Replace the contents of a file, honoring the repository's fsync
 * policy.
 */
gboolean
_ostree_repo_file_replace_contents (OstreeRepo    *self,
                                    int            dfd,
                                    const char    *path,
                                    const guint8   *buf,
                                    gsize          len,
                                    GCancellable  *cancellable,
                                    GError       **error)
{
  return glnx_file_replace_contents_at (dfd, path, buf, len,
                                        self->disable_fsync ? GLNX_FILE_REPLACE_NODATASYNC : GLNX_FILE_REPLACE_DATASYNC_NEW,
                                        cancellable, error);
}

/**
 * ostree_repo_get_path:
 * @self: Repo
 *
 * Note that since the introduction of ostree_repo_open_at(), this function may
 * return a process-specific path in `/proc` if the repository was created using
 * that API. In general, you should avoid use of this API.
 *
 * Returns: (transfer none): Path to repo
 */
GFile *
ostree_repo_get_path (OstreeRepo  *self)
{
  /* Did we have an abspath?  Return it */
  if (self->repodir)
    return self->repodir;
  /* Lazily create a fd-relative path */
  if (!self->repodir_fdrel)
    self->repodir_fdrel = ot_fdrel_to_gfile (self->repo_dir_fd, ".");
  return self->repodir_fdrel;
}

/**
 * ostree_repo_get_dfd:
 * @self: Repo
 *
 * In some cases it's useful for applications to access the repository
 * directly; for example, writing content into `repo/tmp` ensures it's
 * on the same filesystem.  Another case is detecting the mtime on the
 * repository (to see whether a ref was written).
 *
 * Returns: File descriptor for repository root - owned by @self
 */
int
ostree_repo_get_dfd (OstreeRepo  *self)
{
  g_return_val_if_fail (self->repo_dir_fd != -1, -1);
  return self->repo_dir_fd;
}

/**
 * ostree_repo_hash:
 * @self: an #OstreeRepo
 *
 * Calculate a hash value for the given open repository, suitable for use when
 * putting it into a hash table. It is an error to call this on an #OstreeRepo
 * which is not yet open, as a persistent hash value cannot be calculated until
 * the repository is open and the inode of its root directory has been loaded.
 *
 * This function does no I/O.
 *
 * Returns: hash value for the #OstreeRepo
 * Since: 2017.12
 */
guint
ostree_repo_hash (OstreeRepo *self)
{
  g_return_val_if_fail (OSTREE_IS_REPO (self), 0);

  /* We cannot hash non-open repositories, since their hash value would change
   * once they’re opened, resulting in false lookup misses and the inability to
   * remove them from a hash table. */
  g_assert (self->repo_dir_fd >= 0);

  /* device and inode numbers are distributed fairly uniformly, so we can’t
   * do much better than just combining them. No need to rehash to even out
   * the distribution. */
  return (self->device ^ self->inode);
}

/**
 * ostree_repo_equal:
 * @a: an #OstreeRepo
 * @b: an #OstreeRepo
 *
 * Check whether two opened repositories are the same on disk: if their root
 * directories are the same inode. If @a or @b are not open yet (due to
 * ostree_repo_open() not being called on them yet), %FALSE will be returned.
 *
 * Returns: %TRUE if @a and @b are the same repository on disk, %FALSE otherwise
 * Since: 2017.12
 */
gboolean
ostree_repo_equal (OstreeRepo *a,
                   OstreeRepo *b)
{
  g_return_val_if_fail (OSTREE_IS_REPO (a), FALSE);
  g_return_val_if_fail (OSTREE_IS_REPO (b), FALSE);

  if (a->repo_dir_fd < 0 || b->repo_dir_fd < 0)
    return FALSE;

  return (a->device == b->device && a->inode == b->inode);
}

OstreeRepoMode
ostree_repo_get_mode (OstreeRepo  *self)
{
  g_return_val_if_fail (self->inited, FALSE);

  return self->mode;
}

/**
 * ostree_repo_get_parent:
 * @self: Repo
 *
 * Before this function can be used, ostree_repo_init() must have been
 * called.
 *
 * Returns: (transfer none): Parent repository, or %NULL if none
 */
OstreeRepo *
ostree_repo_get_parent (OstreeRepo  *self)
{
  return self->parent_repo;
}

static gboolean
list_loose_objects_at (OstreeRepo             *self,
                       GHashTable             *inout_objects,
                       int                     dfd,
                       const char             *prefix,
                       const char             *commit_starting_with,
                       GCancellable           *cancellable,
                       GError                **error)
{
  GVariant *key, *value;

  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  gboolean exists;
  if (!ot_dfd_iter_init_allow_noent (dfd, prefix, &dfd_iter, &exists, error))
    return FALSE;
  /* Note early return */
  if (!exists)
    return TRUE;

  while (TRUE)
    {
      struct dirent *dent;

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      const char *name = dent->d_name;
      if (strcmp (name, ".") == 0 ||
          strcmp (name, "..") == 0)
        continue;

      const char *dot = strrchr (name, '.');
      if (!dot)
        continue;

      OstreeObjectType objtype;
      if ((self->mode == OSTREE_REPO_MODE_ARCHIVE
           && strcmp (dot, ".filez") == 0) ||
          ((_ostree_repo_mode_is_bare (self->mode))
           && strcmp (dot, ".file") == 0))
        objtype = OSTREE_OBJECT_TYPE_FILE;
      else if (strcmp (dot, ".dirtree") == 0)
        objtype = OSTREE_OBJECT_TYPE_DIR_TREE;
      else if (strcmp (dot, ".dirmeta") == 0)
        objtype = OSTREE_OBJECT_TYPE_DIR_META;
      else if (strcmp (dot, ".commit") == 0)
        objtype = OSTREE_OBJECT_TYPE_COMMIT;
      else
        continue;

      if ((dot - name) != 62)
        continue;

      char buf[OSTREE_SHA256_STRING_LEN+1];

      memcpy (buf, prefix, 2);
      memcpy (buf + 2, name, 62);
      buf[sizeof(buf)-1] = '\0';

      /* if we passed in a "starting with" argument, then
         we only want to return .commit objects with a checksum
         that matches the commit_starting_with argument */
      if (commit_starting_with)
        {
          /* object is not a commit, do not add to array */
          if (objtype != OSTREE_OBJECT_TYPE_COMMIT)
              continue;

          /* commit checksum does not match "starting with", do not add to array */     
          if (!g_str_has_prefix (buf, commit_starting_with))
            continue;
        }

      key = ostree_object_name_serialize (buf, objtype);
      value = g_variant_new ("(b@as)",
                             TRUE, g_variant_new_strv (NULL, 0));
      /* transfer ownership */
      g_hash_table_replace (inout_objects, g_variant_ref_sink (key),
                            g_variant_ref_sink (value));
    }

  return TRUE;
}

static gboolean
list_loose_objects (OstreeRepo                     *self,
                    GHashTable                     *inout_objects,
                    const char                     *commit_starting_with,
                    GCancellable                   *cancellable,
                    GError                        **error)
{
  static const gchar hexchars[] = "0123456789abcdef";

  for (guint c = 0; c < 256; c++)
    {
      char buf[3];
      buf[0] = hexchars[c >> 4];
      buf[1] = hexchars[c & 0xF];
      buf[2] = '\0';
      if (!list_loose_objects_at (self, inout_objects, self->objects_dir_fd, buf,
                                  commit_starting_with,
                                  cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
load_metadata_internal (OstreeRepo       *self,
                        OstreeObjectType  objtype,
                        const char       *sha256,
                        gboolean          error_if_not_found,
                        GVariant        **out_variant,
                        GInputStream    **out_stream,
                        guint64          *out_size,
                        OstreeRepoCommitState *out_state,
                        GCancellable     *cancellable,
                        GError          **error)
{
  char loose_path_buf[_OSTREE_LOOSE_PATH_MAX];
  glnx_autofd int fd = -1;
  g_autoptr(GInputStream) ret_stream = NULL;
  g_autoptr(GVariant) ret_variant = NULL;

  g_return_val_if_fail (OSTREE_OBJECT_TYPE_IS_META (objtype), FALSE);
  g_return_val_if_fail (objtype == OSTREE_OBJECT_TYPE_COMMIT || out_state == NULL, FALSE);

  /* Special caching for dirmeta objects, since they're commonly referenced many
   * times.
   */
  const gboolean is_dirmeta_cachable =
    (objtype == OSTREE_OBJECT_TYPE_DIR_META && out_variant && !out_stream);
  if (is_dirmeta_cachable)
    {
      GMutex *lock = &self->cache_lock;
      g_mutex_lock (lock);
      GVariant *cache_hit = NULL;
      /* Look it up, if we have a cache */
      if (self->dirmeta_cache)
        cache_hit = g_hash_table_lookup (self->dirmeta_cache, sha256);
      if (cache_hit)
        *out_variant = g_variant_ref (cache_hit);
      g_mutex_unlock (lock);
      if (cache_hit)
        return TRUE;
    }

  _ostree_loose_path (loose_path_buf, sha256, objtype, self->mode);

 if (!ot_openat_ignore_enoent (self->objects_dir_fd, loose_path_buf, &fd,
                               error))
    return FALSE;

  if (fd < 0 && self->commit_stagedir.initialized)
    {
      if (!ot_openat_ignore_enoent (self->commit_stagedir.fd, loose_path_buf, &fd,
                                    error))
        return FALSE;
    }

  if (fd != -1)
    {
      struct stat stbuf;
      if (!glnx_fstat (fd, &stbuf, error))
        return FALSE;
      if (out_variant)
        {
          if (!ot_variant_read_fd (fd, 0, ostree_metadata_variant_type (objtype), TRUE,
                                   &ret_variant, error))
            return FALSE;

          /* Now, let's put it in the cache */
          if (is_dirmeta_cachable)
            {
              GMutex *lock = &self->cache_lock;
              g_mutex_lock (lock);
              if (self->dirmeta_cache)
                g_hash_table_replace (self->dirmeta_cache, g_strdup (sha256), g_variant_ref (ret_variant));
              g_mutex_unlock (lock);
            }
        }
      else if (out_stream)
        {
          ret_stream = g_unix_input_stream_new (fd, TRUE);
          if (!ret_stream)
            return FALSE;
          fd = -1; /* Transfer ownership */
        }

      if (out_size)
        *out_size = stbuf.st_size;

      if (out_state)
        {
          g_autofree char *commitpartial_path = _ostree_get_commitpartial_path (sha256);
          *out_state = 0;

          if (!glnx_fstatat_allow_noent (self->repo_dir_fd, commitpartial_path, NULL, 0, error))
            return FALSE;
          if (errno == 0)
            *out_state |= OSTREE_REPO_COMMIT_STATE_PARTIAL;
        }
    }
  else if (self->parent_repo)
    {
      /* Directly recurse to simplify out parameters */
      return load_metadata_internal (self->parent_repo, objtype, sha256, error_if_not_found,
                                     out_variant, out_stream, out_size, out_state,
                                     cancellable, error);
    }
  else if (error_if_not_found)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No such metadata object %s.%s",
                   sha256, ostree_object_type_to_string (objtype));
      return FALSE;
    }

  ot_transfer_out_value (out_variant, &ret_variant);
  ot_transfer_out_value (out_stream, &ret_stream);
  return TRUE;
}

static GVariant  *
filemeta_to_stat (struct stat *stbuf,
                  GVariant   *metadata)
{
  guint32 uid, gid, mode;
  GVariant *xattrs;

  g_variant_get (metadata, "(uuu@a(ayay))",
                 &uid, &gid, &mode, &xattrs);
  stbuf->st_uid = GUINT32_FROM_BE (uid);
  stbuf->st_gid = GUINT32_FROM_BE (gid);
  stbuf->st_mode = GUINT32_FROM_BE (mode);

  return xattrs;
}

static gboolean
repo_load_file_archive (OstreeRepo *self,
                        const char         *checksum,
                        GInputStream      **out_input,
                        GFileInfo         **out_file_info,
                        GVariant          **out_xattrs,
                        GCancellable       *cancellable,
                        GError            **error)
{
  struct stat stbuf;
  char loose_path_buf[_OSTREE_LOOSE_PATH_MAX];
  _ostree_loose_path (loose_path_buf, checksum, OSTREE_OBJECT_TYPE_FILE, self->mode);

  glnx_autofd int fd = -1;
  if (!ot_openat_ignore_enoent (self->objects_dir_fd, loose_path_buf, &fd,
                                error))
    return FALSE;

  if (fd < 0 && self->commit_stagedir.initialized)
    {
      if (!ot_openat_ignore_enoent (self->commit_stagedir.fd, loose_path_buf, &fd,
                                    error))
        return FALSE;
    }

  if (fd != -1)
    {
      if (!glnx_fstat (fd, &stbuf, error))
        return FALSE;

      g_autoptr(GInputStream) tmp_stream = g_unix_input_stream_new (glnx_steal_fd (&fd), TRUE);
      /* Note return here */
      return ostree_content_stream_parse (TRUE, tmp_stream, stbuf.st_size, TRUE,
                                          out_input, out_file_info, out_xattrs,
                                          cancellable, error);
    }
  else if (self->parent_repo)
    {
      return ostree_repo_load_file (self->parent_repo, checksum,
                                    out_input, out_file_info, out_xattrs,
                                    cancellable, error);
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Couldn't find file object '%s'", checksum);
      return FALSE;
    }
}

gboolean
_ostree_repo_load_file_bare (OstreeRepo         *self,
                             const char         *checksum,
                             int                *out_fd,
                             struct stat        *out_stbuf,
                             char              **out_symlink,
                             GVariant          **out_xattrs,
                             GCancellable       *cancellable,
                             GError            **error)
{
  /* The bottom case recursing on the parent repo */
  if (self == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Couldn't find file object '%s'", checksum);
      return FALSE;
    }

  struct stat stbuf;
  glnx_autofd int fd = -1;
  g_autofree char *ret_symlink = NULL;
  g_autoptr(GVariant) ret_xattrs = NULL;
  char loose_path_buf[_OSTREE_LOOSE_PATH_MAX];
  _ostree_loose_path (loose_path_buf, checksum, OSTREE_OBJECT_TYPE_FILE, self->mode);

  /* Do a fstatat() and find the object directory that contains this object */
  int objdir_fd = self->objects_dir_fd;
  int res;
  if ((res = TEMP_FAILURE_RETRY (fstatat (objdir_fd, loose_path_buf, &stbuf, AT_SYMLINK_NOFOLLOW))) < 0
      && errno == ENOENT && self->commit_stagedir.initialized)
    {
      objdir_fd = self->commit_stagedir.fd;
      res = TEMP_FAILURE_RETRY (fstatat (objdir_fd, loose_path_buf, &stbuf, AT_SYMLINK_NOFOLLOW));
    }
  if (res < 0 && errno != ENOENT)
    return glnx_throw_errno_prefix (error, "fstat");
  else if (res < 0)
    {
      g_assert (errno == ENOENT);
      return _ostree_repo_load_file_bare (self->parent_repo, checksum, out_fd,
                                          out_stbuf, out_symlink, out_xattrs,
                                          cancellable, error);
    }

  const gboolean need_open =
    (out_fd || out_xattrs || self->mode == OSTREE_REPO_MODE_BARE_USER);
  /* If it's a regular file and we're requested to return the fd, do it now. As
   * a special case in bare-user, we always do an open, since the stat() metadata
   * lives there.
   */
  if (need_open && S_ISREG (stbuf.st_mode))
    {
      fd = openat (objdir_fd, loose_path_buf, O_CLOEXEC | O_RDONLY);
      if (fd < 0)
        return glnx_throw_errno_prefix (error, "openat");
    }

  if (!(S_ISREG (stbuf.st_mode) || S_ISLNK (stbuf.st_mode)))
    return glnx_throw (error, "Not a regular file or symlink: %s", loose_path_buf);

  /* In the non-bare-user case, gather symlink info if requested */
  if (self->mode != OSTREE_REPO_MODE_BARE_USER
      && S_ISLNK (stbuf.st_mode) && out_symlink)
    {
      ret_symlink = glnx_readlinkat_malloc (objdir_fd, loose_path_buf,
                                            cancellable, error);
      if (!ret_symlink)
        return FALSE;
    }

  if (self->mode == OSTREE_REPO_MODE_BARE_USER)
    {
      g_autoptr(GBytes) bytes = glnx_fgetxattr_bytes (fd, "user.ostreemeta", error);
      if (bytes == NULL)
        return FALSE;

      g_autoptr(GVariant) metadata = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_FILEMETA_GVARIANT_FORMAT,
                                                                                   bytes, FALSE));
      ret_xattrs = filemeta_to_stat (&stbuf, metadata);
      if (S_ISLNK (stbuf.st_mode))
        {
          if (out_symlink)
            {
              char targetbuf[PATH_MAX+1];
              gsize target_size;
              g_autoptr(GInputStream) target_input = g_unix_input_stream_new (fd, FALSE);
              if (!g_input_stream_read_all (target_input, targetbuf, sizeof (targetbuf),
                                            &target_size, cancellable, error))
                return FALSE;

              ret_symlink = g_strndup (targetbuf, target_size);
            }
          /* In the symlink case, we don't want to return the bare-user fd */
          glnx_close_fd (&fd);
        }
    }
  else if (self->mode == OSTREE_REPO_MODE_BARE_USER_ONLY)
    {
      /* Canonical info is: uid/gid is 0 and no xattrs, which
         might be wrong and thus not validate correctly, but
         at least we report something consistent. */
      stbuf.st_uid = stbuf.st_gid = 0;

      if (out_xattrs)
        {
          GVariantBuilder builder;
          g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ayay)"));
          ret_xattrs = g_variant_ref_sink (g_variant_builder_end (&builder));
        }
    }
  else
    {
      g_assert (self->mode == OSTREE_REPO_MODE_BARE);

      if (S_ISREG (stbuf.st_mode) && out_xattrs)
        {
          if (self->disable_xattrs)
            ret_xattrs = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("(ayay)"), NULL, 0));
          else if (!glnx_fd_get_all_xattrs (fd, &ret_xattrs,
                                            cancellable, error))
            return FALSE;
        }
      else if (S_ISLNK (stbuf.st_mode) && out_xattrs)
        {
          if (self->disable_xattrs)
            ret_xattrs = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("(ayay)"), NULL, 0));
          else if (!glnx_dfd_name_get_all_xattrs (objdir_fd, loose_path_buf,
                                                  &ret_xattrs,
                                                  cancellable, error))
            return FALSE;
        }
    }

  if (out_fd)
    *out_fd = glnx_steal_fd (&fd);
  if (out_stbuf)
    *out_stbuf = stbuf;
  ot_transfer_out_value (out_symlink, &ret_symlink);
  ot_transfer_out_value (out_xattrs, &ret_xattrs);
  return TRUE;
}

/**
 * ostree_repo_load_file:
 * @self: Repo
 * @checksum: ASCII SHA256 checksum
 * @out_input: (out) (optional) (nullable): File content
 * @out_file_info: (out) (optional) (nullable): File information
 * @out_xattrs: (out) (optional) (nullable): Extended attributes
 * @cancellable: Cancellable
 * @error: Error
 *
 * Load content object, decomposing it into three parts: the actual
 * content (for regular files), the metadata, and extended attributes.
 */
gboolean
ostree_repo_load_file (OstreeRepo         *self,
                       const char         *checksum,
                       GInputStream      **out_input,
                       GFileInfo         **out_file_info,
                       GVariant          **out_xattrs,
                       GCancellable       *cancellable,
                       GError            **error)
{
  if (self->mode == OSTREE_REPO_MODE_ARCHIVE)
    return repo_load_file_archive (self, checksum, out_input, out_file_info, out_xattrs,
                                   cancellable, error);
  else
    {
      glnx_autofd int fd = -1;
      struct stat stbuf;
      g_autofree char *symlink_target = NULL;
      g_autoptr(GVariant) ret_xattrs = NULL;
      if (!_ostree_repo_load_file_bare (self, checksum,
                                        out_input ? &fd : NULL,
                                        out_file_info ? &stbuf : NULL,
                                        out_file_info ? &symlink_target : NULL,
                                        out_xattrs ? &ret_xattrs : NULL,
                                        cancellable, error))
        return FALSE;

      /* Convert fd → GInputStream and struct stat → GFileInfo */
      if (out_input)
        {
          if (fd != -1)
            *out_input = g_unix_input_stream_new (glnx_steal_fd (&fd), TRUE);
          else
            *out_input = NULL;
        }
      if (out_file_info)
        {
          *out_file_info = _ostree_stbuf_to_gfileinfo (&stbuf);
          if (S_ISLNK (stbuf.st_mode))
            g_file_info_set_symlink_target (*out_file_info, symlink_target);
          else
            g_assert (S_ISREG (stbuf.st_mode));
        }

      ot_transfer_out_value (out_xattrs, &ret_xattrs);
      return TRUE;
    }
}

/**
 * ostree_repo_load_object_stream:
 * @self: Repo
 * @objtype: Object type
 * @checksum: ASCII SHA256 checksum
 * @out_input: (out): Stream for object
 * @out_size: (out): Length of @out_input
 * @cancellable: Cancellable
 * @error: Error
 *
 * Load object as a stream; useful when copying objects between
 * repositories.
 */
gboolean
ostree_repo_load_object_stream (OstreeRepo         *self,
                                OstreeObjectType    objtype,
                                const char         *checksum,
                                GInputStream      **out_input,
                                guint64            *out_size,
                                GCancellable       *cancellable,
                                GError            **error)
{
  guint64 size;
  g_autoptr(GInputStream) ret_input = NULL;

  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    {
      if (!load_metadata_internal (self, objtype, checksum, TRUE, NULL,
                                   &ret_input, &size, NULL,
                                   cancellable, error))
        return FALSE;
    }
  else
    {
      g_autoptr(GInputStream) input = NULL;
      g_autoptr(GFileInfo) finfo = NULL;
      g_autoptr(GVariant) xattrs = NULL;

      if (!ostree_repo_load_file (self, checksum, &input, &finfo, &xattrs,
                                  cancellable, error))
        return FALSE;

      if (!ostree_raw_file_to_content_stream (input, finfo, xattrs,
                                              &ret_input, &size,
                                              cancellable, error))
        return FALSE;
    }

  ot_transfer_out_value (out_input, &ret_input);
  *out_size = size;
  return TRUE;
}

/*
 * _ostree_repo_has_loose_object:
 * @loose_path_buf: Buffer of size _OSTREE_LOOSE_PATH_MAX
 *
 * Locate object in repository; if it exists, @out_is_stored will be
 * set to TRUE.  @loose_path_buf is always set to the loose path.
 */
gboolean
_ostree_repo_has_loose_object (OstreeRepo           *self,
                               const char           *checksum,
                               OstreeObjectType      objtype,
                               gboolean             *out_is_stored,
                               GCancellable         *cancellable,
                               GError             **error)
{
  char loose_path_buf[_OSTREE_LOOSE_PATH_MAX];
  _ostree_loose_path (loose_path_buf, checksum, objtype, self->mode);

  gboolean found = FALSE;
  /* It's easier to share code if we make this an array */
  int dfd_searches[] = { -1, self->objects_dir_fd };
  if (self->commit_stagedir.initialized)
    dfd_searches[0] = self->commit_stagedir.fd;
  for (guint i = 0; i < G_N_ELEMENTS (dfd_searches); i++)
    {
      int dfd = dfd_searches[i];
      if (dfd == -1)
        continue;
      struct stat stbuf;
      if (TEMP_FAILURE_RETRY (fstatat (dfd, loose_path_buf, &stbuf, AT_SYMLINK_NOFOLLOW)) < 0)
        {
          if (errno == ENOENT)
            ; /* Next dfd */
          else
            return glnx_throw_errno_prefix (error, "fstatat(%s)", loose_path_buf);
        }
      else
        {
          found = TRUE;
          break;
        }
    }

  *out_is_stored = found;
  return TRUE;
}

/**
 * ostree_repo_has_object:
 * @self: Repo
 * @objtype: Object type
 * @checksum: ASCII SHA256 checksum
 * @out_have_object: (out): %TRUE if repository contains object
 * @cancellable: Cancellable
 * @error: Error
 *
 * Set @out_have_object to %TRUE if @self contains the given object;
 * %FALSE otherwise.
 *
 * Returns: %FALSE if an unexpected error occurred, %TRUE otherwise
 */
gboolean
ostree_repo_has_object (OstreeRepo           *self,
                        OstreeObjectType      objtype,
                        const char           *checksum,
                        gboolean             *out_have_object,
                        GCancellable         *cancellable,
                        GError              **error)
{
  gboolean ret_have_object;

  if (!_ostree_repo_has_loose_object (self, checksum, objtype, &ret_have_object,
                                      cancellable, error))
    return FALSE;

  /* In the future, here is where we would also look up in metadata pack files */

  if (!ret_have_object && self->parent_repo)
    {
      if (!ostree_repo_has_object (self->parent_repo, objtype, checksum,
                                   &ret_have_object, cancellable, error))
        return FALSE;
    }

  if (out_have_object)
    *out_have_object = ret_have_object;
  return TRUE;
}

/**
 * ostree_repo_delete_object:
 * @self: Repo
 * @objtype: Object type
 * @sha256: Checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Remove the object of type @objtype with checksum @sha256
 * from the repository.  An error of type %G_IO_ERROR_NOT_FOUND
 * is thrown if the object does not exist.
 */
gboolean
ostree_repo_delete_object (OstreeRepo           *self,
                           OstreeObjectType      objtype,
                           const char           *sha256,
                           GCancellable         *cancellable,
                           GError              **error)
{
  char loose_path[_OSTREE_LOOSE_PATH_MAX];
  _ostree_loose_path (loose_path, sha256, objtype, self->mode);

  if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
    {
      char meta_loose[_OSTREE_LOOSE_PATH_MAX];

      _ostree_loose_path (meta_loose, sha256, OSTREE_OBJECT_TYPE_COMMIT_META, self->mode);

      if (!ot_ensure_unlinked_at (self->objects_dir_fd, meta_loose, error))
        return FALSE;
    }

  if (!glnx_unlinkat (self->objects_dir_fd, loose_path, 0, error))
    return glnx_prefix_error (error, "Deleting object %s.%s", sha256, ostree_object_type_to_string (objtype));

  /* If the repository is configured to use tombstone commits, create one when deleting a commit.  */
  if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
    {
      gboolean tombstone_commits = FALSE;
      GKeyFile *readonly_config = ostree_repo_get_config (self);
      if (!ot_keyfile_get_boolean_with_default (readonly_config, "core", "tombstone-commits", FALSE,
                                                &tombstone_commits, error))
        return FALSE;

      if (tombstone_commits)
        {
          g_auto(GVariantBuilder) builder = OT_VARIANT_BUILDER_INITIALIZER;
          g_autoptr(GVariant) variant = NULL;

          g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
          g_variant_builder_add (&builder, "{sv}", "commit", g_variant_new_bytestring (sha256));
          variant = g_variant_ref_sink (g_variant_builder_end (&builder));
          if (!ostree_repo_write_metadata_trusted (self,
                                                   OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT,
                                                   sha256,
                                                   variant,
                                                   cancellable,
                                                   error))
            return FALSE;
        }
    }

  return TRUE;
}

/* Thin wrapper for _ostree_verify_metadata_object() */
static gboolean
fsck_metadata_object (OstreeRepo           *self,
                      OstreeObjectType      objtype,
                      const char           *sha256,
                      GCancellable         *cancellable,
                      GError              **error)
{
  const char *errmsg = glnx_strjoina ("fsck ", sha256, ".", ostree_object_type_to_string (objtype));
  GLNX_AUTO_PREFIX_ERROR (errmsg, error);
  g_autoptr(GVariant) metadata = NULL;
  if (!load_metadata_internal (self, objtype, sha256, TRUE,
                               &metadata, NULL, NULL, NULL,
                               cancellable, error))
    return FALSE;

  return _ostree_verify_metadata_object (objtype, sha256, metadata, error);
}

static gboolean
fsck_content_object (OstreeRepo           *self,
                     const char           *sha256,
                     GCancellable         *cancellable,
                     GError              **error)
{
  const char *errmsg = glnx_strjoina ("fsck content object ", sha256);
  GLNX_AUTO_PREFIX_ERROR (errmsg, error);
  g_autoptr(GInputStream) input = NULL;
  g_autoptr(GFileInfo) file_info = NULL;
  g_autoptr(GVariant) xattrs = NULL;

  if (!ostree_repo_load_file (self, sha256, &input, &file_info, &xattrs,
                              cancellable, error))
    return FALSE;

  /* TODO more consistency checks here */
  const guint32 mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
  if (!ostree_validate_structureof_file_mode (mode, error))
    return FALSE;

  g_autofree guchar *computed_csum = NULL;
  if (!ostree_checksum_file_from_input (file_info, xattrs, input,
                                        OSTREE_OBJECT_TYPE_FILE, &computed_csum,
                                        cancellable, error))
    return FALSE;

  char actual_checksum[OSTREE_SHA256_STRING_LEN+1];
  ostree_checksum_inplace_from_bytes (computed_csum, actual_checksum);
  return _ostree_compare_object_checksum (OSTREE_OBJECT_TYPE_FILE, sha256, actual_checksum, error);
}

/**
 * ostree_repo_fsck_object:
 * @self: Repo
 * @objtype: Object type
 * @sha256: Checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Verify consistency of the object; this performs checks only relevant to the
 * immediate object itself, such as checksumming. This API call will not itself
 * traverse metadata objects for example.
 *
 * Since: 2017.15
 */
gboolean
ostree_repo_fsck_object (OstreeRepo           *self,
                         OstreeObjectType      objtype,
                         const char           *sha256,
                         GCancellable         *cancellable,
                         GError              **error)
{
  if (OSTREE_OBJECT_TYPE_IS_META (objtype))
    return fsck_metadata_object (self, objtype, sha256, cancellable, error);
  else
    return fsck_content_object (self, sha256, cancellable, error);
}

/**
 * ostree_repo_import_object_from:
 * @self: Destination repo
 * @source: Source repo
 * @objtype: Object type
 * @checksum: checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Copy object named by @objtype and @checksum into @self from the
 * source repository @source.  If both repositories are of the same
 * type and on the same filesystem, this will simply be a fast Unix
 * hard link operation.
 *
 * Otherwise, a copy will be performed.
 */
gboolean
ostree_repo_import_object_from (OstreeRepo           *self,
                                OstreeRepo           *source,
                                OstreeObjectType      objtype,
                                const char           *checksum,
                                GCancellable         *cancellable,
                                GError              **error)
{
  return
    ostree_repo_import_object_from_with_trust (self, source, objtype,
                                               checksum, TRUE, cancellable, error);
}

/**
 * ostree_repo_import_object_from_with_trust:
 * @self: Destination repo
 * @source: Source repo
 * @objtype: Object type
 * @checksum: checksum
 * @trusted: If %TRUE, assume the source repo is valid and trusted
 * @cancellable: Cancellable
 * @error: Error
 *
 * Copy object named by @objtype and @checksum into @self from the
 * source repository @source.  If both repositories are of the same
 * type and on the same filesystem, this will simply be a fast Unix
 * hard link operation.
 *
 * Otherwise, a copy will be performed.
 */
gboolean
ostree_repo_import_object_from_with_trust (OstreeRepo           *self,
                                           OstreeRepo           *source,
                                           OstreeObjectType      objtype,
                                           const char           *checksum,
                                           gboolean              trusted,
                                           GCancellable         *cancellable,
                                           GError              **error)
{
  /* This just wraps a currently internal API, may make it public later */
  OstreeRepoImportFlags flags = trusted ? _OSTREE_REPO_IMPORT_FLAGS_TRUSTED : 0;
  return _ostree_repo_import_object (self, source, objtype, checksum,
                                     flags, cancellable, error);
}

/**
 * ostree_repo_query_object_storage_size:
 * @self: Repo
 * @objtype: Object type
 * @sha256: Checksum
 * @out_size: (out): Size in bytes object occupies physically
 * @cancellable: Cancellable
 * @error: Error
 *
 * Return the size in bytes of object with checksum @sha256, after any
 * compression has been applied.
 */
gboolean
ostree_repo_query_object_storage_size (OstreeRepo           *self,
                                       OstreeObjectType      objtype,
                                       const char           *sha256,
                                       guint64              *out_size,
                                       GCancellable         *cancellable,
                                       GError              **error)
{
  char loose_path[_OSTREE_LOOSE_PATH_MAX];
  _ostree_loose_path (loose_path, sha256, objtype, self->mode);
  int res;

  struct stat stbuf;
  res = TEMP_FAILURE_RETRY (fstatat (self->objects_dir_fd, loose_path, &stbuf, AT_SYMLINK_NOFOLLOW));
  if (res < 0 && errno == ENOENT && self->commit_stagedir.initialized)
    res = TEMP_FAILURE_RETRY (fstatat (self->commit_stagedir.fd, loose_path, &stbuf, AT_SYMLINK_NOFOLLOW));

  if (res < 0)
    return glnx_throw_errno_prefix (error, "Querying object %s.%s", sha256, ostree_object_type_to_string (objtype));

  *out_size = stbuf.st_size;
  return TRUE;
}

/**
 * ostree_repo_load_variant_if_exists:
 * @self: Repo
 * @objtype: Object type
 * @sha256: ASCII checksum
 * @out_variant: (out) (transfer full): Metadata
 * @error: Error
 *
 * Attempt to load the metadata object @sha256 of type @objtype if it
 * exists, storing the result in @out_variant.  If it doesn't exist,
 * %NULL is returned.
 */
gboolean
ostree_repo_load_variant_if_exists (OstreeRepo       *self,
                                    OstreeObjectType  objtype,
                                    const char       *sha256,
                                    GVariant        **out_variant,
                                    GError          **error)
{
  return load_metadata_internal (self, objtype, sha256, FALSE,
                                 out_variant, NULL, NULL, NULL, NULL, error);
}

/**
 * ostree_repo_load_variant:
 * @self: Repo
 * @objtype: Expected object type
 * @sha256: Checksum string
 * @out_variant: (out) (transfer full): Metadata object
 * @error: Error
 *
 * Load the metadata object @sha256 of type @objtype, storing the
 * result in @out_variant.
 */
gboolean
ostree_repo_load_variant (OstreeRepo       *self,
                          OstreeObjectType  objtype,
                          const char       *sha256,
                          GVariant        **out_variant,
                          GError          **error)
{
  return load_metadata_internal (self, objtype, sha256, TRUE,
                                 out_variant, NULL, NULL, NULL, NULL, error);
}

/**
 * ostree_repo_load_commit:
 * @self: Repo
 * @checksum: Commit checksum
 * @out_commit: (out) (allow-none): Commit
 * @out_state: (out) (allow-none): Commit state
 * @error: Error
 *
 * A version of ostree_repo_load_variant() specialized to commits,
 * capable of returning extended state information.  Currently
 * the only extended state is %OSTREE_REPO_COMMIT_STATE_PARTIAL, which
 * means that only a sub-path of the commit is available.
 */
gboolean
ostree_repo_load_commit (OstreeRepo            *self,
                         const char            *checksum, 
                         GVariant             **out_variant,
                         OstreeRepoCommitState *out_state,
                         GError               **error)
{
  return load_metadata_internal (self, OSTREE_OBJECT_TYPE_COMMIT, checksum, TRUE,
                                 out_variant, NULL, NULL, out_state, NULL, error);
}

/**
 * ostree_repo_list_objects:
 * @self: Repo
 * @flags: Flags controlling enumeration
 * @out_objects: (out) (transfer container) (element-type GVariant GVariant):
 * Map of serialized object name to variant data
 * @cancellable: Cancellable
 * @error: Error
 *
 * This function synchronously enumerates all objects in the
 * repository, returning data in @out_objects.  @out_objects
 * maps from keys returned by ostree_object_name_serialize()
 * to #GVariant values of type %OSTREE_REPO_LIST_OBJECTS_VARIANT_TYPE.
 *
 * Returns: %TRUE on success, %FALSE on error, and @error will be set
 */
gboolean
ostree_repo_list_objects (OstreeRepo                  *self,
                          OstreeRepoListObjectsFlags   flags,
                          GHashTable                 **out_objects,
                          GCancellable                *cancellable,
                          GError                     **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (self->inited, FALSE);

  g_autoptr(GHashTable) ret_objects =
    g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                           (GDestroyNotify) g_variant_unref,
                           (GDestroyNotify) g_variant_unref);

  if (flags & OSTREE_REPO_LIST_OBJECTS_ALL)
    flags |= (OSTREE_REPO_LIST_OBJECTS_LOOSE | OSTREE_REPO_LIST_OBJECTS_PACKED);

  if (flags & OSTREE_REPO_LIST_OBJECTS_LOOSE)
    {
      if (!list_loose_objects (self, ret_objects, NULL, cancellable, error))
        return FALSE;
      if ((flags & OSTREE_REPO_LIST_OBJECTS_NO_PARENTS) == 0 && self->parent_repo)
        {
          if (!list_loose_objects (self->parent_repo, ret_objects, NULL, cancellable, error))
            return FALSE;
        }
    }

  if (flags & OSTREE_REPO_LIST_OBJECTS_PACKED)
    {
      /* Nothing for now... */
    }

  ot_transfer_out_value (out_objects, &ret_objects);
  return TRUE;
}

/**
 * ostree_repo_list_commit_objects_starting_with:
 * @self: Repo
 * @start: List commits starting with this checksum
 * @out_commits: (out) (transfer container) (element-type GVariant GVariant):
 * Map of serialized commit name to variant data
 * @cancellable: Cancellable
 * @error: Error
 *
 * This function synchronously enumerates all commit objects starting
 * with @start, returning data in @out_commits.
 *
 * Returns: %TRUE on success, %FALSE on error, and @error will be set
 */
gboolean
ostree_repo_list_commit_objects_starting_with (OstreeRepo                  *self,
                                               const char                  *start,
                                               GHashTable                 **out_commits,
                                               GCancellable                *cancellable,
                                               GError                     **error)
{
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (self->inited, FALSE);

  g_autoptr(GHashTable) ret_commits =
    g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                           (GDestroyNotify) g_variant_unref,
                           (GDestroyNotify) g_variant_unref);

  if (!list_loose_objects (self, ret_commits, start, cancellable, error))
    return FALSE;

  if (self->parent_repo)
    {
      if (!list_loose_objects (self->parent_repo, ret_commits, start,
                               cancellable, error))
        return FALSE;
    }

  ot_transfer_out_value (out_commits, &ret_commits);
  return TRUE;
}

/**
 * ostree_repo_read_commit:
 * @self: Repo
 * @ref: Ref or ASCII checksum
 * @out_root: (out): An #OstreeRepoFile corresponding to the root
 * @out_commit: (out): The resolved commit checksum
 * @cancellable: Cancellable
 * @error: Error
 *
 * Load the content for @rev into @out_root.
 */
gboolean
ostree_repo_read_commit (OstreeRepo   *self,
                         const char   *ref,
                         GFile       **out_root,
                         char        **out_commit,
                         GCancellable *cancellable,
                         GError **error)
{
  g_autofree char *resolved_commit = NULL;
  if (!ostree_repo_resolve_rev (self, ref, FALSE, &resolved_commit, error))
    return FALSE;

  g_autoptr(GFile) ret_root = (GFile*) _ostree_repo_file_new_for_commit (self, resolved_commit, error);
  if (!ret_root)
    return FALSE;

  if (!ostree_repo_file_ensure_resolved ((OstreeRepoFile*)ret_root, error))
    return FALSE;

  ot_transfer_out_value(out_root, &ret_root);
  ot_transfer_out_value(out_commit, &resolved_commit);
  return TRUE;
}

/**
 * ostree_repo_pull:
 * @self: Repo
 * @remote_name: Name of remote
 * @refs_to_fetch: (array zero-terminated=1) (element-type utf8) (allow-none): Optional list of refs; if %NULL, fetch all configured refs
 * @flags: Options controlling fetch behavior
 * @progress: (allow-none): Progress
 * @cancellable: Cancellable
 * @error: Error
 *
 * Connect to the remote repository, fetching the specified set of
 * refs @refs_to_fetch.  For each ref that is changed, download the
 * commit, all metadata, and all content objects, storing them safely
 * on disk in @self.
 *
 * If @flags contains %OSTREE_REPO_PULL_FLAGS_MIRROR, and
 * the @refs_to_fetch is %NULL, and the remote repository contains a
 * summary file, then all refs will be fetched.
 *
 * If @flags contains %OSTREE_REPO_PULL_FLAGS_COMMIT_ONLY, then only the
 * metadata for the commits in @refs_to_fetch is pulled.
 *
 * Warning: This API will iterate the thread default main context,
 * which is a bug, but kept for compatibility reasons.  If you want to
 * avoid this, use g_main_context_push_thread_default() to push a new
 * one around this call.
 */
gboolean
ostree_repo_pull (OstreeRepo               *self,
                  const char               *remote_name,
                  char                    **refs_to_fetch,
                  OstreeRepoPullFlags       flags,
                  OstreeAsyncProgress      *progress,
                  GCancellable             *cancellable,
                  GError                  **error)
{
  return ostree_repo_pull_one_dir (self, remote_name, NULL, refs_to_fetch, flags, progress, cancellable, error);
}

/**
 * ostree_repo_pull_one_dir:
 * @self: Repo
 * @remote_name: Name of remote
 * @dir_to_pull: Subdirectory path
 * @refs_to_fetch: (array zero-terminated=1) (element-type utf8) (allow-none): Optional list of refs; if %NULL, fetch all configured refs
 * @flags: Options controlling fetch behavior
 * @progress: (allow-none): Progress
 * @cancellable: Cancellable
 * @error: Error
 *
 * This is similar to ostree_repo_pull(), but only fetches a single
 * subpath.
 */
gboolean
ostree_repo_pull_one_dir (OstreeRepo               *self,
                          const char               *remote_name,
                          const char               *dir_to_pull,
                          char                    **refs_to_fetch,
                          OstreeRepoPullFlags       flags,
                          OstreeAsyncProgress      *progress,
                          GCancellable             *cancellable,
                          GError                  **error)
{
  GVariantBuilder builder;
  g_autoptr(GVariant) options = NULL;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

  if (dir_to_pull)
    g_variant_builder_add (&builder, "{s@v}", "subdir",
                           g_variant_new_variant (g_variant_new_string (dir_to_pull)));
  g_variant_builder_add (&builder, "{s@v}", "flags",
                         g_variant_new_variant (g_variant_new_int32 (flags)));
  if (refs_to_fetch)
    g_variant_builder_add (&builder, "{s@v}", "refs",
                           g_variant_new_variant (g_variant_new_strv ((const char *const*) refs_to_fetch, -1)));

  options = g_variant_ref_sink (g_variant_builder_end (&builder));
  return ostree_repo_pull_with_options (self, remote_name, options,
                                        progress, cancellable, error);
}

/**
 * _formatted_time_remaining_from_seconds
 * @seconds_remaining: Estimated number of seconds remaining.
 *
 * Returns a strings showing the number of days, hours, minutes
 * and seconds remaining.
 **/
static char *
_formatted_time_remaining_from_seconds (guint64 seconds_remaining)
{
  guint64 minutes_remaining = seconds_remaining / 60;
  guint64 hours_remaining = minutes_remaining / 60;
  guint64 days_remaining = hours_remaining / 24;

  GString *description = g_string_new (NULL);

  if (days_remaining)
    g_string_append_printf (description, "%" G_GUINT64_FORMAT " days ", days_remaining);

  if (hours_remaining)
    g_string_append_printf (description, "%" G_GUINT64_FORMAT " hours ", hours_remaining % 24);

  if (minutes_remaining)
    g_string_append_printf (description, "%" G_GUINT64_FORMAT " minutes ", minutes_remaining % 60);

  g_string_append_printf (description, "%" G_GUINT64_FORMAT " seconds ", seconds_remaining % 60);

  return g_string_free (description, FALSE);
}

/**
 * ostree_repo_pull_default_console_progress_changed:
 * @progress: Async progress
 * @user_data: (allow-none): User data
 *
 * Convenient "changed" callback for use with
 * ostree_async_progress_new_and_connect() when pulling from a remote
 * repository.
 *
 * Depending on the state of the #OstreeAsyncProgress, either displays a
 * custom status message, or else outstanding fetch progress in bytes/sec,
 * or else outstanding content or metadata writes to the repository in
 * number of objects.
 *
 * Compatibility note: this function previously assumed that @user_data
 * was a pointer to a #GSConsole instance.  This is no longer the case,
 * and @user_data is ignored.
 **/
void
ostree_repo_pull_default_console_progress_changed (OstreeAsyncProgress *progress,
                                                   gpointer             user_data)
{
  g_autofree char *status = NULL;
  gboolean caught_error, scanning;
  guint outstanding_fetches;
  guint outstanding_metadata_fetches;
  guint outstanding_writes;
  guint n_scanned_metadata;
  guint fetched_delta_parts;
  guint total_delta_parts;
  guint fetched_delta_part_fallbacks;
  guint total_delta_part_fallbacks;

  g_autoptr(GString) buf = g_string_new ("");

  ostree_async_progress_get (progress,
                             "outstanding-fetches", "u", &outstanding_fetches,
                             "outstanding-metadata-fetches", "u", &outstanding_metadata_fetches,
                             "outstanding-writes", "u", &outstanding_writes,
                             "caught-error", "b", &caught_error,
                             "scanning", "u", &scanning,
                             "scanned-metadata", "u", &n_scanned_metadata,
                             "fetched-delta-parts", "u", &fetched_delta_parts,
                             "total-delta-parts", "u", &total_delta_parts,
                             "fetched-delta-fallbacks", "u", &fetched_delta_part_fallbacks,
                             "total-delta-fallbacks", "u", &total_delta_part_fallbacks,
                             "status", "s", &status,
                             NULL);

  if (*status != '\0')
    {
      g_string_append (buf, status);
    }
  else if (caught_error)
    {
      g_string_append_printf (buf, "Caught error, waiting for outstanding tasks");
    }
  else if (outstanding_fetches)
    {
      guint64 bytes_transferred, start_time, total_delta_part_size;
      guint fetched, metadata_fetched, requested;
      guint64 current_time = g_get_monotonic_time ();
      g_autofree char *formatted_bytes_transferred = NULL;
      g_autofree char *formatted_bytes_sec = NULL;
      guint64 bytes_sec;

      /* Note: This is not atomic wrt the above getter call. */
      ostree_async_progress_get (progress,
                                 "bytes-transferred", "t", &bytes_transferred,
                                 "fetched", "u", &fetched,
                                 "metadata-fetched", "u", &metadata_fetched,
                                 "requested", "u", &requested,
                                 "start-time", "t", &start_time,
                                 "total-delta-part-size", "t", &total_delta_part_size,
                                 NULL);

      formatted_bytes_transferred = g_format_size_full (bytes_transferred, 0);

      /* Ignore the first second, or when we haven't transferred any
       * data, since those could cause divide by zero below.
       */
      if ((current_time - start_time) < G_USEC_PER_SEC || bytes_transferred == 0)
        {
          bytes_sec = 0;
          formatted_bytes_sec = g_strdup ("-");
        }
      else
        {
          bytes_sec = bytes_transferred / ((current_time - start_time) / G_USEC_PER_SEC);
          formatted_bytes_sec = g_format_size (bytes_sec);
        }

      /* Are we doing deltas?  If so, we can be more accurate */
      if (total_delta_parts > 0)
        {
          guint64 fetched_delta_part_size = ostree_async_progress_get_uint64 (progress, "fetched-delta-part-size");
          g_autofree char *formatted_fetched = NULL;
          g_autofree char *formatted_total = NULL;

          /* Here we merge together deltaparts + fallbacks to avoid bloating the text UI */
          fetched_delta_parts += fetched_delta_part_fallbacks;
          total_delta_parts += total_delta_part_fallbacks;

          formatted_fetched = g_format_size (fetched_delta_part_size);
          formatted_total = g_format_size (total_delta_part_size);

          if (bytes_sec > 0)
            {
              /* MAX(0, value) here just to be defensive */
              guint64 est_time_remaining = MAX(0, (total_delta_part_size - fetched_delta_part_size)) / bytes_sec;
              g_autofree char *formatted_est_time_remaining = _formatted_time_remaining_from_seconds (est_time_remaining);
              /* No space between %s and remaining, since formatted_est_time_remaining has a trailing space */
              g_string_append_printf (buf, "Receiving delta parts: %u/%u %s/%s %s/s %sremaining",
                                      fetched_delta_parts, total_delta_parts,
                                      formatted_fetched, formatted_total,
                                      formatted_bytes_sec,
                                      formatted_est_time_remaining);
            }
          else
            {
              g_string_append_printf (buf, "Receiving delta parts: %u/%u %s/%s",
                                      fetched_delta_parts, total_delta_parts,
                                      formatted_fetched, formatted_total);
            }
        }
      else if (scanning || outstanding_metadata_fetches)
        {
          g_string_append_printf (buf, "Receiving metadata objects: %u/(estimating) %s/s %s",
                                  metadata_fetched, formatted_bytes_sec, formatted_bytes_transferred);
        }
      else
        {
          g_string_append_printf (buf, "Receiving objects: %u%% (%u/%u) %s/s %s",
                                  (guint)((((double)fetched) / requested) * 100),
                                  fetched, requested, formatted_bytes_sec, formatted_bytes_transferred);
        }
    }
  else if (outstanding_writes)
    {
      g_string_append_printf (buf, "Writing objects: %u", outstanding_writes);
    }
  else
    {
      g_string_append_printf (buf, "Scanning metadata: %u", n_scanned_metadata);
    }

  glnx_console_text (buf->str);
}

/**
 * ostree_repo_append_gpg_signature:
 * @self: Self
 * @commit_checksum: SHA256 of given commit to sign
 * @signature_bytes: Signature data
 * @cancellable: A #GCancellable
 * @error: a #GError
 *
 * Append a GPG signature to a commit.
 */
gboolean
ostree_repo_append_gpg_signature (OstreeRepo     *self,
                                  const gchar    *commit_checksum,
                                  GBytes         *signature_bytes,
                                  GCancellable   *cancellable,
                                  GError        **error)
{
  g_autoptr(GVariant) metadata = NULL;
  if (!ostree_repo_read_commit_detached_metadata (self,
                                                  commit_checksum,
                                                  &metadata,
                                                  cancellable,
                                                  error))
    return FALSE;

  g_autoptr(GVariant) new_metadata =
    _ostree_detached_metadata_append_gpg_sig (metadata, signature_bytes);

  if (!ostree_repo_write_commit_detached_metadata (self,
                                                   commit_checksum,
                                                   new_metadata,
                                                   cancellable,
                                                   error))
    return FALSE;

  return TRUE;
}

static gboolean
sign_data (OstreeRepo     *self,
           GBytes         *input_data,
           const gchar    *key_id,
           const gchar    *homedir,
           GBytes        **out_signature,
           GCancellable   *cancellable,
           GError        **error)
{
  g_auto(GLnxTmpfile) tmpf = { 0, };
  if (!glnx_open_tmpfile_linkable_at (self->tmp_dir_fd, ".", O_RDWR | O_CLOEXEC,
                                      &tmpf, error))
    return FALSE;
  g_autoptr(GOutputStream) tmp_signature_output = g_unix_output_stream_new (tmpf.fd, FALSE);

  g_auto(gpgme_ctx_t) context = ot_gpgme_new_ctx (homedir, error);
  if (!context)
    return FALSE;

  /* Get the secret keys with the given key id */
  g_auto(gpgme_key_t) key = NULL;
  gpgme_error_t err = gpgme_get_key (context, key_id, &key, 1);
  if (gpgme_err_code (err) == GPG_ERR_EOF)
    return glnx_throw (error, "No gpg key found with ID %s (homedir: %s)", key_id,
                       homedir ? homedir : "<default>");
  else if (err != GPG_ERR_NO_ERROR)
    return ot_gpgme_throw (err, error, "Unable to lookup key ID %s", key_id);

  /* Add the key to the context as a signer */
  if ((err = gpgme_signers_add (context, key)) != GPG_ERR_NO_ERROR)
    return ot_gpgme_throw (err, error, "Error signing commit");

  /* Get a gpg buffer from the commit */
  g_auto(gpgme_data_t) commit_buffer = NULL;
  gsize len;
  const char *buf = g_bytes_get_data (input_data, &len);
  if ((err = gpgme_data_new_from_mem (&commit_buffer, buf, len, FALSE)) != GPG_ERR_NO_ERROR)
    return ot_gpgme_throw (err, error, "Failed to create buffer from commit file");

  /* Sign it */
  g_auto(gpgme_data_t) signature_buffer = ot_gpgme_data_output (tmp_signature_output);
  if ((err = gpgme_op_sign (context, commit_buffer, signature_buffer, GPGME_SIG_MODE_DETACH))
      != GPG_ERR_NO_ERROR)
    return ot_gpgme_throw (err, error, "Failure signing commit file");
  if (!g_output_stream_close (tmp_signature_output, cancellable, error))
    return FALSE;

  /* Return a mmap() reference */
  g_autoptr(GMappedFile) signature_file = g_mapped_file_new_from_fd (tmpf.fd, FALSE, error);
  if (!signature_file)
    return FALSE;

  if (out_signature)
    *out_signature = g_mapped_file_get_bytes (signature_file);
  return TRUE;
}

/**
 * ostree_repo_sign_commit:
 * @self: Self
 * @commit_checksum: SHA256 of given commit to sign
 * @key_id: Use this GPG key id
 * @homedir: (allow-none): GPG home directory, or %NULL
 * @cancellable: A #GCancellable
 * @error: a #GError
 *
 * Add a GPG signature to a commit.
 */
gboolean
ostree_repo_sign_commit (OstreeRepo     *self,
                         const gchar    *commit_checksum,
                         const gchar    *key_id,
                         const gchar    *homedir,
                         GCancellable   *cancellable,
                         GError        **error)
{
  g_autoptr(GBytes) commit_data = NULL;
  g_autoptr(GBytes) signature = NULL;

  g_autoptr(GVariant) commit_variant = NULL;
  if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_COMMIT,
                                 commit_checksum, &commit_variant, error))
    return glnx_prefix_error (error, "Failed to read commit");

  g_autoptr(GVariant) old_metadata = NULL;
  if (!ostree_repo_read_commit_detached_metadata (self,
                                                  commit_checksum,
                                                  &old_metadata,
                                                  cancellable,
                                                  error))
    return glnx_prefix_error (error, "Failed to read detached metadata");

  commit_data = g_variant_get_data_as_bytes (commit_variant);

  /* The verify operation is merely to parse any existing signatures to
   * check if the commit has already been signed with the given key ID.
   * We want to avoid storing duplicate signatures in the metadata. We
   * pass the homedir so that the signing key can be imported, allowing
   * subkey signatures to be recognised. */
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GFile) verify_keydir = NULL;
  if (homedir != NULL)
    verify_keydir = g_file_new_for_path (homedir);
  g_autoptr(OstreeGpgVerifyResult) result
    =_ostree_repo_gpg_verify_with_metadata (self, commit_data, old_metadata,
                                            NULL, verify_keydir, NULL,
                                            cancellable, &local_error);
  if (!result)
    {
      /* "Not found" just means the commit is not yet signed.  That's okay. */
      if (g_error_matches (local_error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_NO_SIGNATURE))
        {
          g_clear_error (&local_error);
        }
      else
        return g_propagate_error (error, g_steal_pointer (&local_error)), FALSE;
    }
  else if (ostree_gpg_verify_result_lookup (result, key_id, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                   "Commit is already signed with GPG key %s", key_id);
      return FALSE;
    }

  if (!sign_data (self, commit_data, key_id, homedir,
                  &signature, cancellable, error))
    return FALSE;

  g_autoptr(GVariant) new_metadata =
    _ostree_detached_metadata_append_gpg_sig (old_metadata, signature);

  if (!ostree_repo_write_commit_detached_metadata (self,
                                                   commit_checksum,
                                                   new_metadata,
                                                   cancellable,
                                                   error))
    return FALSE;

  return TRUE;
}

/**
 * ostree_repo_sign_delta:
 * @self: Self
 * @from_commit: From commit
 * @to_commit: To commit
 * @key_id: key id
 * @homedir: homedir
 * @cancellable: cancellable
 * @error: error
 *
 * This function is deprecated, sign the summary file instead.
 * Add a GPG signature to a static delta.
 */
gboolean
ostree_repo_sign_delta (OstreeRepo     *self,
                        const gchar    *from_commit,
                        const gchar    *to_commit,
                        const gchar    *key_id,
                        const gchar    *homedir,
                        GCancellable   *cancellable,
                        GError        **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "ostree_repo_sign_delta is deprecated");
  return FALSE;
}

/**
 * ostree_repo_add_gpg_signature_summary:
 * @self: Self
 * @key_id: (array zero-terminated=1) (element-type utf8): NULL-terminated array of GPG keys.
 * @homedir: (allow-none): GPG home directory, or %NULL
 * @cancellable: A #GCancellable
 * @error: a #GError
 *
 * Add a GPG signature to a summary file.
 */
gboolean
ostree_repo_add_gpg_signature_summary (OstreeRepo     *self,
                                       const gchar    **key_id,
                                       const gchar    *homedir,
                                       GCancellable   *cancellable,
                                       GError        **error)
{
  glnx_autofd int fd = -1;
  if (!glnx_openat_rdonly (self->repo_dir_fd, "summary", TRUE, &fd, error))
    return FALSE;
  g_autoptr(GBytes) summary_data = ot_fd_readall_or_mmap (fd, 0, error);
  if (!summary_data)
    return FALSE;
  /* Note that fd is reused below */
  glnx_close_fd (&fd);

  g_autoptr(GVariant) existing_signatures = NULL;
  if (!ot_openat_ignore_enoent (self->repo_dir_fd, "summary.sig", &fd, error))
    return FALSE;
  if (fd != -1)
    {
      if (!ot_variant_read_fd (fd, 0, G_VARIANT_TYPE (OSTREE_SUMMARY_SIG_GVARIANT_STRING),
                               FALSE, &existing_signatures, error))
        return FALSE;
    }

  g_autoptr(GVariant) new_metadata = NULL;
  for (guint i = 0; key_id[i]; i++)
    {
      g_autoptr(GBytes) signature_data = NULL;
      if (!sign_data (self, summary_data, key_id[i], homedir,
                      &signature_data,
                      cancellable, error))
        return FALSE;

      new_metadata = _ostree_detached_metadata_append_gpg_sig (existing_signatures, signature_data);
    }

  g_autoptr(GVariant) normalized = g_variant_get_normal_form (new_metadata);

  if (!_ostree_repo_file_replace_contents (self,
                                           self->repo_dir_fd,
                                           "summary.sig",
                                           g_variant_get_data (normalized),
                                           g_variant_get_size (normalized),
                                           cancellable, error))
    return FALSE;

  return TRUE;
}

/* Special remote for _ostree_repo_gpg_verify_with_metadata() */
static const char *OSTREE_ALL_REMOTES = "__OSTREE_ALL_REMOTES__";

/* Look for a keyring for @remote in the repo itself, or in
 * /etc/ostree/remotes.d.
 */
static gboolean
find_keyring (OstreeRepo          *self,
              OstreeRemote        *remote,
              GBytes             **ret_bytes,
              GCancellable        *cancellable,
              GError             **error)
{
  glnx_autofd int fd = -1;
  if (!ot_openat_ignore_enoent (self->repo_dir_fd, remote->keyring, &fd, error))
    return FALSE;

  if (fd != -1)
    {
      GBytes *ret = glnx_fd_readall_bytes (fd, cancellable, error);
      if (!ret)
        return FALSE;
      *ret_bytes = ret;
      return TRUE;
    }

  g_autoptr(GFile) remotes_d = get_remotes_d_dir (self, NULL);
  if (remotes_d)
    {
      g_autoptr(GFile) child = g_file_get_child (remotes_d, remote->keyring);

      if (!ot_openat_ignore_enoent (AT_FDCWD, gs_file_get_path_cached (child), &fd, error))
        return FALSE;

      if (fd != -1)
        {
          GBytes *ret = glnx_fd_readall_bytes (fd, cancellable, error);
          if (!ret)
            return FALSE;
          *ret_bytes = ret;
          return TRUE;
        }
    }

  if (self->parent_repo)
    return find_keyring (self->parent_repo, remote, ret_bytes, cancellable, error);

  *ret_bytes = NULL;
  return TRUE;
}

static OstreeGpgVerifyResult *
_ostree_repo_gpg_verify_data_internal (OstreeRepo    *self,
                                       const gchar   *remote_name,
                                       GBytes        *data,
                                       GBytes        *signatures,
                                       GFile         *keyringdir,
                                       GFile         *extra_keyring,
                                       GCancellable  *cancellable,
                                       GError       **error)
{
  g_autoptr(OstreeGpgVerifier) verifier = NULL;
  gboolean add_global_keyring_dir = TRUE;

  verifier = _ostree_gpg_verifier_new ();

  if (remote_name == OSTREE_ALL_REMOTES)
    {
      /* Add all available remote keyring files. */

      if (!_ostree_gpg_verifier_add_keyring_dir_at (verifier, self->repo_dir_fd, ".",
                                                    cancellable, error))
        return NULL;
    }
  else if (remote_name != NULL)
    {
      g_autofree char *gpgkeypath = NULL;
      /* Add the remote's keyring file if it exists. */

      OstreeRemote *remote;

      remote = _ostree_repo_get_remote_inherited (self, remote_name, error);
      if (remote == NULL)
        return NULL;

      g_autoptr(GBytes) keyring_data = NULL;
      if (!find_keyring (self, remote, &keyring_data, cancellable, error))
        return NULL;

      if (keyring_data != NULL)
        {
          _ostree_gpg_verifier_add_keyring_data (verifier, keyring_data, remote->keyring);
          add_global_keyring_dir = FALSE;
        }

      if (!ot_keyfile_get_value_with_default (remote->options, remote->group, "gpgkeypath", NULL,
                                              &gpgkeypath, error))
        return NULL;

      if (gpgkeypath)
        _ostree_gpg_verifier_add_key_ascii_file (verifier, gpgkeypath);

      ostree_remote_unref (remote);
    }

  if (add_global_keyring_dir)
    {
      /* Use the deprecated global keyring directory. */
      if (!_ostree_gpg_verifier_add_global_keyring_dir (verifier, cancellable, error))
        return NULL;
    }

  if (keyringdir)
    {
      if (!_ostree_gpg_verifier_add_keyring_dir (verifier, keyringdir,
                                                 cancellable, error))
        return NULL;
    }
  if (extra_keyring != NULL)
    {
      _ostree_gpg_verifier_add_keyring_file (verifier, extra_keyring);
    }

  return _ostree_gpg_verifier_check_signature (verifier,
                                               data,
                                               signatures,
                                               cancellable,
                                               error);
}

OstreeGpgVerifyResult *
_ostree_repo_gpg_verify_with_metadata (OstreeRepo          *self,
                                       GBytes              *signed_data,
                                       GVariant            *metadata,
                                       const char          *remote_name,
                                       GFile               *keyringdir,
                                       GFile               *extra_keyring,
                                       GCancellable        *cancellable,
                                       GError             **error)
{
  g_autoptr(GVariant) signaturedata = NULL;
  GByteArray *buffer;
  GVariantIter iter;
  GVariant *child;
  g_autoptr (GBytes) signatures = NULL;

  if (metadata)
    signaturedata = g_variant_lookup_value (metadata,
                                            _OSTREE_METADATA_GPGSIGS_NAME,
                                            _OSTREE_METADATA_GPGSIGS_TYPE);
  if (!signaturedata)
    {
      g_set_error_literal (error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_NO_SIGNATURE,
                           "GPG verification enabled, but no signatures found (use gpg-verify=false in remote config to disable)");
      return NULL;
    }

  /* OpenPGP data is organized into binary records called packets.  RFC 4880
   * defines a packet as a chunk of data that has a tag specifying its meaning,
   * and consists of a packet header followed by a packet body.  Each packet
   * encodes its own length, and so packets can be concatenated to construct
   * OpenPGP messages, keyrings, or in this case, detached signatures.
   *
   * Each binary blob in the GVariant list is a complete signature packet, so
   * we can concatenate them together to verify all the signatures at once. */
  buffer = g_byte_array_new ();
  g_variant_iter_init (&iter, signaturedata);
  while ((child = g_variant_iter_next_value (&iter)) != NULL)
    {
      g_byte_array_append (buffer,
                           g_variant_get_data (child),
                           g_variant_get_size (child));
      g_variant_unref (child);
    }
  signatures = g_byte_array_free_to_bytes (buffer);

  return _ostree_repo_gpg_verify_data_internal (self,
                                                remote_name,
                                                signed_data,
                                                signatures,
                                                keyringdir,
                                                extra_keyring,
                                                cancellable,
                                                error);
}

/* Needed an internal version for the remote_name parameter. */
OstreeGpgVerifyResult *
_ostree_repo_verify_commit_internal (OstreeRepo    *self,
                                     const char    *commit_checksum,
                                     const char    *remote_name,
                                     GFile         *keyringdir,
                                     GFile         *extra_keyring,
                                     GCancellable  *cancellable,
                                     GError       **error)
{
  g_autoptr(GVariant) commit_variant = NULL;
  /* Load the commit */
  if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_COMMIT,
                                 commit_checksum, &commit_variant,
                                 error))
    return glnx_prefix_error_null (error, "Failed to read commit");

  /* Load the metadata */
  g_autoptr(GVariant) metadata = NULL;
  if (!ostree_repo_read_commit_detached_metadata (self,
                                                  commit_checksum,
                                                  &metadata,
                                                  cancellable,
                                                  error))
    return glnx_prefix_error_null (error, "Failed to read detached metadata");

  g_autoptr(GBytes) signed_data = g_variant_get_data_as_bytes (commit_variant);

  /* XXX This is a hackish way to indicate to use ALL remote-specific
   *     keyrings in the signature verification.  We want this when
   *     verifying a signed commit that's already been pulled. */
  if (remote_name == NULL)
    remote_name = OSTREE_ALL_REMOTES;

  return _ostree_repo_gpg_verify_with_metadata (self, signed_data,
                                                metadata, remote_name,
                                                keyringdir, extra_keyring,
                                                cancellable, error);
}

/**
 * ostree_repo_verify_commit:
 * @self: Repository
 * @commit_checksum: ASCII SHA256 checksum
 * @keyringdir: (allow-none): Path to directory GPG keyrings; overrides built-in default if given
 * @extra_keyring: (allow-none): Path to additional keyring file (not a directory)
 * @cancellable: Cancellable
 * @error: Error
 *
 * Check for a valid GPG signature on commit named by the ASCII
 * checksum @commit_checksum.
 *
 * Returns: %TRUE if there was a GPG signature from a trusted keyring, otherwise %FALSE
 */
gboolean
ostree_repo_verify_commit (OstreeRepo   *self,
                           const gchar  *commit_checksum,
                           GFile        *keyringdir,
                           GFile        *extra_keyring,
                           GCancellable *cancellable,
                           GError      **error)
{
  g_autoptr(OstreeGpgVerifyResult) result = NULL;

  result = ostree_repo_verify_commit_ext (self, commit_checksum,
                                          keyringdir, extra_keyring,
                                          cancellable, error);

  if (!ostree_gpg_verify_result_require_valid_signature (result, error))
    return glnx_prefix_error (error, "Commit %s", commit_checksum);
  return TRUE;
}

/**
 * ostree_repo_verify_commit_ext:
 * @self: Repository
 * @commit_checksum: ASCII SHA256 checksum
 * @keyringdir: (allow-none): Path to directory GPG keyrings; overrides built-in default if given
 * @extra_keyring: (allow-none): Path to additional keyring file (not a directory)
 * @cancellable: Cancellable
 * @error: Error
 *
 * Read GPG signature(s) on the commit named by the ASCII checksum
 * @commit_checksum and return detailed results.
 *
 * Returns: (transfer full): an #OstreeGpgVerifyResult, or %NULL on error
 */
OstreeGpgVerifyResult *
ostree_repo_verify_commit_ext (OstreeRepo    *self,
                               const gchar   *commit_checksum,
                               GFile         *keyringdir,
                               GFile         *extra_keyring,
                               GCancellable  *cancellable,
                               GError       **error)
{
  return _ostree_repo_verify_commit_internal (self,
                                              commit_checksum,
                                              NULL,
                                              keyringdir,
                                              extra_keyring,
                                              cancellable,
                                              error);
}

/**
 * ostree_repo_verify_commit_for_remote:
 * @self: Repository
 * @commit_checksum: ASCII SHA256 checksum
 * @remote_name: OSTree remote to use for configuration
 * @cancellable: Cancellable
 * @error: Error
 *
 * Read GPG signature(s) on the commit named by the ASCII checksum
 * @commit_checksum and return detailed results, based on the keyring
 * configured for @remote.
 *
 * Returns: (transfer full): an #OstreeGpgVerifyResult, or %NULL on error
 */
OstreeGpgVerifyResult *
ostree_repo_verify_commit_for_remote (OstreeRepo    *self,
                                      const gchar   *commit_checksum,
                                      const gchar   *remote_name,
                                      GCancellable  *cancellable,
                                      GError       **error)
{
  return _ostree_repo_verify_commit_internal (self,
                                              commit_checksum,
                                              remote_name,
                                              NULL,
                                              NULL,
                                              cancellable,
                                              error);
}

/**
 * ostree_repo_gpg_verify_data:
 * @self: Repository
 * @remote_name: (nullable): Name of remote
 * @data: Data as a #GBytes
 * @signatures: Signatures as a #GBytes
 * @keyringdir: (nullable): Path to directory GPG keyrings; overrides built-in default if given
 * @extra_keyring: (nullable): Path to additional keyring file (not a directory)
 * @cancellable: Cancellable
 * @error: Error
 *
 * Verify @signatures for @data using GPG keys in the keyring for
 * @remote_name, and return an #OstreeGpgVerifyResult.
 *
 * The @remote_name parameter can be %NULL. In that case it will do
 * the verifications using GPG keys in the keyrings of all remotes.
 *
 * Returns: (transfer full): an #OstreeGpgVerifyResult, or %NULL on error
 */
OstreeGpgVerifyResult *
ostree_repo_gpg_verify_data (OstreeRepo    *self,
                             const gchar   *remote_name,
                             GBytes        *data,
                             GBytes        *signatures,
                             GFile         *keyringdir,
                             GFile         *extra_keyring,
                             GCancellable  *cancellable,
                             GError       **error)
{
  g_return_val_if_fail (OSTREE_IS_REPO (self), NULL);
  g_return_val_if_fail (data != NULL, NULL);
  g_return_val_if_fail (signatures != NULL, NULL);

  return _ostree_repo_gpg_verify_data_internal (self,
                                                (remote_name != NULL) ? remote_name : OSTREE_ALL_REMOTES,
                                                data,
                                                signatures,
                                                keyringdir,
                                                extra_keyring,
                                                cancellable,
                                                error);
}

/**
 * ostree_repo_verify_summary:
 * @self: Repo
 * @remote_name: Name of remote
 * @summary: Summary data as a #GBytes
 * @signatures: Summary signatures as a #GBytes
 * @cancellable: Cancellable
 * @error: Error
 *
 * Verify @signatures for @summary data using GPG keys in the keyring for
 * @remote_name, and return an #OstreeGpgVerifyResult.
 *
 * Returns: (transfer full): an #OstreeGpgVerifyResult, or %NULL on error
 */
OstreeGpgVerifyResult *
ostree_repo_verify_summary (OstreeRepo    *self,
                            const char    *remote_name,
                            GBytes        *summary,
                            GBytes        *signatures,
                            GCancellable  *cancellable,
                            GError       **error)
{
  g_autoptr(GVariant) signatures_variant = NULL;

  g_return_val_if_fail (OSTREE_IS_REPO (self), NULL);
  g_return_val_if_fail (remote_name != NULL, NULL);
  g_return_val_if_fail (summary != NULL, NULL);
  g_return_val_if_fail (signatures != NULL, NULL);

  signatures_variant = g_variant_new_from_bytes (OSTREE_SUMMARY_SIG_GVARIANT_FORMAT,
                                                 signatures, FALSE);

  return _ostree_repo_gpg_verify_with_metadata (self,
                                                summary,
                                                signatures_variant,
                                                remote_name,
                                                NULL, NULL,
                                                cancellable,
                                                error);
}

/* Add an entry for a @ref ↦ @checksum mapping to an `a(s(t@ay@a{sv}))`
 * @refs_builder to go into a `summary` file. This includes building the
 * standard additional metadata keys for the ref. */
static gboolean
summary_add_ref_entry (OstreeRepo       *self,
                       const char       *ref,
                       const char       *checksum,
                       GVariantBuilder  *refs_builder,
                       GError          **error)
{
  g_auto(GVariantDict) commit_metadata_builder = OT_VARIANT_BUILDER_INITIALIZER;

  g_assert (ref);  g_assert (checksum);

  g_autofree char *remotename = NULL;
  if (!ostree_parse_refspec (ref, &remotename, NULL, NULL))
    g_assert_not_reached ();

  /* Don't put remote refs in the summary */
  if (remotename != NULL)
    return TRUE;

  g_autoptr(GVariant) commit_obj = NULL;
  if (!ostree_repo_load_variant (self, OSTREE_OBJECT_TYPE_COMMIT, checksum, &commit_obj, error))
    return FALSE;

  g_variant_dict_init (&commit_metadata_builder, NULL);

  /* Forward the commit’s timestamp if it’s valid. */
  guint64 commit_timestamp = ostree_commit_get_timestamp (commit_obj);
  g_autoptr(GDateTime) dt = g_date_time_new_from_unix_utc (commit_timestamp);

  if (dt != NULL)
    g_variant_dict_insert_value (&commit_metadata_builder, OSTREE_COMMIT_TIMESTAMP,
                                 g_variant_new_uint64 (GUINT64_TO_BE (commit_timestamp)));

  g_variant_builder_add_value (refs_builder,
                               g_variant_new ("(s(t@ay@a{sv}))", ref,
                                              (guint64) g_variant_get_size (commit_obj),
                                              ostree_checksum_to_bytes_v (checksum),
                                              g_variant_dict_end (&commit_metadata_builder)));

  return TRUE;
}

/**
 * ostree_repo_regenerate_summary:
 * @self: Repo
 * @additional_metadata: (allow-none): A GVariant of type a{sv}, or %NULL
 * @cancellable: Cancellable
 * @error: Error
 *
 * An OSTree repository can contain a high level "summary" file that
 * describes the available branches and other metadata.
 *
 * If the timetable for making commits and updating the summary file is fairly
 * regular, setting the `ostree.summary.expires` key in @additional_metadata
 * will aid clients in working out when to check for updates.
 *
 * It is regenerated automatically after a commit if
 * `core/commit-update-summary` is set.
 *
 * If the `core/collection-id` key is set in the configuration, it will be
 * included as %OSTREE_SUMMARY_COLLECTION_ID in the summary file. Refs from the
 * `refs/mirrors` directory will be included in the generated summary file,
 * listed under the %OSTREE_SUMMARY_COLLECTION_MAP key. Collection IDs and refs
 * in %OSTREE_SUMMARY_COLLECTION_MAP are guaranteed to be in lexicographic
 * order.
 */
gboolean
ostree_repo_regenerate_summary (OstreeRepo     *self,
                                GVariant       *additional_metadata,
                                GCancellable   *cancellable,
                                GError        **error)
{
  g_auto(GVariantDict) additional_metadata_builder = OT_VARIANT_BUILDER_INITIALIZER;
  g_variant_dict_init (&additional_metadata_builder, additional_metadata);
  g_autoptr(GVariantBuilder) refs_builder = g_variant_builder_new (G_VARIANT_TYPE ("a(s(taya{sv}))"));

  const gchar *main_collection_id = ostree_repo_get_collection_id (self);

  {
    if (main_collection_id == NULL)
      {
        g_autoptr(GHashTable) refs = NULL;
        if (!ostree_repo_list_refs (self, NULL, &refs, cancellable, error))
          return FALSE;

        g_autoptr(GList) ordered_keys = g_hash_table_get_keys (refs);
        ordered_keys = g_list_sort (ordered_keys, (GCompareFunc)strcmp);

        for (GList *iter = ordered_keys; iter; iter = iter->next)
          {
            const char *ref = iter->data;
            const char *commit = g_hash_table_lookup (refs, ref);

            if (!summary_add_ref_entry (self, ref, commit, refs_builder, error))
              return FALSE;
          }
      }
  }

  {
    g_autoptr(GPtrArray) delta_names = NULL;
    g_auto(GVariantDict) deltas_builder = OT_VARIANT_BUILDER_INITIALIZER;

    if (!ostree_repo_list_static_delta_names (self, &delta_names, cancellable, error))
      return FALSE;

    g_variant_dict_init (&deltas_builder, NULL);
    for (guint i = 0; i < delta_names->len; i++)
      {
        g_autofree char *from = NULL;
        g_autofree char *to = NULL;
        if (!_ostree_parse_delta_name (delta_names->pdata[i], &from, &to, error))
          return FALSE;

        g_autofree char *superblock = _ostree_get_relative_static_delta_superblock_path ((from && from[0]) ? from : NULL, to);
        glnx_autofd int superblock_file_fd = -1;

        if (!glnx_openat_rdonly (self->repo_dir_fd, superblock, TRUE, &superblock_file_fd, error))
          return FALSE;

        g_autoptr(GBytes) superblock_content = ot_fd_readall_or_mmap (superblock_file_fd, 0, error);
        if (!superblock_content)
          return FALSE;
        g_auto(OtChecksum) hasher = { 0, };
        ot_checksum_init (&hasher);
        ot_checksum_update_bytes (&hasher, superblock_content);
        guint8 digest[OSTREE_SHA256_DIGEST_LEN];
        ot_checksum_get_digest (&hasher, digest, sizeof (digest));

        g_variant_dict_insert_value (&deltas_builder, delta_names->pdata[i], ot_gvariant_new_bytearray (digest, sizeof (digest)));
      }

    if (delta_names->len > 0)
      g_variant_dict_insert_value (&additional_metadata_builder, OSTREE_SUMMARY_STATIC_DELTAS, g_variant_dict_end (&deltas_builder));
  }

  {
    g_variant_dict_insert_value (&additional_metadata_builder, OSTREE_SUMMARY_LAST_MODIFIED,
                                 g_variant_new_uint64 (GUINT64_TO_BE (g_get_real_time () / G_USEC_PER_SEC)));
  }

  /* Add refs which have a collection specified, which could be in refs/mirrors,
   * refs/heads, and/or refs/remotes. */
  {
    g_autoptr(GHashTable) collection_refs = NULL;
    if (!ostree_repo_list_collection_refs (self, NULL, &collection_refs,
                                           OSTREE_REPO_LIST_REFS_EXT_NONE, cancellable, error))
      return FALSE;

    gsize collection_map_size = 0;
    GHashTableIter iter;
    g_autoptr(GHashTable) collection_map = NULL;  /* (element-type utf8 GHashTable) */
    g_hash_table_iter_init (&iter, collection_refs);
    collection_map = g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
                                            (GDestroyNotify) g_hash_table_unref);

    const OstreeCollectionRef *ref;
    const char *checksum;
    while (g_hash_table_iter_next (&iter, (gpointer *) &ref, (gpointer *) &checksum))
      {
        GHashTable *ref_map = g_hash_table_lookup (collection_map, ref->collection_id);

        if (ref_map == NULL)
          {
            ref_map = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);
            g_hash_table_insert (collection_map, ref->collection_id, ref_map);
          }

        g_hash_table_insert (ref_map, ref->ref_name, (gpointer) checksum);
      }

    g_autoptr(GVariantBuilder) collection_refs_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sa(s(taya{sv}))}"));

    g_autoptr(GList) ordered_collection_ids = g_hash_table_get_keys (collection_map);
    ordered_collection_ids = g_list_sort (ordered_collection_ids, (GCompareFunc) strcmp);

    for (GList *collection_iter = ordered_collection_ids; collection_iter; collection_iter = collection_iter->next)
      {
        const char *collection_id = collection_iter->data;
        GHashTable *ref_map = g_hash_table_lookup (collection_map, collection_id);

        /* We put the local repo's collection ID in the main refs map, rather
         * than the collection map, for backwards compatibility. */
        gboolean is_main_collection_id = (main_collection_id != NULL && g_str_equal (collection_id, main_collection_id));

        if (!is_main_collection_id)
          {
            g_variant_builder_open (collection_refs_builder, G_VARIANT_TYPE ("{sa(s(taya{sv}))}"));
            g_variant_builder_add (collection_refs_builder, "s", collection_id);
            g_variant_builder_open (collection_refs_builder, G_VARIANT_TYPE ("a(s(taya{sv}))"));
          }

        g_autoptr(GList) ordered_refs = g_hash_table_get_keys (ref_map);
        ordered_refs = g_list_sort (ordered_refs, (GCompareFunc) strcmp);

        for (GList *ref_iter = ordered_refs; ref_iter != NULL; ref_iter = ref_iter->next)
          {
            const char *ref = ref_iter->data;
            const char *commit = g_hash_table_lookup (ref_map, ref);
            GVariantBuilder *builder = is_main_collection_id ? refs_builder : collection_refs_builder;

            if (!summary_add_ref_entry (self, ref, commit, builder, error))
              return FALSE;

            if (!is_main_collection_id)
              collection_map_size++;
          }

        if (!is_main_collection_id)
          {
            g_variant_builder_close (collection_refs_builder);  /* array */
            g_variant_builder_close (collection_refs_builder);  /* dict entry */
          }
      }

    if (main_collection_id != NULL)
      g_variant_dict_insert_value (&additional_metadata_builder, OSTREE_SUMMARY_COLLECTION_ID,
                                   g_variant_new_string (main_collection_id));
    if (collection_map_size > 0)
      g_variant_dict_insert_value (&additional_metadata_builder, OSTREE_SUMMARY_COLLECTION_MAP,
                                   g_variant_builder_end (collection_refs_builder));
  }

  g_autoptr(GVariant) summary = NULL;
  {
    g_autoptr(GVariantBuilder) summary_builder =
      g_variant_builder_new (OSTREE_SUMMARY_GVARIANT_FORMAT);

    g_variant_builder_add_value (summary_builder, g_variant_builder_end (refs_builder));
    g_variant_builder_add_value (summary_builder, g_variant_dict_end (&additional_metadata_builder));
    summary = g_variant_builder_end (summary_builder);
    g_variant_ref_sink (summary);
  }

  if (!_ostree_repo_file_replace_contents (self,
                                           self->repo_dir_fd,
                                           "summary",
                                           g_variant_get_data (summary),
                                           g_variant_get_size (summary),
                                           cancellable,
                                           error))
    return FALSE;

  if (!ot_ensure_unlinked_at (self->repo_dir_fd, "summary.sig", error))
    return FALSE;

  return TRUE;
}

gboolean
_ostree_repo_is_locked_tmpdir (const char *filename)
{
  return g_str_has_prefix (filename, OSTREE_REPO_TMPDIR_STAGING);
}

gboolean
_ostree_repo_try_lock_tmpdir (int            tmpdir_dfd,
                              const char    *tmpdir_name,
                              GLnxLockFile  *file_lock_out,
                              gboolean      *out_did_lock,
                              GError       **error)
{
  g_autofree char *lock_name = g_strconcat (tmpdir_name, "-lock", NULL);
  gboolean did_lock = FALSE;
  g_autoptr(GError) local_error = NULL;

  /* We put the lock outside the dir, so we can hold the lock
   * until the directory is fully removed */
  if (!glnx_make_lock_file (tmpdir_dfd, lock_name, LOCK_EX | LOCK_NB,
                            file_lock_out, &local_error))
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
        {
          did_lock = FALSE;
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
    }
  else
    {
      /* It's possible that we got a lock after seeing the directory, but
       * another process deleted the tmpdir, so verify it still exists.
       */
      struct stat stbuf;
      if (!glnx_fstatat_allow_noent (tmpdir_dfd, tmpdir_name, &stbuf, AT_SYMLINK_NOFOLLOW, error))
        return FALSE;
      if (errno == 0 && S_ISDIR (stbuf.st_mode))
        did_lock = TRUE;
      else
        glnx_release_lock_file (file_lock_out);
    }

  *out_did_lock = did_lock;
  return TRUE;
}

/* This allocates and locks a subdir of the repo tmp dir, using an existing
 * one with the same prefix if it is not in use already. */
gboolean
_ostree_repo_allocate_tmpdir (int tmpdir_dfd,
                              const char *tmpdir_prefix,
                              GLnxTmpDir *tmpdir_out,
                              GLnxLockFile *file_lock_out,
                              gboolean *reusing_dir_out,
                              GCancellable *cancellable,
                              GError **error)
{
  g_return_val_if_fail (_ostree_repo_is_locked_tmpdir (tmpdir_prefix), FALSE);

  /* Look for existing tmpdir (with same prefix) to reuse */
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  if (!glnx_dirfd_iterator_init_at (tmpdir_dfd, ".", FALSE, &dfd_iter, error))
    return FALSE;

  gboolean reusing_dir = FALSE;
  gboolean did_lock = FALSE;
  g_auto(GLnxTmpDir) ret_tmpdir = { 0, };
  while (!ret_tmpdir.initialized)
    {
      struct dirent *dent;
      g_autoptr(GError) local_error = NULL;

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;

      if (dent == NULL)
        break;

      if (!g_str_has_prefix (dent->d_name, tmpdir_prefix))
        continue;

      /* Quickly skip non-dirs, if unknown we ignore ENOTDIR when opening instead */
      if (dent->d_type != DT_UNKNOWN &&
          dent->d_type != DT_DIR)
        continue;

      glnx_autofd int target_dfd = -1;
      if (!glnx_opendirat (dfd_iter.fd, dent->d_name, FALSE,
                           &target_dfd, &local_error))
        {
          if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY) ||
              g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            continue;
          else
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }
        }

      /* We put the lock outside the dir, so we can hold the lock
       * until the directory is fully removed */
      if (!_ostree_repo_try_lock_tmpdir (tmpdir_dfd, dent->d_name,
                                         file_lock_out, &did_lock,
                                         error))
        return FALSE;
      if (!did_lock)
        continue;

      /* Touch the reused directory so that we don't accidentally
       * remove it due to being old when cleaning up the tmpdir.
       */
      (void)futimens (target_dfd, NULL);

      /* We found an existing tmpdir which we managed to lock */
      g_debug ("Reusing tmpdir %s", dent->d_name);
      reusing_dir = TRUE;
      ret_tmpdir.src_dfd = tmpdir_dfd;
      ret_tmpdir.fd = glnx_steal_fd (&target_dfd);
      ret_tmpdir.path = g_strdup (dent->d_name);
      ret_tmpdir.initialized = TRUE;
    }

  const char *tmpdir_name_template = glnx_strjoina (tmpdir_prefix, "XXXXXX");
  while (!ret_tmpdir.initialized)
    {
      g_auto(GLnxTmpDir) new_tmpdir = { 0, };
      /* No existing tmpdir found, create a new */
      if (!glnx_mkdtempat (tmpdir_dfd, tmpdir_name_template, 0755,
                           &new_tmpdir, error))
        return FALSE;

      /* Note, at this point we can race with another process that picks up this
       * new directory. If that happens we need to retry, making a new directory. */
      if (!_ostree_repo_try_lock_tmpdir (new_tmpdir.src_dfd, new_tmpdir.path,
                                         file_lock_out, &did_lock,
                                         error))
        return FALSE;
      if (!did_lock)
        {
          /* We raced and someone else already locked the newly created
           * directory. Free the resources here and then mark it as
           * uninitialized so glnx_tmpdir_cleanup doesn't delete the directory
           * when new_tmpdir goes out of scope.
           */
          glnx_tmpdir_unset (&new_tmpdir);
          new_tmpdir.initialized = FALSE;
          continue;
        }

      g_debug ("Using new tmpdir %s", new_tmpdir.path);
      ret_tmpdir = new_tmpdir; /* Transfer ownership */
      new_tmpdir.initialized = FALSE;
    }

  *tmpdir_out = ret_tmpdir; /* Transfer ownership */
  ret_tmpdir.initialized = FALSE;
  *reusing_dir_out = reusing_dir;
  return TRUE;
}

/* See ostree-repo-private.h for more information about this */
void
_ostree_repo_memory_cache_ref_init (OstreeRepoMemoryCacheRef *state,
                                    OstreeRepo               *repo)
{
  state->repo = g_object_ref (repo);
  GMutex *lock = &repo->cache_lock;
  g_mutex_lock (lock);
  repo->dirmeta_cache_refcount++;
  if (repo->dirmeta_cache == NULL)
    repo->dirmeta_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
  g_mutex_unlock (lock);

}

/* See ostree-repo-private.h for more information about this */
void
_ostree_repo_memory_cache_ref_destroy (OstreeRepoMemoryCacheRef *state)
{
  OstreeRepo *repo = state->repo;
  GMutex *lock = &repo->cache_lock;
  g_mutex_lock (lock);
  repo->dirmeta_cache_refcount--;
  if (repo->dirmeta_cache_refcount == 0)
    g_clear_pointer (&repo->dirmeta_cache, (GDestroyNotify) g_hash_table_unref);
  g_mutex_unlock (lock);
  g_object_unref (repo);
}

/**
 * ostree_repo_get_collection_id:
 * @self: an #OstreeRepo
 *
 * Get the collection ID of this repository. See [collection IDs][collection-ids].
 *
 * Returns: (nullable): collection ID for the repository
 * Since: 2017.8
 */
const gchar *
ostree_repo_get_collection_id (OstreeRepo *self)
{
  g_return_val_if_fail (OSTREE_IS_REPO (self), NULL);

  return self->collection_id;
}

/**
 * ostree_repo_set_collection_id:
 * @self: an #OstreeRepo
 * @collection_id: (nullable): new collection ID, or %NULL to unset it
 * @error: return location for a #GError, or %NULL
 *
 * Set or clear the collection ID of this repository. See [collection IDs][collection-ids].
 * The update will be made in memory, but must be written out to the repository
 * configuration on disk using ostree_repo_write_config().
 *
 * Returns: %TRUE on success, %FALSE otherwise
 * Since: 2017.8
 */
gboolean
ostree_repo_set_collection_id (OstreeRepo   *self,
                               const gchar  *collection_id,
                               GError      **error)
{
  if (collection_id != NULL && !ostree_validate_collection_id (collection_id, error))
    return FALSE;

  g_autofree gchar *new_collection_id = g_strdup (collection_id);
  g_free (self->collection_id);
  self->collection_id = g_steal_pointer (&new_collection_id);

  if (self->config != NULL)
    {
      if (collection_id != NULL)
        g_key_file_set_string (self->config, "core", "collection-id", collection_id);
      else
        return g_key_file_remove_key (self->config, "core", "collection-id", error);
    }

  return TRUE;
}
