/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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
 */

#include "config.h"

#include "otutil.h"
#include <err.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include "ostree-bootloader-aboot.h"
#include "ostree-bootloader-grub2.h"
#include "ostree-bootloader-syslinux.h"
#include "ostree-bootloader-uboot.h"
#include "ostree-bootloader-zipl.h"
#include "ostree-core-private.h"
#include "ostree-deployment-private.h"
#include "ostree-repo-private.h"
#include "ostree-sepolicy-private.h"
#include "ostree-sysroot-private.h"
#include "otcore.h"

#define BOOTVERSION_FILE "boot/loader/ostree_bootversion"

/**
 * SECTION:ostree-sysroot
 * @title: Root partition mount point
 * @short_description: Manage physical root filesystem
 *
 * A #OstreeSysroot object represents a physical root filesystem,
 * which in particular should contain a toplevel /ostree directory.
 * Inside this directory is an #OstreeRepo in /ostree/repo, plus a set
 * of deployments in /ostree/deploy.
 *
 * This class is not by default safe against concurrent use by threads
 * or external processes.  You can use ostree_sysroot_lock() to
 * perform locking externally.
 */
typedef struct
{
  GObjectClass parent_class;

  /* Signals */
  void (*journal_msg) (OstreeSysroot *sysroot, const char *msg);
} OstreeSysrootClass;

enum
{
  JOURNAL_MSG_SIGNAL,
  LAST_SIGNAL,
};
static guint signals[LAST_SIGNAL] = { 0 };

enum
{
  PROP_0,

  PROP_PATH
};

G_DEFINE_TYPE (OstreeSysroot, ostree_sysroot, G_TYPE_OBJECT)

static void
ostree_sysroot_finalize (GObject *object)
{
  OstreeSysroot *self = OSTREE_SYSROOT (object);

  g_clear_object (&self->path);
  g_clear_object (&self->repo);
  g_clear_pointer (&self->run_ostree_metadata, g_variant_dict_unref);
  g_clear_pointer (&self->deployments, g_ptr_array_unref);
  g_clear_object (&self->booted_deployment);
  g_clear_object (&self->staged_deployment);
  g_clear_pointer (&self->staged_deployment_data, g_variant_unref);

  glnx_release_lock_file (&self->lock);

  ostree_sysroot_unload (self);

  G_OBJECT_CLASS (ostree_sysroot_parent_class)->finalize (object);
}

static void
ostree_sysroot_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  OstreeSysroot *self = OSTREE_SYSROOT (object);

  switch (prop_id)
    {
    case PROP_PATH:
      self->path = g_value_dup_object (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_sysroot_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  OstreeSysroot *self = OSTREE_SYSROOT (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_object (value, self->path);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_sysroot_constructed (GObject *object)
{
  OstreeSysroot *self = OSTREE_SYSROOT (object);

  /* Ensure the system root path is set. */
  if (self->path == NULL)
    self->path = g_object_ref (_ostree_get_default_sysroot_path ());

  G_OBJECT_CLASS (ostree_sysroot_parent_class)->constructed (object);
}

static void
ostree_sysroot_class_init (OstreeSysrootClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = ostree_sysroot_constructed;
  object_class->get_property = ostree_sysroot_get_property;
  object_class->set_property = ostree_sysroot_set_property;
  object_class->finalize = ostree_sysroot_finalize;

  g_object_class_install_property (
      object_class, PROP_PATH,
      g_param_spec_object ("path", "", "", G_TYPE_FILE,
                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  /**
   * OstreeSysroot::journal-msg:
   * @self: Self
   * @msg: Human-readable string (should not contain newlines)
   *
   * libostree will log to the journal various events, such as the /etc merge
   * status, and transaction completion. Connect to this signal to also
   * synchronously receive the text for those messages. This is intended to be
   * used by command line tools which link to libostree as a library.
   *
   * Currently, the structured data is only available via the systemd journal.
   *
   * Since: 2017.10
   */
  signals[JOURNAL_MSG_SIGNAL]
      = g_signal_new ("journal-msg", G_OBJECT_CLASS_TYPE (object_class), G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (OstreeSysrootClass, journal_msg), NULL, NULL, NULL,
                      G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
ostree_sysroot_init (OstreeSysroot *self)
{
  const GDebugKey globalopt_keys[] = {
    { "skip-sync", OSTREE_SYSROOT_GLOBAL_OPT_SKIP_SYNC },
    { "no-early-prune", OSTREE_SYSROOT_GLOBAL_OPT_NO_EARLY_PRUNE },
    { "bootloader-naming-1", OSTREE_SYSROOT_GLOBAL_OPT_BOOTLOADER_NAMING_1 },
  };
  const GDebugKey keys[] = {
    { "mutable-deployments", OSTREE_SYSROOT_DEBUG_MUTABLE_DEPLOYMENTS },
    { "test-fifreeze", OSTREE_SYSROOT_DEBUG_TEST_FIFREEZE },
    { "no-xattrs", OSTREE_SYSROOT_DEBUG_NO_XATTRS },
    { "no-dtb", OSTREE_SYSROOT_DEBUG_TEST_NO_DTB },
  };

  self->opt_flags = g_parse_debug_string (g_getenv ("OSTREE_SYSROOT_OPTS"), globalopt_keys,
                                          G_N_ELEMENTS (globalopt_keys));
  self->debug_flags
      = g_parse_debug_string (g_getenv ("OSTREE_SYSROOT_DEBUG"), keys, G_N_ELEMENTS (keys));

  self->sysroot_fd = -1;
  self->boot_fd = -1;
}

/**
 * ostree_sysroot_new:
 * @path: (allow-none): Path to a system root directory, or %NULL to use the
 *   current visible root file system
 *
 * Create a new #OstreeSysroot object for the sysroot at @path. If @path is %NULL,
 * the current visible root file system is used, equivalent to
 * ostree_sysroot_new_default().
 *
 * Returns: (transfer full): An accessor object for an system root located at @path
 */
OstreeSysroot *
ostree_sysroot_new (GFile *path)
{
  return g_object_new (OSTREE_TYPE_SYSROOT, "path", path, NULL);
}

/**
 * ostree_sysroot_new_default:
 *
 * Returns: (transfer full): An accessor for the current visible root / filesystem
 */
OstreeSysroot *
ostree_sysroot_new_default (void)
{
  return ostree_sysroot_new (NULL);
}

/**
 * ostree_sysroot_set_mount_namespace_in_use:
 *
 * If this function is invoked, then libostree will assume that
 * a private Linux mount namespace has been created by the process.
 * The primary use case for this is to have e.g. /sysroot mounted
 * read-only by default.
 *
 * If this function has been called, then when a function which requires
 * writable access is invoked, libostree will automatically remount as writable
 * any mount points on which it operates.  This currently is just `/sysroot` and
 * `/boot`.
 *
 * If you invoke this function, it must be before ostree_sysroot_load(); it may
 * be invoked before or after ostree_sysroot_initialize().
 *
 * Since: 2020.1
 */
void
ostree_sysroot_set_mount_namespace_in_use (OstreeSysroot *self)
{
  /* Must be before we're loaded, as otherwise we'd have to close/reopen all our
     fds, e.g. the repo */
  g_return_if_fail (self->loadstate < OSTREE_SYSROOT_LOAD_STATE_LOADED);
  self->mount_namespace_in_use = TRUE;
}

/**
 * ostree_sysroot_initialize_with_mount_namespace:
 *
 * Prepare the current process for modifying a booted sysroot, if applicable.
 * This function subsumes the functionality of `ostree_sysroot_initialize`
 * and may be invoked wherever that function is.
 *
 * If the sysroot does not appear to be booted, or where the current process is not uid 0,
 * this function returns successfully.
 *
 * Otherwise, if the process is in the same mount namespace as pid 1, create
 * a new namespace.
 *
 * If you invoke this function, it must be before ostree_sysroot_load(); it may
 * be invoked before or after ostree_sysroot_initialize().
 *
 * Since: 2022.7
 */
gboolean
ostree_sysroot_initialize_with_mount_namespace (OstreeSysroot *self, GCancellable *cancellable,
                                                GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Initializing with mountns", error);
  /* Must be before we're loaded, as otherwise we'd have to close/reopen all our
     fds, e.g. the repo */
  g_assert (self->loadstate < OSTREE_SYSROOT_LOAD_STATE_LOADED);

  if (!ostree_sysroot_initialize (self, error))
    return FALSE;

  /* Do nothing if we're not privileged */
  if (!ot_util_process_privileged ())
    return TRUE;

  /* We also assume operating on non-booted roots won't have a readonly sysroot */
  if (!self->root_is_ostree_booted)
    return TRUE;

  g_autofree char *mntns_pid1
      = glnx_readlinkat_malloc (AT_FDCWD, "/proc/1/ns/mnt", cancellable, error);
  if (!mntns_pid1)
    return glnx_prefix_error (error, "Reading /proc/1/ns/mnt");
  g_autofree char *mntns_self
      = glnx_readlinkat_malloc (AT_FDCWD, "/proc/self/ns/mnt", cancellable, error);
  if (!mntns_self)
    return glnx_prefix_error (error, "Reading /proc/self/ns/mnt");

  // If the mount namespaces are the same, we need to unshare().
  if (strcmp (mntns_pid1, mntns_self) == 0)
    {
      if (unshare (CLONE_NEWNS) < 0)
        return glnx_throw_errno_prefix (error, "Failed to invoke unshare(CLONE_NEWNS)");
    }

  ostree_sysroot_set_mount_namespace_in_use (self);
  return TRUE;
}

/**
 * ostree_sysroot_get_path:
 * @self: Sysroot
 *
 * Returns: (transfer none) (not nullable): Path to rootfs
 */
GFile *
ostree_sysroot_get_path (OstreeSysroot *self)
{
  return self->path;
}

/* Open a directory file descriptor for the sysroot if we haven't yet */
static gboolean
ensure_sysroot_fd (OstreeSysroot *self, GError **error)
{
  if (self->sysroot_fd == -1)
    {
      if (!glnx_opendirat (AT_FDCWD, gs_file_get_path_cached (self->path), TRUE, &self->sysroot_fd,
                           error))
        return FALSE;
    }

  return TRUE;
}

gboolean
_ostree_sysroot_ensure_boot_fd (OstreeSysroot *self, GError **error)
{
  if (self->boot_fd == -1)
    {
      if (!glnx_opendirat (self->sysroot_fd, "boot", TRUE, &self->boot_fd, error))
        return FALSE;
    }
  return TRUE;
}

static gboolean
remount_writable (const char *path, gboolean *did_remount, GError **error)
{
  *did_remount = FALSE;
  struct statvfs stvfsbuf;
  if (statvfs (path, &stvfsbuf) < 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "statvfs(%s)", path);
      else
        return TRUE;
    }

  if ((stvfsbuf.f_flag & ST_RDONLY) != 0)
    {
      /* OK, let's remount writable. */
      if (mount (path, path, NULL, MS_REMOUNT | MS_RELATIME, "") < 0)
        return glnx_throw_errno_prefix (error, "Remounting %s read-write", path);
      *did_remount = TRUE;
      g_debug ("remounted %s writable", path);
    }

  return TRUE;
}

/* Remount /sysroot read-write if necessary */
gboolean
_ostree_sysroot_ensure_writable (OstreeSysroot *self, GError **error)
{
  if (!ostree_sysroot_initialize (self, error))
    return FALSE;

  /* Do nothing if no mount namespace is in use */
  if (!self->mount_namespace_in_use)
    return TRUE;

  /* If we aren't operating on a booted system, then we don't
   * do anything with mounts.
   */
  if (!self->root_is_ostree_booted)
    return TRUE;

  /* In these cases we also require /boot */
  if (!_ostree_sysroot_ensure_boot_fd (self, error))
    return FALSE;

  gboolean did_remount_sysroot = FALSE;
  if (!remount_writable ("/sysroot", &did_remount_sysroot, error))
    return FALSE;
  gboolean did_remount_boot = FALSE;
  if (!remount_writable ("/boot", &did_remount_boot, error))
    return FALSE;

  /* Now close and reopen our file descriptors */
  ostree_sysroot_unload (self);
  if (!ensure_sysroot_fd (self, error))
    return FALSE;
  if (!_ostree_sysroot_ensure_boot_fd (self, error))
    return FALSE;

  return TRUE;
}

/**
 * ostree_sysroot_get_fd:
 * @self: Sysroot
 *
 * Access a file descriptor that refers to the root directory of this sysroot.
 * ostree_sysroot_initialize() (or ostree_sysroot_load()) must have been invoked
 * prior to calling this function.
 *
 * Returns: A file descriptor valid for the lifetime of @self
 */
int
ostree_sysroot_get_fd (OstreeSysroot *self)
{
  g_return_val_if_fail (self->sysroot_fd != -1, -1);
  return self->sysroot_fd;
}

/**
 * ostree_sysroot_is_booted:
 * @self: Sysroot
 *
 * Can only be invoked after `ostree_sysroot_initialize()`.
 *
 * Returns: %TRUE iff the sysroot points to a booted deployment
 * Since: 2020.1
 */
gboolean
ostree_sysroot_is_booted (OstreeSysroot *self)
{
  g_return_val_if_fail (self->loadstate >= OSTREE_SYSROOT_LOAD_STATE_INIT, FALSE);
  return self->root_is_ostree_booted;
}

gboolean
_ostree_sysroot_bump_mtime (OstreeSysroot *self, GError **error)
{
  /* Allow other systems to monitor for changes */
  if (utimensat (self->sysroot_fd, "ostree/deploy", NULL, 0) < 0)
    {
      glnx_set_prefix_error_from_errno (error, "%s", "futimens");
      return FALSE;
    }
  return TRUE;
}

/**
 * ostree_sysroot_unload:
 * @self: Sysroot
 *
 * Release any resources such as file descriptors referring to the
 * root directory of this sysroot.  Normally, those resources are
 * cleared by finalization, but in garbage collected languages that
 * may not be predictable.
 *
 * This undoes the effect of `ostree_sysroot_load()`.
 */
void
ostree_sysroot_unload (OstreeSysroot *self)
{
  glnx_close_fd (&self->sysroot_fd);
  glnx_close_fd (&self->boot_fd);
}

/**
 * ostree_sysroot_ensure_initialized:
 * @self: Sysroot
 * @cancellable: Cancellable
 * @error: Error
 *
 * Ensure that @self is set up as a valid rootfs, by creating
 * /ostree/repo, among other things.
 */
gboolean
ostree_sysroot_ensure_initialized (OstreeSysroot *self, GCancellable *cancellable, GError **error)
{
  if (!ensure_sysroot_fd (self, error))
    return FALSE;

  if (!glnx_shutil_mkdir_p_at (self->sysroot_fd, "ostree/repo", 0755, cancellable, error))
    return FALSE;

  if (!glnx_shutil_mkdir_p_at (self->sysroot_fd, "ostree/deploy", 0755, cancellable, error))
    return FALSE;

  g_autoptr (OstreeRepo) repo = ostree_repo_create_at (
      self->sysroot_fd, "ostree/repo", OSTREE_REPO_MODE_BARE, NULL, cancellable, error);
  if (!repo)
    return FALSE;
  return TRUE;
}

void
_ostree_sysroot_emit_journal_msg (OstreeSysroot *self, const char *msg)
{
  g_signal_emit (self, signals[JOURNAL_MSG_SIGNAL], 0, msg);
}

gboolean
_ostree_sysroot_parse_deploy_path_name (const char *name, char **out_csum, int *out_serial,
                                        GError **error)
{

  static gsize regex_initialized;
  static GRegex *regex;
  if (g_once_init_enter (&regex_initialized))
    {
      regex = g_regex_new ("^([0-9a-f]+)\\.([0-9]+)$", 0, 0, NULL);
      g_assert (regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  g_autoptr (GMatchInfo) match = NULL;
  if (!g_regex_match (regex, name, 0, &match))
    return glnx_throw (error, "Invalid deploy name '%s', expected CHECKSUM.TREESERIAL", name);

  g_autofree char *serial_str = g_match_info_fetch (match, 2);
  *out_csum = g_match_info_fetch (match, 1);
  *out_serial = (int)g_ascii_strtoll (serial_str, NULL, 10);
  return TRUE;
}

/* For a given bootversion, get its subbootversion from `/ostree/boot.$bootversion`. */
gboolean
_ostree_sysroot_read_current_subbootversion (OstreeSysroot *self, int bootversion,
                                             int *out_subbootversion, GCancellable *cancellable,
                                             GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Reading current subbootversion", error);

  if (!ensure_sysroot_fd (self, error))
    return FALSE;

  g_autofree char *ostree_bootdir_name = g_strdup_printf ("ostree/boot.%d", bootversion);
  struct stat stbuf;
  if (!glnx_fstatat_allow_noent (self->sysroot_fd, ostree_bootdir_name, &stbuf, AT_SYMLINK_NOFOLLOW,
                                 error))
    return FALSE;
  if (errno == ENOENT)
    {
      g_debug ("Didn't find $sysroot/ostree/boot.%d symlink; assuming subbootversion 0",
               bootversion);
      *out_subbootversion = 0;
    }
  else
    {
      g_autofree char *current_subbootdir_name
          = glnx_readlinkat_malloc (self->sysroot_fd, ostree_bootdir_name, cancellable, error);
      if (!current_subbootdir_name)
        return glnx_prefix_error (error, "Reading %s", ostree_bootdir_name);

      if (g_str_has_suffix (current_subbootdir_name, ".0"))
        *out_subbootversion = 0;
      else if (g_str_has_suffix (current_subbootdir_name, ".1"))
        *out_subbootversion = 1;
      else
        return glnx_throw (error, "Invalid target '%s' in %s", current_subbootdir_name,
                           ostree_bootdir_name);
    }

  return TRUE;
}

static gint
compare_boot_loader_configs (OstreeBootconfigParser *a, OstreeBootconfigParser *b)
{
  const char *a_version = ostree_bootconfig_parser_get (a, "version");
  const char *b_version = ostree_bootconfig_parser_get (b, "version");

  if (a_version && b_version)
    {
      int r = strverscmp (a_version, b_version);
      /* Reverse */
      return -r;
    }
  else if (a_version)
    return -1;
  else
    return 1;
}

static int
compare_loader_configs_for_sorting (gconstpointer a_pp, gconstpointer b_pp)
{
  OstreeBootconfigParser *a = *((OstreeBootconfigParser **)a_pp);
  OstreeBootconfigParser *b = *((OstreeBootconfigParser **)b_pp);

  return compare_boot_loader_configs (a, b);
}

static gboolean read_current_bootversion (OstreeSysroot *self, int *out_bootversion,
                                          GCancellable *cancellable, GError **error);

/* Read all the bootconfigs from `/boot/loader/`. */
gboolean
_ostree_sysroot_read_boot_loader_configs (OstreeSysroot *self, int bootversion,
                                          GPtrArray **out_loader_configs, GCancellable *cancellable,
                                          GError **error)
{
  if (!ensure_sysroot_fd (self, error))
    return FALSE;

  g_autoptr (GPtrArray) ret_loader_configs
      = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  g_autofree char *entries_path = NULL;
  int current_version;
  if (!read_current_bootversion (self, &current_version, cancellable, error))
    return FALSE;

  if (current_version == bootversion)
    entries_path = g_strdup ("boot/loader/entries");
  else
    entries_path = g_strdup_printf ("boot/loader.%d/entries", bootversion);

  gboolean entries_exists;
  g_auto (GLnxDirFdIterator) dfd_iter = {
    0,
  };
  if (!ot_dfd_iter_init_allow_noent (self->sysroot_fd, entries_path, &dfd_iter, &entries_exists,
                                     error))
    return FALSE;
  if (!entries_exists)
    {
      /* Note early return */
      *out_loader_configs = g_steal_pointer (&ret_loader_configs);
      return TRUE;
    }

  while (TRUE)
    {
      struct dirent *dent;
      struct stat stbuf;

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (!glnx_fstatat (dfd_iter.fd, dent->d_name, &stbuf, 0, error))
        return FALSE;

      if (g_str_has_prefix (dent->d_name, "ostree-") && g_str_has_suffix (dent->d_name, ".conf")
          && S_ISREG (stbuf.st_mode))
        {
          g_autoptr (OstreeBootconfigParser) config = ostree_bootconfig_parser_new ();

          if (!ostree_bootconfig_parser_parse_at (config, dfd_iter.fd, dent->d_name, cancellable,
                                                  error))
            return glnx_prefix_error (error, "Parsing %s", dent->d_name);

          g_ptr_array_add (ret_loader_configs, g_object_ref (config));
        }
    }

  /* Callers expect us to give them a sorted array */
  g_ptr_array_sort (ret_loader_configs, compare_loader_configs_for_sorting);
  ot_transfer_out_value (out_loader_configs, &ret_loader_configs);
  return TRUE;
}

/* Get the bootversion from the `/boot/loader` directory or symlink. */
static gboolean
read_current_bootversion (OstreeSysroot *self, int *out_bootversion, GCancellable *cancellable,
                          GError **error)
{
  int ret_bootversion;
  struct stat stbuf;

  if (!glnx_fstatat_allow_noent (self->sysroot_fd, "boot/loader", &stbuf, AT_SYMLINK_NOFOLLOW,
                                 error))
    return FALSE;
  if (errno == ENOENT)
    {
      g_debug ("Didn't find $sysroot/boot/loader directory or symlink; assuming bootversion 0");
      ret_bootversion = 0;
    }
  else
    {
      if (S_ISLNK (stbuf.st_mode))
        {
          /* Traditional link, check version by reading link name */
          g_autofree char *target
              = glnx_readlinkat_malloc (self->sysroot_fd, "boot/loader", cancellable, error);
          if (!target)
            return FALSE;
          if (g_strcmp0 (target, "loader.0") == 0)
            ret_bootversion = 0;
          else if (g_strcmp0 (target, "loader.1") == 0)
            ret_bootversion = 1;
          else
            return glnx_throw (error, "Invalid target '%s' in boot/loader", target);
        }
      else
        {
          /* Loader is a directory, check version by reading ostree_bootversion */
          glnx_autofd int bversion_fd = -1;
          if (!ot_openat_ignore_enoent (self->sysroot_fd, BOOTVERSION_FILE, &bversion_fd, error))
            return FALSE;

          if (bversion_fd == -1)
            {
              g_debug ("File " BOOTVERSION_FILE " is not available, assuming bootversion 0");
              ret_bootversion = 0;
            }
          else
            {
              g_autofree char *version
                  = glnx_fd_readall_utf8 (bversion_fd, NULL, cancellable, error);
              if (g_strcmp0 (version, "loader.0") == 0)
                ret_bootversion = 0;
              else if (g_strcmp0 (version, "loader.1") == 0)
                ret_bootversion = 1;
              else
                return glnx_throw (error, "Invalid version '%s' in " BOOTVERSION_FILE, version);
            }
        }
    }

  *out_bootversion = ret_bootversion;
  return TRUE;
}

static gboolean
load_origin (OstreeSysroot *self, OstreeDeployment *deployment, GCancellable *cancellable,
             GError **error)
{
  g_autofree char *origin_path = ostree_deployment_get_origin_relpath (deployment);

  glnx_autofd int fd = -1;
  if (!ot_openat_ignore_enoent (self->sysroot_fd, origin_path, &fd, error))
    return FALSE;
  if (fd >= 0)
    {
      g_autofree char *origin_contents = glnx_fd_readall_utf8 (fd, NULL, cancellable, error);
      if (!origin_contents)
        return FALSE;

      g_autoptr (GKeyFile) origin = g_key_file_new ();
      if (!g_key_file_load_from_data (origin, origin_contents, -1, 0, error))
        return glnx_prefix_error (error, "Parsing %s", origin_path);

      ostree_deployment_set_origin (deployment, origin);
    }

  return TRUE;
}

// Parse the kernel argument ostree=
gboolean
_ostree_sysroot_parse_bootlink (const char *bootlink, int *out_entry_bootversion, char **out_osname,
                                char **out_bootcsum, int *out_treebootserial, GError **error)
{
  static gsize regex_initialized;
  static GRegex *regex;
  if (g_once_init_enter (&regex_initialized))
    {
      regex = g_regex_new ("^/ostree/boot.([01])/([^/]+)/([^/]+)/([0-9]+)$", 0, 0, NULL);
      g_assert (regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  g_autoptr (GMatchInfo) match = NULL;
  if (!g_regex_match (regex, bootlink, 0, &match))
    return glnx_throw (error,
                       "Invalid ostree= argument '%s', expected "
                       "ostree=/ostree/boot.BOOTVERSION/OSNAME/BOOTCSUM/TREESERIAL",
                       bootlink);

  g_autofree char *bootversion_str = g_match_info_fetch (match, 1);
  g_autofree char *treebootserial_str = g_match_info_fetch (match, 4);
  if (out_entry_bootversion)
    *out_entry_bootversion = (int)g_ascii_strtoll (bootversion_str, NULL, 10);
  if (out_osname)
    *out_osname = g_match_info_fetch (match, 2);
  if (out_bootcsum)
    *out_bootcsum = g_match_info_fetch (match, 3);
  if (out_treebootserial)
    *out_treebootserial = (int)g_ascii_strtoll (treebootserial_str, NULL, 10);
  return TRUE;
}

char *
_ostree_sysroot_get_runstate_path (OstreeDeployment *deployment, const char *key)
{
  return g_strdup_printf ("%s%s.%d/%s", _OSTREE_SYSROOT_DEPLOYMENT_RUNSTATE_DIR,
                          ostree_deployment_get_csum (deployment),
                          ostree_deployment_get_deployserial (deployment), key);
}

static gboolean
parse_deployment (OstreeSysroot *self, const char *boot_link, OstreeDeployment **out_deployment,
                  GCancellable *cancellable, GError **error)
{
  if (!ensure_sysroot_fd (self, error))
    return FALSE;

  int entry_boot_version;
  g_autofree char *osname = NULL;
  g_autofree char *bootcsum = NULL;
  int treebootserial = -1;

  // Note is_boot should always be false here, this boot_link is taken from BLS file, not
  // /proc/cmdline, BLS files are present in aboot images
  if (!_ostree_sysroot_parse_bootlink (boot_link, &entry_boot_version, &osname, &bootcsum,
                                       &treebootserial, error))
    return FALSE;

  g_autofree char *errprefix
      = g_strdup_printf ("Parsing deployment %s in stateroot '%s'", boot_link, osname);
  GLNX_AUTO_PREFIX_ERROR (errprefix, error);

  const char *relative_boot_link = boot_link;
  if (*relative_boot_link == '/')
    relative_boot_link++;

  g_autofree char *treebootserial_target
      = glnx_readlinkat_malloc (self->sysroot_fd, relative_boot_link, cancellable, error);
  if (!treebootserial_target)
    return FALSE;

  const char *deploy_basename = glnx_basename (treebootserial_target);
  g_autofree char *treecsum = NULL;
  int deployserial = -1;
  if (!_ostree_sysroot_parse_deploy_path_name (deploy_basename, &treecsum, &deployserial, error))
    return FALSE;

  glnx_autofd int deployment_dfd = -1;
  if (!glnx_opendirat (self->sysroot_fd, relative_boot_link, TRUE, &deployment_dfd, error))
    return FALSE;

  /* See if this is the booted deployment */
  const gboolean looking_for_booted_deployment
      = (self->root_is_ostree_booted && !self->booted_deployment);
  gboolean is_booted_deployment = FALSE;
  if (looking_for_booted_deployment)
    {
      struct stat stbuf;
      if (!glnx_fstat (deployment_dfd, &stbuf, error))
        return FALSE;

      /* ostree-prepare-root records the (device, inode) pair of the underlying real deployment
       * directory (before we might have mounted a composefs or overlayfs on top).
       *
       * Because this parser is operating outside the mounted namespace, we compare against
       * that backing directory.
       */
      g_assert (self->run_ostree_metadata);
      guint64 expected_root_dev = 0;
      guint64 expected_root_inode = 0;
      if (!g_variant_dict_lookup (self->run_ostree_metadata,
                                  OTCORE_RUN_BOOTED_KEY_BACKING_ROOTDEVINO, "(tt)",
                                  &expected_root_dev, &expected_root_inode))
        {
          g_debug ("Missing %s", OTCORE_RUN_BOOTED_KEY_BACKING_ROOTDEVINO);
          expected_root_dev = (guint64)self->root_device;
          expected_root_inode = (guint64)self->root_inode;
        }
      else
        g_debug ("Target rootdev key %s found", OTCORE_RUN_BOOTED_KEY_BACKING_ROOTDEVINO);

      /* A bit ugly, we're assigning to a sysroot-owned variable from deep in
       * this parsing code. But eh, if something fails the sysroot state can't
       * be relied on anyways.
       */
      is_booted_deployment
          = stbuf.st_dev == expected_root_dev && stbuf.st_ino == expected_root_inode;
    }

  g_autoptr (OstreeDeployment) ret_deployment
      = ostree_deployment_new (-1, osname, treecsum, deployserial, bootcsum, treebootserial);
  if (!load_origin (self, ret_deployment, cancellable, error))
    return FALSE;

  ret_deployment->unlocked = OSTREE_DEPLOYMENT_UNLOCKED_NONE;
  g_autofree char *unlocked_development_path = _ostree_sysroot_get_runstate_path (
      ret_deployment, _OSTREE_SYSROOT_DEPLOYMENT_RUNSTATE_FLAG_DEVELOPMENT);
  g_autofree char *unlocked_transient_path = _ostree_sysroot_get_runstate_path (
      ret_deployment, _OSTREE_SYSROOT_DEPLOYMENT_RUNSTATE_FLAG_TRANSIENT);
  struct stat stbuf;
  if (lstat (unlocked_development_path, &stbuf) == 0)
    ret_deployment->unlocked = OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT;
  else if (lstat (unlocked_transient_path, &stbuf) == 0)
    ret_deployment->unlocked = OSTREE_DEPLOYMENT_UNLOCKED_TRANSIENT;
  else
    {
      GKeyFile *origin = ostree_deployment_get_origin (ret_deployment);
      g_autofree char *existing_unlocked_state
          = origin ? g_key_file_get_string (origin, "origin", "unlocked", NULL) : NULL;

      if (g_strcmp0 (existing_unlocked_state, "hotfix") == 0)
        {
          ret_deployment->unlocked = OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX;
        }
      /* TODO: warn on unknown unlock types? */
    }

  g_debug ("Deployment %s.%d unlocked=%d", treecsum, deployserial, ret_deployment->unlocked);

  if (is_booted_deployment)
    self->booted_deployment = g_object_ref (ret_deployment);
  if (out_deployment)
    *out_deployment = g_steal_pointer (&ret_deployment);
  return TRUE;
}

/* Given a bootloader config, return the value part of the ostree= kernel
 * argument.
 */
static char *
get_ostree_kernel_arg_from_config (OstreeBootconfigParser *config)
{
  const char *options = ostree_bootconfig_parser_get (config, "options");
  if (!options)
    return NULL;

  g_auto (GStrv) opts = g_strsplit (options, " ", -1);
  for (char **iter = opts; *iter; iter++)
    {
      const char *opt = *iter;
      if (g_str_has_prefix (opt, "ostree="))
        return g_strdup (opt + strlen ("ostree="));
    }

  return NULL;
}

/* From a BLS config, use its ostree= karg to find the deployment it points to and add it to
 * the inout_deployments array. */
static gboolean
list_deployments_process_one_boot_entry (OstreeSysroot *self, OstreeBootconfigParser *config,
                                         GPtrArray *inout_deployments, GCancellable *cancellable,
                                         GError **error)
{
  g_autofree char *ostree_arg = get_ostree_kernel_arg_from_config (config);
  if (ostree_arg == NULL)
    return glnx_throw (error, "No ostree= kernel argument found");

  g_autoptr (OstreeDeployment) deployment = NULL;
  if (!parse_deployment (self, ostree_arg, &deployment, cancellable, error))
    return FALSE;

  ostree_deployment_set_bootconfig (deployment, config);
  char **overlay_initrds = ostree_bootconfig_parser_get_overlay_initrds (config);
  g_autoptr (GPtrArray) initrds_chksums = NULL;
  for (char **it = overlay_initrds; it && *it; it++)
    {
      const char *basename = glnx_basename (*it);
      if (strlen (basename) != (_OSTREE_SHA256_STRING_LEN + strlen (".img")))
        return glnx_throw (error, "Malformed overlay initrd filename: %s", basename);

      if (!initrds_chksums) /* lazy init */
        initrds_chksums = g_ptr_array_new_full (g_strv_length (overlay_initrds), g_free);
      g_ptr_array_add (initrds_chksums, g_strndup (basename, _OSTREE_SHA256_STRING_LEN));
    }

  if (initrds_chksums)
    {
      g_ptr_array_add (initrds_chksums, NULL);
      _ostree_deployment_set_overlay_initrds (deployment, (char **)initrds_chksums->pdata);
    }

  g_ptr_array_add (inout_deployments, g_object_ref (deployment));
  return TRUE;
}

static gint
compare_deployments_by_boot_loader_version_reversed (gconstpointer a_pp, gconstpointer b_pp)
{
  OstreeDeployment *a = *((OstreeDeployment **)a_pp);
  OstreeDeployment *b = *((OstreeDeployment **)b_pp);
  OstreeBootconfigParser *a_bootconfig = ostree_deployment_get_bootconfig (a);
  OstreeBootconfigParser *b_bootconfig = ostree_deployment_get_bootconfig (b);

  /* Staged deployments are always first */
  if (ostree_deployment_is_staged (a))
    {
      g_assert (!ostree_deployment_is_staged (b));
      return -1;
    }
  else if (ostree_deployment_is_staged (b))
    return 1;

  return compare_boot_loader_configs (a_bootconfig, b_bootconfig);
}

/**
 * ostree_sysroot_load:
 * @self: Sysroot
 * @cancellable: Cancellable
 * @error: Error
 *
 * Load deployment list, bootversion, and subbootversion from the
 * rootfs @self.
 */
gboolean
ostree_sysroot_load (OstreeSysroot *self, GCancellable *cancellable, GError **error)
{
  return ostree_sysroot_load_if_changed (self, NULL, cancellable, error);
}

static gboolean
ensure_repo (OstreeSysroot *self, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Opening sysroot repo", error);

  if (self->repo != NULL)
    return TRUE;
  if (!ensure_sysroot_fd (self, error))
    return FALSE;
  self->repo = ostree_repo_open_at (self->sysroot_fd, "ostree/repo", NULL, error);
  if (!self->repo)
    return FALSE;

  /* Flag it as having been created via ostree_sysroot_get_repo(), and hold a
   * weak ref for the remote-add handling.
   */
  g_weak_ref_init (&self->repo->sysroot, self);
  self->repo->sysroot_kind = OSTREE_REPO_SYSROOT_KIND_VIA_SYSROOT;

  /* Reload the repo config in case any defaults depend on knowing if this is
   * a system repo.
   */
  if (!ostree_repo_reload_config (self->repo, NULL, error))
    return FALSE;

  return TRUE;
}

/**
 * ostree_sysroot_initialize:
 * @self: sysroot
 *
 * Subset of ostree_sysroot_load(); performs basic initialization. Notably, one
 * can invoke `ostree_sysroot_get_fd()` after calling this function.
 *
 * It is not necessary to call this function if ostree_sysroot_load() is
 * invoked.
 *
 * Since: 2020.1
 */
gboolean
ostree_sysroot_initialize (OstreeSysroot *self, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Initializing sysroot", error);

  if (!ensure_sysroot_fd (self, error))
    return FALSE;

  if (self->loadstate < OSTREE_SYSROOT_LOAD_STATE_INIT)
    {
      /* Gather some global state; first if we have the global ostree-booted flag;
       * we'll use it to sanity check that we found a booted deployment for example.
       * Second, we also find out whether sysroot == /.
       */
      glnx_autofd int booted_state_fd = -1;
      if (!ot_openat_ignore_enoent (AT_FDCWD, OSTREE_PATH_BOOTED, &booted_state_fd, error))
        return FALSE;
      const gboolean ostree_booted = booted_state_fd != -1;

      if (booted_state_fd != -1)
        {
          g_autoptr (GVariant) ostree_run_metadata_v = NULL;
          if (!ot_variant_read_fd (booted_state_fd, 0, G_VARIANT_TYPE_VARDICT, TRUE,
                                   &ostree_run_metadata_v, error))
            return glnx_prefix_error (error, "failed to read %s", OTCORE_RUN_BOOTED);
          self->run_ostree_metadata = g_variant_dict_new (ostree_run_metadata_v);
        }

      // Gather the root device/inode
      {
        struct stat root_stbuf;
        if (!glnx_fstatat (AT_FDCWD, "/", &root_stbuf, 0, error))
          return FALSE;
        self->root_device = root_stbuf.st_dev;
        self->root_inode = root_stbuf.st_ino;
      }

      struct stat self_stbuf;
      if (!glnx_fstatat (AT_FDCWD, gs_file_get_path_cached (self->path), &self_stbuf, 0, error))
        return FALSE;

      const gboolean root_is_sysroot
          = (self->root_device == self_stbuf.st_dev && self->root_inode == self_stbuf.st_ino);

      self->root_is_ostree_booted = (ostree_booted && root_is_sysroot);
      g_debug ("root_is_ostree_booted: %d", self->root_is_ostree_booted);
      self->loadstate = OSTREE_SYSROOT_LOAD_STATE_INIT;
    }

  return TRUE;
}

/* Reload the staged deployment from the file in /run */
gboolean
_ostree_sysroot_reload_staged (OstreeSysroot *self, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Loading staged deployment", error);
  if (!self->root_is_ostree_booted)
    return TRUE; /* Note early return */

  g_assert (self->booted_deployment);

  g_clear_object (&self->staged_deployment);
  g_clear_pointer (&self->staged_deployment_data, g_variant_unref);

  /* Read the staged state from disk */
  glnx_autofd int fd = -1;
  if (!ot_openat_ignore_enoent (AT_FDCWD, _OSTREE_SYSROOT_RUNSTATE_STAGED, &fd, error))
    return FALSE;
  if (fd != -1)
    {
      g_autoptr (GBytes) contents = ot_fd_readall_or_mmap (fd, 0, error);
      if (!contents)
        return FALSE;
      g_autoptr (GVariant) staged_deployment_data
          = g_variant_new_from_bytes ((GVariantType *)"a{sv}", contents, TRUE);
      g_autoptr (GVariantDict) staged_deployment_dict = g_variant_dict_new (staged_deployment_data);

      /* Parse it */
      g_autoptr (GVariant) target = NULL;
      g_autofree char **kargs = NULL;
      g_autofree char **overlay_initrds = NULL;
      g_variant_dict_lookup (staged_deployment_dict, "target", "@a{sv}", &target);
      g_variant_dict_lookup (staged_deployment_dict, "kargs", "^a&s", &kargs);
      g_variant_dict_lookup (staged_deployment_dict, "overlay-initrds", "^a&s", &overlay_initrds);
      if (target)
        {
          g_autoptr (OstreeDeployment) staged
              = _ostree_sysroot_deserialize_deployment_from_variant (target, error);
          if (!staged)
            return FALSE;

          _ostree_deployment_set_bootconfig_from_kargs (staged, kargs);
          if (!load_origin (self, staged, NULL, error))
            return FALSE;

          _ostree_deployment_set_overlay_initrds (staged, overlay_initrds);

          self->staged_deployment = g_steal_pointer (&staged);
          self->staged_deployment_data = g_steal_pointer (&staged_deployment_data);
          /* We set this flag for ostree_deployment_is_staged() because that API
           * doesn't have access to the sysroot, which currently has the
           * canonical "staged_deployment" reference.
           */
          self->staged_deployment->staged = TRUE;
          (void)g_variant_dict_lookup (staged_deployment_dict, _OSTREE_SYSROOT_STAGED_KEY_LOCKED,
                                       "b", &self->staged_deployment->finalization_locked);
        }
    }

  return TRUE;
}

/* Loads the current bootversion, subbootversion, and deployments, starting from the
 * bootloader configs which are the source of truth.
 */
static gboolean
sysroot_load_from_bootloader_configs (OstreeSysroot *self, GCancellable *cancellable,
                                      GError **error)
{
  struct stat stbuf;

  int bootversion = 0;
  if (!read_current_bootversion (self, &bootversion, cancellable, error))
    return FALSE;

  int subbootversion = 0;
  if (!_ostree_sysroot_read_current_subbootversion (self, bootversion, &subbootversion, cancellable,
                                                    error))
    return FALSE;

  g_autoptr (GPtrArray) boot_loader_configs = NULL;
  if (!_ostree_sysroot_read_boot_loader_configs (self, bootversion, &boot_loader_configs,
                                                 cancellable, error))
    return FALSE;

  g_autoptr (GPtrArray) deployments
      = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  g_assert (boot_loader_configs); /* Pacify static analysis */
  for (guint i = 0; i < boot_loader_configs->len; i++)
    {
      OstreeBootconfigParser *config = boot_loader_configs->pdata[i];

      /* Note this also sets self->booted_deployment */
      if (!list_deployments_process_one_boot_entry (self, config, deployments, cancellable, error))
        {
          g_clear_object (&self->booted_deployment);
          return FALSE;
        }
    }

  if (self->root_is_ostree_booted && !self->booted_deployment)
    {
      if (!glnx_fstatat_allow_noent (self->sysroot_fd, "boot/loader", NULL, AT_SYMLINK_NOFOLLOW,
                                     error))
        return FALSE;
      if (errno == ENOENT)
        {
          return glnx_throw (error, "Unexpected state: %s found, but no /boot/loader directory",
                             OSTREE_PATH_BOOTED);
        }
      else
        {
          return glnx_throw (
              error, "Unexpected state: %s found and in / sysroot, but bootloader entry not found",
              OSTREE_PATH_BOOTED);
        }
    }

  if (!_ostree_sysroot_reload_staged (self, error))
    return FALSE;

  /* Ensure the entires are sorted */
  g_ptr_array_sort (deployments, compare_deployments_by_boot_loader_version_reversed);

  /* Staged shows up first */
  if (self->staged_deployment)
    g_ptr_array_insert (deployments, 0, g_object_ref (self->staged_deployment));

  /* And then set their index variables */
  for (guint i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      ostree_deployment_set_index (deployment, i);
    }

  /* Determine whether we're "physical" or not, the first time we load deployments */
  if (self->loadstate < OSTREE_SYSROOT_LOAD_STATE_LOADED)
    {
      /* If we have a booted deployment, the sysroot is / and we're definitely
       * not physical.
       */
      if (self->booted_deployment)
        self->is_physical = FALSE; /* (the default, but explicit for clarity) */
      /* Otherwise - check for /sysroot which should only exist in a deployment,
       * not in ${sysroot} (a metavariable for the real physical root).
       */
      else
        {
          if (!glnx_fstatat_allow_noent (self->sysroot_fd, "sysroot", &stbuf, 0, error))
            return FALSE;
          if (errno == ENOENT)
            self->is_physical = TRUE;
        }
      /* Otherwise, the default is FALSE */

      self->loadstate = OSTREE_SYSROOT_LOAD_STATE_LOADED;
    }

  self->bootversion = bootversion;
  self->subbootversion = subbootversion;
  self->deployments = g_steal_pointer (&deployments);

  return TRUE;
}

/**
 * ostree_sysroot_load_if_changed:
 * @self: #OstreeSysroot
 * @out_changed: (out caller-allocates):
 * @cancellable: Cancellable
 * @error: Error
 *
 * Since: 2016.4
 */
gboolean
ostree_sysroot_load_if_changed (OstreeSysroot *self, gboolean *out_changed,
                                GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("loading sysroot", error);

  if (!ostree_sysroot_initialize (self, error))
    return FALSE;

  /* Here we also lazily initialize the repository.  We didn't do this
   * previous to v2017.6, but we do now to support the error-free
   * ostree_sysroot_repo() API.
   */
  if (!ensure_repo (self, error))
    return FALSE;

  struct stat stbuf;
  if (!glnx_fstatat (self->sysroot_fd, "ostree/deploy", &stbuf, 0, error))
    return FALSE;

  if (self->has_loaded && self->loaded_ts.tv_sec == stbuf.st_mtim.tv_sec
      && self->loaded_ts.tv_nsec == stbuf.st_mtim.tv_nsec)
    {
      if (out_changed)
        *out_changed = FALSE;
      /* Note early return */
      return TRUE;
    }

  g_clear_pointer (&self->deployments, g_ptr_array_unref);
  g_clear_object (&self->booted_deployment);
  g_clear_object (&self->staged_deployment);
  self->bootversion = -1;
  self->subbootversion = -1;

  if (!sysroot_load_from_bootloader_configs (self, cancellable, error))
    return FALSE;

  self->loaded_ts = stbuf.st_mtim;
  self->has_loaded = TRUE;

  if (out_changed)
    *out_changed = TRUE;
  return TRUE;
}

int
ostree_sysroot_get_bootversion (OstreeSysroot *self)
{
  return self->bootversion;
}

int
ostree_sysroot_get_subbootversion (OstreeSysroot *self)
{
  return self->subbootversion;
}

/**
 * ostree_sysroot_get_booted_deployment:
 * @self: Sysroot
 *
 * This function may only be called if the sysroot is loaded.
 *
 * Returns: (transfer none) (nullable): The currently booted deployment, or %NULL if none
 */
OstreeDeployment *
ostree_sysroot_get_booted_deployment (OstreeSysroot *self)
{
  g_assert (self);
  g_assert (self->loadstate == OSTREE_SYSROOT_LOAD_STATE_LOADED);

  return self->booted_deployment;
}

/**
 * ostree_sysroot_require_booted_deployment:
 * @self: Sysroot
 *
 * Find the booted deployment, or return an error if not booted via OSTree.
 *
 * Returns: (transfer none) (not nullable): The currently booted deployment, or an error
 * Since: 2021.1
 */
OstreeDeployment *
ostree_sysroot_require_booted_deployment (OstreeSysroot *self, GError **error)
{
  g_assert (self->loadstate == OSTREE_SYSROOT_LOAD_STATE_LOADED);

  if (!self->booted_deployment)
    return glnx_null_throw (error, "Not currently booted into an OSTree system");
  return self->booted_deployment;
}

/**
 * ostree_sysroot_get_staged_deployment:
 * @self: Sysroot
 *
 * Returns: (transfer none) (nullable): The currently staged deployment, or %NULL if none
 *
 * Since: 2018.5
 */
OstreeDeployment *
ostree_sysroot_get_staged_deployment (OstreeSysroot *self)
{
  g_assert (self->loadstate == OSTREE_SYSROOT_LOAD_STATE_LOADED);

  return self->staged_deployment;
}

/**
 * ostree_sysroot_get_deployments:
 * @self: Sysroot
 *
 * Returns: (element-type OstreeDeployment) (transfer container): Ordered list of deployments
 */
GPtrArray *
ostree_sysroot_get_deployments (OstreeSysroot *self)
{
  g_assert (self->loadstate == OSTREE_SYSROOT_LOAD_STATE_LOADED);

  GPtrArray *copy = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  for (guint i = 0; i < self->deployments->len; i++)
    g_ptr_array_add (copy, g_object_ref (self->deployments->pdata[i]));
  return copy;
}

/**
 * ostree_sysroot_get_deployment_dirpath:
 * @self: Repo
 * @deployment: A deployment
 *
 * Note this function only returns a *relative* path - if you want
 * to access, it, you must either use fd-relative api such as openat(),
 * or concatenate it with the full ostree_sysroot_get_path().
 *
 * Returns: (transfer full) (not nullable): Path to deployment root directory, relative to sysroot
 */
char *
ostree_sysroot_get_deployment_dirpath (OstreeSysroot *self, OstreeDeployment *deployment)
{
  return g_strdup_printf (
      "ostree/deploy/%s/deploy/%s.%d", ostree_deployment_get_osname (deployment),
      ostree_deployment_get_csum (deployment), ostree_deployment_get_deployserial (deployment));
}

/**
 * ostree_sysroot_get_deployment_directory:
 * @self: Sysroot
 * @deployment: A deployment
 *
 * Returns: (transfer full): Path to deployment root directory
 */
GFile *
ostree_sysroot_get_deployment_directory (OstreeSysroot *self, OstreeDeployment *deployment)
{
  g_autofree char *dirpath = ostree_sysroot_get_deployment_dirpath (self, deployment);
  return g_file_resolve_relative_path (self->path, dirpath);
}

/**
 * ostree_sysroot_get_deployment_origin_path:
 * @deployment_path: A deployment path
 *
 * Returns: (transfer full): Path to deployment origin file
 */
GFile *
ostree_sysroot_get_deployment_origin_path (GFile *deployment_path)
{
  g_autoptr (GFile) deployment_parent = g_file_get_parent (deployment_path);
  return ot_gfile_resolve_path_printf (deployment_parent, "%s.origin",
                                       gs_file_get_path_cached (deployment_path));
}

/**
 * ostree_sysroot_get_repo:
 * @self: Sysroot
 * @out_repo: (out) (transfer full) (optional): Repository in sysroot @self
 * @cancellable: Cancellable
 * @error: Error
 *
 * Retrieve the OSTree repository in sysroot @self. The repo is guaranteed to be open
 * (see ostree_repo_open()).
 *
 * Returns: %TRUE on success, %FALSE otherwise
 */
gboolean
ostree_sysroot_get_repo (OstreeSysroot *self, OstreeRepo **out_repo, GCancellable *cancellable,
                         GError **error)
{
  if (!ensure_repo (self, error))
    return FALSE;
  if (out_repo != NULL)
    *out_repo = g_object_ref (self->repo);
  return TRUE;
}

/**
 * ostree_sysroot_repo:
 * @self: Sysroot
 *
 * This function is a variant of ostree_sysroot_get_repo() that cannot fail, and
 * returns a cached repository. Can only be called after ostree_sysroot_initialize()
 * or ostree_sysroot_load() has been invoked successfully.
 *
 * Returns: (transfer none) (not nullable): The OSTree repository in sysroot @self.
 *
 * Since: 2017.7
 */
OstreeRepo *
ostree_sysroot_repo (OstreeSysroot *self)
{
  g_assert (self);
  g_assert (self->loadstate >= OSTREE_SYSROOT_LOAD_STATE_LOADED);
  g_assert (self->repo);
  return self->repo;
}

static OstreeBootloader *
_ostree_sysroot_new_bootloader_by_type (OstreeSysroot *sysroot,
                                        OstreeCfgSysrootBootloaderOpt bl_type)
{
  switch (bl_type)
    {
    case CFG_SYSROOT_BOOTLOADER_OPT_NONE:
      /* No bootloader specified; do not query bootloaders to run. */
      return NULL;
    case CFG_SYSROOT_BOOTLOADER_OPT_GRUB2:
      return (OstreeBootloader *)_ostree_bootloader_grub2_new (sysroot);
    case CFG_SYSROOT_BOOTLOADER_OPT_SYSLINUX:
      return (OstreeBootloader *)_ostree_bootloader_syslinux_new (sysroot);
    case CFG_SYSROOT_BOOTLOADER_OPT_ABOOT:
      return (OstreeBootloader *)_ostree_bootloader_aboot_new (sysroot);
    case CFG_SYSROOT_BOOTLOADER_OPT_UBOOT:
      return (OstreeBootloader *)_ostree_bootloader_uboot_new (sysroot);
    case CFG_SYSROOT_BOOTLOADER_OPT_ZIPL:
      /* We never consider zipl as active by default, so it can only be created
       * if it's explicitly requested in the config */
      return (OstreeBootloader *)_ostree_bootloader_zipl_new (sysroot);
    case CFG_SYSROOT_BOOTLOADER_OPT_AUTO:
      /* "auto" is handled by ostree_sysroot_query_bootloader so we should
       * never get here: Fallthrough */
    default:
      g_assert_not_reached ();
    }
}

/**
 * ostree_sysroot_query_bootloader:
 * @sysroot: Sysroot
 * @out_bootloader: (out) (transfer full) (optional) (nullable): Return location for bootloader,
 * may be %NULL
 * @cancellable: Cancellable
 * @error: Error
 */
gboolean
_ostree_sysroot_query_bootloader (OstreeSysroot *sysroot, OstreeBootloader **out_bootloader,
                                  GCancellable *cancellable, GError **error)
{
  OstreeRepo *repo = ostree_sysroot_repo (sysroot);
  OstreeCfgSysrootBootloaderOpt bootloader_config = repo->bootloader;

  g_debug ("Using bootloader configuration: %s",
           CFG_SYSROOT_BOOTLOADER_OPTS_STR[bootloader_config]);

  g_autoptr (OstreeBootloader) ret_loader = NULL;
  if (bootloader_config == CFG_SYSROOT_BOOTLOADER_OPT_AUTO)
    {
      OstreeCfgSysrootBootloaderOpt probe[] = {
        CFG_SYSROOT_BOOTLOADER_OPT_SYSLINUX,
        CFG_SYSROOT_BOOTLOADER_OPT_GRUB2,
        CFG_SYSROOT_BOOTLOADER_OPT_UBOOT,
      };
      for (int i = 0; i < G_N_ELEMENTS (probe); i++)
        {
          g_autoptr (OstreeBootloader) bl
              = _ostree_sysroot_new_bootloader_by_type (sysroot, probe[i]);
          gboolean is_active = FALSE;
          if (!_ostree_bootloader_query (bl, &is_active, cancellable, error))
            return FALSE;
          if (is_active)
            {
              ret_loader = g_steal_pointer (&bl);
              break;
            }
        }
    }
  else
    ret_loader = _ostree_sysroot_new_bootloader_by_type (sysroot, bootloader_config);

  ot_transfer_out_value (out_bootloader, &ret_loader) return TRUE;
}

char *
_ostree_sysroot_join_lines (GPtrArray *lines)
{
  GString *buf = g_string_new ("");
  gboolean prev_was_empty = FALSE;

  for (guint i = 0; i < lines->len; i++)
    {
      const char *line = lines->pdata[i];
      /* Special bit to remove extraneous empty lines */
      if (*line == '\0')
        {
          if (prev_was_empty || i == 0)
            continue;
          else
            prev_was_empty = TRUE;
        }
      g_string_append (buf, line);
      g_string_append_c (buf, '\n');
    }
  return g_string_free (buf, FALSE);
}

/**
 * ostree_sysroot_query_deployments_for:
 * @self: Sysroot
 * @osname: (allow-none): "stateroot" name
 * @out_pending: (out) (nullable) (optional) (transfer full): The pending deployment
 * @out_rollback: (out) (nullable) (optional) (transfer full): The rollback deployment
 *
 * Find the pending and rollback deployments for @osname. Pass %NULL for @osname
 * to use the booted deployment's osname. By default, pending deployment is the
 * first deployment in the order that matches @osname, and @rollback will be the
 * next one after the booted deployment, or the deployment after the pending if
 * we're not looking at the booted deployment.
 *
 * Since: 2017.7
 */
void
ostree_sysroot_query_deployments_for (OstreeSysroot *self, const char *osname,
                                      OstreeDeployment **out_pending,
                                      OstreeDeployment **out_rollback)
{
  g_assert (osname != NULL || self->booted_deployment != NULL);
  g_autoptr (OstreeDeployment) ret_pending = NULL;
  g_autoptr (OstreeDeployment) ret_rollback = NULL;

  if (osname == NULL)
    osname = ostree_deployment_get_osname (self->booted_deployment);

  gboolean found_booted = FALSE;
  for (guint i = 0; i < self->deployments->len; i++)
    {
      OstreeDeployment *deployment = self->deployments->pdata[i];

      /* Ignore deployments not for this osname */
      if (strcmp (ostree_deployment_get_osname (deployment), osname) != 0)
        continue;

      /* Is this deployment booted?  If so, note we're past the booted */
      if (self->booted_deployment != NULL
          && ostree_deployment_equal (deployment, self->booted_deployment))
        {
          found_booted = TRUE;
          continue;
        }

      if (!found_booted && !ret_pending)
        ret_pending = g_object_ref (deployment);
      else if (found_booted && !ret_rollback)
        ret_rollback = g_object_ref (deployment);
    }
  if (out_pending)
    *out_pending = g_steal_pointer (&ret_pending);
  if (out_rollback)
    *out_rollback = g_steal_pointer (&ret_rollback);
}

/**
 * ostree_sysroot_get_merge_deployment:
 * @self: Sysroot
 * @osname: (allow-none): Operating system group
 *
 * Find the deployment to use as a configuration merge source; this is
 * the first one in the current deployment list which matches osname.
 *
 * Returns: (transfer full) (nullable): Configuration merge deployment
 */
OstreeDeployment *
ostree_sysroot_get_merge_deployment (OstreeSysroot *self, const char *osname)
{
  g_return_val_if_fail (osname != NULL || self->booted_deployment != NULL, NULL);

  if (osname == NULL)
    osname = ostree_deployment_get_osname (self->booted_deployment);

  /* If we're booted into the OS into which we're deploying, then
   * merge the currently *booted* configuration, rather than the most
   * recently deployed.
   */
  if (self->booted_deployment
      && g_strcmp0 (ostree_deployment_get_osname (self->booted_deployment), osname) == 0)
    return g_object_ref (self->booted_deployment);
  else
    {
      g_autoptr (OstreeDeployment) pending = NULL;
      ostree_sysroot_query_deployments_for (self, osname, &pending, NULL);
      return g_steal_pointer (&pending);
    }
}

/**
 * ostree_sysroot_origin_new_from_refspec:
 * @self: Sysroot
 * @refspec: A refspec
 *
 * Returns: (transfer full) (not nullable): A new config file which sets @refspec as an origin
 */
GKeyFile *
ostree_sysroot_origin_new_from_refspec (OstreeSysroot *self, const char *refspec)
{
  GKeyFile *ret = g_key_file_new ();
  g_key_file_set_string (ret, "origin", "refspec", refspec);
  return ret;
}

/**
 * ostree_sysroot_lock:
 * @self: Self
 * @error: Error
 *
 * Acquire an exclusive multi-process write lock for @self.  This call
 * blocks until the lock has been acquired.  The lock is not
 * reentrant.
 *
 * Release the lock with ostree_sysroot_unlock().  The lock will also
 * be released if @self is deallocated.
 */
gboolean
ostree_sysroot_lock (OstreeSysroot *self, GError **error)
{
  if (!ensure_sysroot_fd (self, error))
    return FALSE;

  if (!_ostree_sysroot_ensure_writable (self, error))
    return FALSE;

  return glnx_make_lock_file (self->sysroot_fd, OSTREE_SYSROOT_LOCKFILE, LOCK_EX, &self->lock,
                              error);
}

/**
 * ostree_sysroot_try_lock:
 * @self: Self
 * @out_acquired: (out): Whether or not the lock has been acquired
 * @error: Error
 *
 * Try to acquire an exclusive multi-process write lock for @self.  If
 * another process holds the lock, this function will return
 * immediately, setting @out_acquired to %FALSE, and returning %TRUE
 * (and no error).
 *
 * Release the lock with ostree_sysroot_unlock().  The lock will also
 * be released if @self is deallocated.
 */
gboolean
ostree_sysroot_try_lock (OstreeSysroot *self, gboolean *out_acquired, GError **error)
{
  if (!ensure_sysroot_fd (self, error))
    return FALSE;

  if (!_ostree_sysroot_ensure_writable (self, error))
    return FALSE;

  /* Note use of LOCK_NB */
  g_autoptr (GError) local_error = NULL;
  if (!glnx_make_lock_file (self->sysroot_fd, OSTREE_SYSROOT_LOCKFILE, LOCK_EX | LOCK_NB,
                            &self->lock, &local_error))
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
        {
          *out_acquired = FALSE;
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
    }
  else
    {
      *out_acquired = TRUE;
    }

  return TRUE;
}

/**
 * ostree_sysroot_unlock:
 * @self: Self
 *
 * Clear the lock previously acquired with ostree_sysroot_lock().  It
 * is safe to call this function if the lock has not been previously
 * acquired.
 */
void
ostree_sysroot_unlock (OstreeSysroot *self)
{
  glnx_release_lock_file (&self->lock);
}

static void
lock_in_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  GError *local_error = NULL;
  OstreeSysroot *self = source;

  if (!ostree_sysroot_lock (self, &local_error))
    goto out;

  if (g_cancellable_set_error_if_cancelled (cancellable, &local_error))
    ostree_sysroot_unlock (self);

out:
  if (local_error)
    g_task_return_error (task, local_error);
  else
    g_task_return_boolean (task, TRUE);
}

/**
 * ostree_sysroot_lock_async:
 * @self: Self
 * @cancellable: Cancellable
 * @callback: Callback
 * @user_data: User data
 *
 * An asynchronous version of ostree_sysroot_lock().
 */
void
ostree_sysroot_lock_async (OstreeSysroot *self, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);
  g_task_run_in_thread (task, lock_in_thread);
}

/**
 * ostree_sysroot_lock_finish:
 * @self: Self
 * @result: Result
 * @error: Error
 *
 * Call when ostree_sysroot_lock_async() is ready.
 */
gboolean
ostree_sysroot_lock_finish (OstreeSysroot *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  return g_task_propagate_boolean ((GTask *)result, error);
}

// This is a legacy subset of what happens normally via systemd tmpfiles.d;
// it is only run in the case that the deployment it self comes without
// usr/lib/tmpfiles.d
gboolean
_ostree_sysroot_stateroot_legacy_var_init (int dfd, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Legacy mode stateroot var initialization", error);

  /* This is a bit of a legacy hack...but we have to keep it around
   * now.  We're ensuring core subdirectories of /var exist.
   */
  if (!glnx_ensure_dir (dfd, "tmp", 0777, error))
    return FALSE;

  if (fchmodat (dfd, "tmp", 01777, 0) < 0)
    return glnx_throw_errno_prefix (error, "fchmod %s", "var/tmp");

  if (!glnx_ensure_dir (dfd, "lib", 0777, error))
    return FALSE;

  /* This needs to be available and properly labeled early during the boot
   * process (before tmpfiles.d kicks in), so that journald can flush logs from
   * the first boot there. https://bugzilla.redhat.com/show_bug.cgi?id=1265295
   * */
  if (!glnx_ensure_dir (dfd, "log", 0755, error))
    return FALSE;

  if (!glnx_fstatat_allow_noent (dfd, "run", NULL, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  if (errno == ENOENT && symlinkat ("../run", dfd, "run") < 0)
    return glnx_throw_errno_prefix (error, "Symlinking %s", "var/run");

  if (!glnx_fstatat_allow_noent (dfd, "lock", NULL, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  if (errno == ENOENT && symlinkat ("../run/lock", dfd, "lock") < 0)
    return glnx_throw_errno_prefix (error, "Symlinking %s", "var/lock");

  return TRUE;
}

/**
 * ostree_sysroot_init_osname:
 * @self: Sysroot
 * @osname: Name group of operating system checkouts
 * @cancellable: Cancellable
 * @error: Error
 *
 * Initialize the directory structure for an "osname", which is a
 * group of operating system deployments, with a shared `/var`.  One
 * is required for generating a deployment.
 *
 * Since: 2016.4
 */
gboolean
ostree_sysroot_init_osname (OstreeSysroot *self, const char *osname, GCancellable *cancellable,
                            GError **error)
{
  if (!_ostree_sysroot_ensure_writable (self, error))
    return FALSE;

  const char *deploydir = glnx_strjoina ("ostree/deploy/", osname);
  if (mkdirat (self->sysroot_fd, deploydir, 0777) < 0)
    return glnx_throw_errno_prefix (error, "Creating %s", deploydir);

  glnx_autofd int dfd = -1;
  if (!glnx_opendirat (self->sysroot_fd, deploydir, TRUE, &dfd, error))
    return FALSE;

  if (mkdirat (dfd, "var", 0777) < 0)
    return glnx_throw_errno_prefix (error, "Creating %s", "var");

  if (!_ostree_sysroot_bump_mtime (self, error))
    return FALSE;

  return TRUE;
}

/**
 * ostree_sysroot_simple_write_deployment:
 * @sysroot: Sysroot
 * @osname: (allow-none): OS name
 * @new_deployment: Prepend this deployment to the list
 * @merge_deployment: (allow-none): Use this deployment for configuration merge
 * @flags: Flags controlling behavior
 * @cancellable: Cancellable
 * @error: Error
 *
 * Prepend @new_deployment to the list of deployments, commit, and
 * cleanup.  By default, all other deployments for the given @osname
 * except the merge deployment and the booted deployment will be
 * garbage collected.
 *
 * If %OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN is
 * specified, then all current deployments will be kept.
 *
 * If %OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN_PENDING is
 * specified, then pending deployments will be kept.
 *
 * If %OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN_ROLLBACK is
 * specified, then rollback deployments will be kept.
 *
 * If %OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NOT_DEFAULT is
 * specified, then instead of prepending, the new deployment will be
 * added right after the booted or merge deployment, instead of first.
 *
 * If %OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NO_CLEAN is
 * specified, then no cleanup will be performed after adding the
 * deployment. Make sure to call ostree_sysroot_cleanup() sometime
 * later, instead.
 */
gboolean
ostree_sysroot_simple_write_deployment (OstreeSysroot *sysroot, const char *osname,
                                        OstreeDeployment *new_deployment,
                                        OstreeDeployment *merge_deployment,
                                        OstreeSysrootSimpleWriteDeploymentFlags flags,
                                        GCancellable *cancellable, GError **error)
{
  const gboolean postclean = (flags & OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NO_CLEAN) == 0;
  const gboolean make_default
      = !((flags & OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NOT_DEFAULT) > 0);
  const gboolean retain_pending
      = (flags & OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN_PENDING) > 0;
  const gboolean retain_rollback
      = (flags & OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN_ROLLBACK) > 0;
  gboolean retain = (flags & OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN) > 0;

  g_autoptr (GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  if (osname == NULL && booted_deployment)
    osname = ostree_deployment_get_osname (booted_deployment);

  gboolean added_new = FALSE;
  g_autoptr (GPtrArray) new_deployments = g_ptr_array_new_with_free_func (g_object_unref);
  if (make_default)
    {
      g_ptr_array_add (new_deployments, g_object_ref (new_deployment));
      added_new = TRUE;
    }

  /* without a booted and a merge deployment, retain_pending/rollback become meaningless;
   * let's just retain all deployments in that case */
  if (!booted_deployment && !merge_deployment && (retain_pending || retain_rollback))
    retain = TRUE;

  /* tracks when we come across the booted deployment */
  gboolean before_booted = TRUE;
  gboolean before_merge = TRUE;
  g_assert (deployments);
  for (guint i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      const gboolean osname_matches
          = (osname == NULL || g_str_equal (ostree_deployment_get_osname (deployment), osname));
      const gboolean is_booted = ostree_deployment_equal (deployment, booted_deployment);
      const gboolean is_merge = ostree_deployment_equal (deployment, merge_deployment);

      if (is_booted)
        before_booted = FALSE;
      if (is_merge)
        before_merge = FALSE;

      /* use the booted deployment as the "crossover" point between pending and rollback
       * deployments, fall back on merge deployment */
      const gboolean passed_crossover = booted_deployment ? !before_booted : !before_merge;

      /* Retain deployment if:
       *   - we're explicitly asked to, or
       *   - it's pinned
       *   - the deployment is for another osname, or
       *   - we're keeping pending deployments and this is a pending deployment, or
       *   - this is the merge or boot deployment, or
       *   - we're keeping rollback deployments and this is a rollback deployment
       */
      if (retain || ostree_deployment_is_pinned (deployment) || !osname_matches
          || (retain_pending && !passed_crossover) || (is_booted || is_merge)
          || (retain_rollback && passed_crossover))
        g_ptr_array_add (new_deployments, g_object_ref (deployment));

      /* add right after booted/merge deployment */
      if (!added_new && passed_crossover)
        {
          g_ptr_array_add (new_deployments, g_object_ref (new_deployment));
          added_new = TRUE;
        }
    }

  /* add it last if no crossover defined (or it's the first deployment in the sysroot) */
  if (!added_new)
    g_ptr_array_add (new_deployments, g_object_ref (new_deployment));

  OstreeSysrootWriteDeploymentsOpts write_opts = { .do_postclean = postclean };
  if (!ostree_sysroot_write_deployments_with_options (sysroot, new_deployments, &write_opts,
                                                      cancellable, error))
    return FALSE;

  return TRUE;
}

/* Return the sysroot-relative path to the "backing" directory of a deployment
 * which can hold additional data.
 */
char *
_ostree_sysroot_get_deployment_backing_relpath (OstreeDeployment *deployment)
{
  return g_strdup_printf (
      "ostree/deploy/%s/backing/%s.%d", ostree_deployment_get_osname (deployment),
      ostree_deployment_get_csum (deployment), ostree_deployment_get_deployserial (deployment));
}

/* Deploy a copy of @target_deployment */
static gboolean
clone_deployment (OstreeSysroot *sysroot, OstreeDeployment *target_deployment,
                  OstreeDeployment *merge_deployment, GCancellable *cancellable, GError **error)
{
  /* Ensure we have a clean slate */
  if (!ostree_sysroot_prepare_cleanup (sysroot, cancellable, error))
    return glnx_prefix_error (error, "Performing initial cleanup");

  /* Copy the bootloader config options */
  OstreeBootconfigParser *bootconfig = ostree_deployment_get_bootconfig (merge_deployment);
  g_auto (GStrv) previous_args
      = g_strsplit (ostree_bootconfig_parser_get (bootconfig, "options"), " ", -1);
  g_autoptr (OstreeKernelArgs) kargs = ostree_kernel_args_new ();
  ostree_kernel_args_append_argv (kargs, previous_args);

  /* Deploy the copy */
  g_autoptr (OstreeDeployment) new_deployment = NULL;
  g_auto (GStrv) kargs_strv = ostree_kernel_args_to_strv (kargs);
  if (!ostree_sysroot_deploy_tree (sysroot, ostree_deployment_get_osname (target_deployment),
                                   ostree_deployment_get_csum (target_deployment),
                                   ostree_deployment_get_origin (target_deployment),
                                   merge_deployment, kargs_strv, &new_deployment, cancellable,
                                   error))
    return FALSE;

  /* Hotfixes push the deployment as rollback target, so it shouldn't
   * be the default.
   */
  if (!ostree_sysroot_simple_write_deployment (
          sysroot, ostree_deployment_get_osname (target_deployment), new_deployment,
          merge_deployment, OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NOT_DEFAULT, cancellable,
          error))
    return FALSE;

  return TRUE;
}

/* Do `mkdir()` followed by `chmod()` immediately afterwards to ensure `umask()` isn't
 * masking permissions where we don't want it to. Thus we avoid calling `umask()`, which
 * would affect the whole process. */
static gboolean
mkdir_unmasked (int dfd, const char *path, int mode, GCancellable *cancellable, GError **error)
{
  if (!glnx_shutil_mkdir_p_at (dfd, path, mode, cancellable, error))
    return FALSE;
  if (fchmodat (dfd, path, mode, 0) < 0)
    return glnx_throw_errno_prefix (error, "chmod(%s)", path);
  return TRUE;
}

/**
 * ostree_sysroot_deployment_unlock:
 * @self: Sysroot
 * @deployment: Deployment
 * @unlocked_state: Transition to this unlocked state
 * @cancellable: Cancellable
 * @error: Error
 *
 * Configure the target deployment @deployment such that it
 * is writable.  There are multiple modes, essentially differing
 * in whether or not any changes persist across reboot.
 *
 * The `OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX` state is persistent
 * across reboots.
 *
 * Since: 2016.4
 */
gboolean
ostree_sysroot_deployment_unlock (OstreeSysroot *self, OstreeDeployment *deployment,
                                  OstreeDeploymentUnlockedState unlocked_state,
                                  GCancellable *cancellable, GError **error)
{
  /* This function cannot re-lock */
  g_return_val_if_fail (unlocked_state != OSTREE_DEPLOYMENT_UNLOCKED_NONE, FALSE);

  OstreeDeploymentUnlockedState current_unlocked = ostree_deployment_get_unlocked (deployment);
  if (current_unlocked != OSTREE_DEPLOYMENT_UNLOCKED_NONE)
    return glnx_throw (error, "Deployment is already in unlocked state: %s",
                       ostree_deployment_unlocked_state_to_string (current_unlocked));

  g_autoptr (OstreeDeployment) merge_deployment
      = ostree_sysroot_get_merge_deployment (self, ostree_deployment_get_osname (deployment));
  if (!merge_deployment)
    return glnx_throw (error, "No previous deployment to duplicate");

  /* For hotfixes, we push a rollback target */
  if (unlocked_state == OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX)
    {
      if (!clone_deployment (self, deployment, merge_deployment, cancellable, error))
        return FALSE;
    }

  /* Crack it open */
  if (!ostree_sysroot_deployment_set_mutable (self, deployment, TRUE, cancellable, error))
    return FALSE;

  g_autofree char *deployment_path = ostree_sysroot_get_deployment_dirpath (self, deployment);
  glnx_autofd int deployment_dfd = -1;
  if (!glnx_opendirat (self->sysroot_fd, deployment_path, TRUE, &deployment_dfd, error))
    return FALSE;

  g_autofree char *backing_relpath = _ostree_sysroot_get_deployment_backing_relpath (deployment);

  g_autoptr (OstreeSePolicy) sepolicy = ostree_sepolicy_new_at (deployment_dfd, cancellable, error);
  if (!sepolicy)
    return FALSE;

  /* we want our /usr overlay to have the same permission bits as the one we'll shadow */
  mode_t usr_mode;
  {
    struct stat stbuf;
    if (!glnx_fstatat (deployment_dfd, "usr", &stbuf, 0, error))
      return FALSE;
    usr_mode = stbuf.st_mode;
  }

  g_autofree char *ovl_options = NULL;
  static const char hotfix_ovl_options[]
      = "lowerdir=usr,upperdir=.usr-ovl-upper,workdir=.usr-ovl-work";

  switch (unlocked_state)
    {
    case OSTREE_DEPLOYMENT_UNLOCKED_NONE:
      g_assert_not_reached ();
      break;
    case OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX:
      {
        /* Create the overlayfs directories in the deployment root
         * directly for hotfixes.  The ostree-prepare-root.c helper
         * is also set up to detect and mount these.
         */
        if (!mkdir_unmasked (deployment_dfd, ".usr-ovl-upper", usr_mode, cancellable, error))
          return FALSE;
        if (!mkdir_unmasked (deployment_dfd, ".usr-ovl-work", usr_mode, cancellable, error))
          return FALSE;
        ovl_options = g_strdup (hotfix_ovl_options);
      }
      break;
    case OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT:
    case OSTREE_DEPLOYMENT_UNLOCKED_TRANSIENT:
      {
        // Holds the overlay backing data in the deployment backing dir, which
        // ensures that (unlike our previous usage of /var/tmp) that it's on the same
        // physical filesystem. It's valid to make /var/tmp a separate FS, but for
        // this data it needs to scale to the root.
        g_autofree char *usrovldir_relative
            = g_build_filename (backing_relpath, OSTREE_DEPLOYMENT_USR_TRANSIENT_DIR, NULL);
        // We explicitly don't want this data to persist, so if it happened
        // to leak from a previous boot, ensure the dir is cleaned now.
        if (!glnx_shutil_rm_rf_at (self->sysroot_fd, usrovldir_relative, cancellable, error))
          return FALSE;

        /* Ensure that the directory is created with the same label as `/usr` */
        {
          g_auto (OstreeSepolicyFsCreatecon) con = {
            0,
          };

          if (!_ostree_sepolicy_preparefscreatecon (&con, sepolicy, "/usr", usr_mode, error))
            return FALSE;

          // Create a new backing dir.
          if (!mkdir_unmasked (self->sysroot_fd, usrovldir_relative, usr_mode, cancellable, error))
            return FALSE;
        }

        // Open a fd for our new dir
        int ovldir_fd = -1;
        if (!glnx_opendirat (self->sysroot_fd, usrovldir_relative, FALSE, &ovldir_fd, error))
          return FALSE;

        // Create the work and upper dirs there
        if (!mkdir_unmasked (ovldir_fd, "upper", usr_mode, cancellable, error))
          return FALSE;
        if (!mkdir_unmasked (ovldir_fd, "work", usr_mode, cancellable, error))
          return FALSE;

        // TODO investigate depending on the new mount API with overlayfs
        ovl_options = g_strdup_printf ("lowerdir=usr,upperdir=/proc/self/fd/%d/upper"
                                       ",workdir=/proc/self/fd/%d/work",
                                       ovldir_fd, ovldir_fd);
      }
    }

  g_assert (ovl_options != NULL);

  /* Here we run `mount()` in a fork()ed child because we need to use
   * `chdir()` in order to have the mount path options to overlayfs not
   * look ugly.
   *
   * We can't `chdir()` inside a shared library since there may be
   * threads, etc.
   */
  {
    pid_t mount_child = fork ();
    if (mount_child < 0)
      return glnx_throw_errno_prefix (error, "fork");
    else if (mount_child == 0)
      {
        int mountflags = 0;
        if (unlocked_state == OSTREE_DEPLOYMENT_UNLOCKED_TRANSIENT)
          mountflags |= MS_RDONLY;
        /* Child process. Do NOT use any GLib API here; it's not generally fork() safe.
         *
         * TODO: report errors across a pipe (or use the journal?) rather than
         * spewing to stderr.
         */
        if (fchdir (deployment_dfd) < 0)
          err (1, "fchdir");
        if (mount ("overlay", "/usr", "overlay", mountflags, ovl_options) < 0)
          err (1, "mount");
        exit (EXIT_SUCCESS);
      }
    else
      {
        /* Parent */
        int estatus;

        if (TEMP_FAILURE_RETRY (waitpid (mount_child, &estatus, 0)) < 0)
          return glnx_throw_errno_prefix (error, "waitpid() on mount helper");
        if (!g_spawn_check_exit_status (estatus, error))
          return glnx_prefix_error (error, "Failed overlayfs mount");
      }
  }

  g_autoptr (OstreeDeployment) deployment_clone = ostree_deployment_clone (deployment);
  GKeyFile *origin_clone = ostree_deployment_get_origin (deployment_clone);

  /* Now, write out the flag saying what we did */
  switch (unlocked_state)
    {
    case OSTREE_DEPLOYMENT_UNLOCKED_NONE:
      g_assert_not_reached ();
      break;
    case OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX:
      g_key_file_set_string (origin_clone, "origin", "unlocked",
                             ostree_deployment_unlocked_state_to_string (unlocked_state));
      if (!ostree_sysroot_write_origin_file (self, deployment, origin_clone, cancellable, error))
        return FALSE;
      break;
    case OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT:
    case OSTREE_DEPLOYMENT_UNLOCKED_TRANSIENT:
      {
        g_autofree char *devpath
            = unlocked_state == OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT
                  ? _ostree_sysroot_get_runstate_path (
                        deployment, _OSTREE_SYSROOT_DEPLOYMENT_RUNSTATE_FLAG_DEVELOPMENT)
                  : _ostree_sysroot_get_runstate_path (
                        deployment, _OSTREE_SYSROOT_DEPLOYMENT_RUNSTATE_FLAG_TRANSIENT);
        g_autofree char *devpath_parent_owned = g_strdup (devpath);
        const char *devpath_parent = dirname (devpath_parent_owned);

        if (!glnx_shutil_mkdir_p_at (AT_FDCWD, devpath_parent, 0755, cancellable, error))
          return FALSE;

        if (!g_file_set_contents (devpath, "", -1, error))
          return FALSE;
      }
    }

  /* For hotfixes we already pushed a rollback which will bump the
   * mtime, but we need to bump it again so that clients get the state
   * change for this deployment.  For development we need to do this
   * regardless.
   */
  if (!_ostree_sysroot_bump_mtime (self, error))
    return FALSE;

  return TRUE;
}

/**
 * ostree_sysroot_deployment_set_pinned:
 * @self: Sysroot
 * @deployment: A deployment
 * @is_pinned: Whether or not deployment will be automatically GC'd
 * @error: Error
 *
 * By default, deployments may be subject to garbage collection. Typical uses of
 * libostree only retain at most 2 deployments. If @is_pinned is `TRUE`, a
 * metadata bit will be set causing libostree to avoid automatic GC of the
 * deployment. However, this is really an "advisory" note; it's still possible
 * for e.g. older versions of libostree unaware of pinning to GC the deployment.
 *
 * This function does nothing and returns successfully if the deployment
 * is already in the desired pinning state.  It is an error to try to pin
 * the staged deployment (as it's not in the bootloader entries).
 *
 * Since: 2018.3
 */
gboolean
ostree_sysroot_deployment_set_pinned (OstreeSysroot *self, OstreeDeployment *deployment,
                                      gboolean is_pinned, GError **error)
{
  const gboolean current_pin = ostree_deployment_is_pinned (deployment);
  if (is_pinned == current_pin)
    return TRUE;

  if (ostree_deployment_is_staged (deployment))
    return glnx_throw (error, "Cannot pin staged deployment");

  g_autoptr (OstreeDeployment) deployment_clone = ostree_deployment_clone (deployment);
  GKeyFile *origin_clone = ostree_deployment_get_origin (deployment_clone);

  if (is_pinned)
    g_key_file_set_boolean (origin_clone, OSTREE_ORIGIN_TRANSIENT_GROUP, "pinned", TRUE);
  else
    g_key_file_remove_key (origin_clone, OSTREE_ORIGIN_TRANSIENT_GROUP, "pinned", NULL);

  if (!ostree_sysroot_write_origin_file (self, deployment, origin_clone, NULL, error))
    return FALSE;

  return TRUE;
}
