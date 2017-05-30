/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

#include "otutil.h"
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-sepolicy-private.h"
#include "ostree-sysroot-private.h"
#include "ostree-deployment-private.h"
#include "ostree-bootloader-uboot.h"
#include "ostree-bootloader-syslinux.h"
#include "ostree-bootloader-grub2.h"

static gboolean
find_booted_deployment (OstreeSysroot       *self,
                        GPtrArray           *deployments,
                        OstreeDeployment   **out_deployment,
                        GCancellable        *cancellable,
                        GError             **error);

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
typedef struct {
  GObjectClass parent_class;
} OstreeSysrootClass;

enum {
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
  g_clear_pointer (&self->deployments, g_ptr_array_unref);
  g_clear_object (&self->booted_deployment);

  glnx_release_lock_file (&self->lock);

  ostree_sysroot_unload (self);

  G_OBJECT_CLASS (ostree_sysroot_parent_class)->finalize (object);
}

static void
ostree_sysroot_set_property(GObject         *object,
                            guint            prop_id,
                            const GValue    *value,
                            GParamSpec      *pspec)
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
ostree_sysroot_get_property(GObject         *object,
                            guint            prop_id,
                            GValue          *value,
                            GParamSpec      *pspec)
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
  g_autoptr(GFile) repo_path = NULL;

  /* Ensure the system root path is set. */
  if (self->path == NULL)
    self->path = g_object_ref (_ostree_get_default_sysroot_path ());

  repo_path = g_file_resolve_relative_path (self->path, "ostree/repo");
  self->repo = ostree_repo_new_for_sysroot_path (repo_path, self->path);
  self->repo->sysroot_kind = OSTREE_REPO_SYSROOT_KIND_VIA_SYSROOT;
  /* Hold a weak ref for the remote-add handling */
  g_weak_ref_init (&self->repo->sysroot, object);

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

  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_object ("path",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
ostree_sysroot_init (OstreeSysroot *self)
{
  const GDebugKey keys[] = {
    { "mutable-deployments", OSTREE_SYSROOT_DEBUG_MUTABLE_DEPLOYMENTS },
    { "no-xattrs", OSTREE_SYSROOT_DEBUG_NO_XATTRS },
  };

  self->debug_flags = g_parse_debug_string (g_getenv ("OSTREE_SYSROOT_DEBUG"),
                                            keys, G_N_ELEMENTS (keys));

  self->sysroot_fd = -1;
  self->lock = (GLnxLockFile)GLNX_LOCK_FILE_INIT;
}

/**
 * ostree_sysroot_new:
 * @path: (allow-none): Path to a system root directory, or %NULL
 *
 * Returns: (transfer full): An accessor object for an system root located at @path
 */
OstreeSysroot*
ostree_sysroot_new (GFile *path)
{
  return g_object_new (OSTREE_TYPE_SYSROOT, "path", path, NULL);
}

/**
 * ostree_sysroot_new_default:
 *
 * Returns: (transfer full): An accessor for the current visible root / filesystem
 */
OstreeSysroot*
ostree_sysroot_new_default (void)
{
  return ostree_sysroot_new (NULL);
}

/**
 * ostree_sysroot_get_path:
 * @self:
 *
 * Returns: (transfer none): Path to rootfs
 */
GFile *
ostree_sysroot_get_path (OstreeSysroot  *self)
{
  return self->path;
}

static gboolean
ensure_sysroot_fd (OstreeSysroot          *self,
                   GError                **error)
{
  if (self->sysroot_fd == -1)
    {
      if (!glnx_opendirat (AT_FDCWD, gs_file_get_path_cached (self->path), TRUE,
                           &self->sysroot_fd, error))
        return FALSE;
    }
  return TRUE;
}

/**
 * ostree_sysroot_get_fd:
 * @self: Sysroot
 *
 * Access a file descriptor that refers to the root directory of this
 * sysroot.  ostree_sysroot_load() must have been invoked prior to
 * calling this function.
 * 
 * Returns: A file descriptor valid for the lifetime of @self
 */
int
ostree_sysroot_get_fd (OstreeSysroot *self)
{
  g_return_val_if_fail (self->sysroot_fd != -1, -1);
  return self->sysroot_fd;
}

gboolean
_ostree_sysroot_bump_mtime (OstreeSysroot *self,
                            GError       **error)
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
ostree_sysroot_unload (OstreeSysroot  *self)
{
  if (self->sysroot_fd != -1)
    {
      (void) close (self->sysroot_fd);
      self->sysroot_fd = -1;
    }
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
ostree_sysroot_ensure_initialized (OstreeSysroot  *self,
                                   GCancellable   *cancellable,
                                   GError        **error)
{
  if (!ensure_sysroot_fd (self, error))
    return FALSE;

  if (!glnx_shutil_mkdir_p_at (self->sysroot_fd, "ostree/repo", 0755,
                               cancellable, error))
    return FALSE;

  if (!glnx_shutil_mkdir_p_at (self->sysroot_fd, "ostree/deploy", 0755,
                               cancellable, error))
    return FALSE;

  struct stat stbuf;
  if (fstatat (self->sysroot_fd, "ostree/repo/objects", &stbuf, 0) != 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "stat(ostree/repo/objects)");
      else
        {
          g_autoptr(GFile) repo_dir = g_file_resolve_relative_path (self->path, "ostree/repo");
          glnx_unref_object OstreeRepo *repo = ostree_repo_new (repo_dir);
          if (!ostree_repo_create (repo, OSTREE_REPO_MODE_BARE,
                                   cancellable, error))
            return FALSE;
        }
    }

  return TRUE;
}

gboolean
_ostree_sysroot_parse_deploy_path_name (const char *name,
                                        char      **out_csum,
                                        int        *out_serial,
                                        GError    **error)
{

  static gsize regex_initialized;
  static GRegex *regex;
  if (g_once_init_enter (&regex_initialized))
    {
      regex = g_regex_new ("^([0-9a-f]+)\\.([0-9]+)$", 0, 0, NULL);
      g_assert (regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  g_autoptr(GMatchInfo) match = NULL;
  if (!g_regex_match (regex, name, 0, &match))
    return glnx_throw (error, "Invalid deploy name '%s', expected CHECKSUM.TREESERIAL", name);

  g_autofree char *serial_str = g_match_info_fetch (match, 2);
  *out_csum = g_match_info_fetch (match, 1);
  *out_serial = (int)g_ascii_strtoll (serial_str, NULL, 10);
  return TRUE;
}

gboolean
_ostree_sysroot_read_current_subbootversion (OstreeSysroot *self,
                                             int            bootversion,
                                             int           *out_subbootversion,
                                             GCancellable  *cancellable,
                                             GError       **error)
{
  if (!ensure_sysroot_fd (self, error))
    return FALSE;

  g_autofree char *ostree_bootdir_name = g_strdup_printf ("ostree/boot.%d", bootversion);
  struct stat stbuf;
  if (fstatat (self->sysroot_fd, ostree_bootdir_name, &stbuf, AT_SYMLINK_NOFOLLOW) != 0)
    {
      if (errno == ENOENT)
        *out_subbootversion = 0;
      else
        return glnx_throw_errno (error);
    }
  else
    {
      g_autofree char *current_subbootdir_name =
        glnx_readlinkat_malloc (self->sysroot_fd, ostree_bootdir_name,
                                cancellable, error);
      if (!current_subbootdir_name)
        return FALSE;

      if (g_str_has_suffix (current_subbootdir_name, ".0"))
        *out_subbootversion = 0;
      else if (g_str_has_suffix (current_subbootdir_name, ".1"))
        *out_subbootversion = 1;
      else
        return glnx_throw (error, "Invalid target '%s' in %s",
                           current_subbootdir_name, ostree_bootdir_name);
    }

  return TRUE;
}

static gint
compare_boot_loader_configs (OstreeBootconfigParser     *a,
                             OstreeBootconfigParser     *b)
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
compare_loader_configs_for_sorting (gconstpointer  a_pp,
                                    gconstpointer  b_pp)
{
  OstreeBootconfigParser *a = *((OstreeBootconfigParser**)a_pp);
  OstreeBootconfigParser *b = *((OstreeBootconfigParser**)b_pp);

  return compare_boot_loader_configs (a, b);
}

gboolean
_ostree_sysroot_read_boot_loader_configs (OstreeSysroot *self,
                                          int            bootversion,
                                          GPtrArray    **out_loader_configs,
                                          GCancellable  *cancellable,
                                          GError       **error)
{
  if (!ensure_sysroot_fd (self, error))
    return FALSE;

  g_autoptr(GPtrArray) ret_loader_configs =
    g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  g_autofree char *entries_path = g_strdup_printf ("boot/loader.%d/entries", bootversion);
  gboolean entries_exists;
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
  if (!ot_dfd_iter_init_allow_noent (self->sysroot_fd, entries_path,
                                     &dfd_iter, &entries_exists, error))
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

      if (fstatat (dfd_iter.fd, dent->d_name, &stbuf, 0) != 0)
        return glnx_throw_errno (error);

      if (g_str_has_prefix (dent->d_name, "ostree-") &&
          g_str_has_suffix (dent->d_name, ".conf") &&
          S_ISREG (stbuf.st_mode))
        {
          glnx_unref_object OstreeBootconfigParser *config = ostree_bootconfig_parser_new ();

          if (!ostree_bootconfig_parser_parse_at (config, dfd_iter.fd, dent->d_name, cancellable, error))
            return glnx_prefix_error (error, "Parsing %s", dent->d_name);

          g_ptr_array_add (ret_loader_configs, g_object_ref (config));
        }
    }

  /* Callers expect us to give them a sorted array */
  g_ptr_array_sort (ret_loader_configs, compare_loader_configs_for_sorting);
  ot_transfer_out_value(out_loader_configs, &ret_loader_configs);
  return TRUE;
}

static gboolean
read_current_bootversion (OstreeSysroot *self,
                          int           *out_bootversion,
                          GCancellable  *cancellable,
                          GError       **error)
{
  int ret_bootversion;
  struct stat stbuf;

  if (fstatat (self->sysroot_fd, "boot/loader", &stbuf, AT_SYMLINK_NOFOLLOW) != 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno (error);
      ret_bootversion = 0;
    }
  else
    {
      if (!S_ISLNK (stbuf.st_mode))
        return glnx_throw (error, "Not a symbolic link: boot/loader");

      g_autofree char *target =
        glnx_readlinkat_malloc (self->sysroot_fd, "boot/loader", cancellable, error);
      if (!target)
        return FALSE;
      if (g_strcmp0 (target, "loader.0") == 0)
        ret_bootversion = 0;
      else if (g_strcmp0 (target, "loader.1") == 0)
        ret_bootversion = 1;
      else
        return glnx_throw (error, "Invalid target '%s' in boot/loader", target);
    }

  *out_bootversion = ret_bootversion;
  return TRUE;
}

static gboolean
parse_origin (OstreeSysroot   *self,
              int              deployment_dfd,
              const char      *deployment_name,
              GKeyFile       **out_origin,
              GCancellable    *cancellable,
              GError         **error)
{
  g_autofree char *origin_path = g_strconcat ("../", deployment_name, ".origin", NULL);
  g_autoptr(GKeyFile) ret_origin = g_key_file_new ();

  struct stat stbuf;
  if (fstatat (deployment_dfd, origin_path, &stbuf, 0) != 0)
    {
      if (errno != ENOENT)
        return glnx_throw_errno (error);
    }
  else
    {
      g_autofree char *origin_contents =
        glnx_file_get_contents_utf8_at (deployment_dfd, origin_path,
                                        NULL, cancellable, error);
      if (!origin_contents)
        return FALSE;

      if (!g_key_file_load_from_data (ret_origin, origin_contents, -1, 0, error))
        return glnx_prefix_error (error, "Parsing %s", origin_path);
    }

  ot_transfer_out_value(out_origin, &ret_origin);
  return TRUE;
}

static gboolean
parse_bootlink (const char    *bootlink,
                int           *out_entry_bootversion,
                char         **out_osname,
                char         **out_bootcsum,
                int           *out_treebootserial,
                GError       **error)
{
  static gsize regex_initialized;
  static GRegex *regex;
  if (g_once_init_enter (&regex_initialized))
    {
      regex = g_regex_new ("^/ostree/boot.([01])/([^/]+)/([^/]+)/([0-9]+)$", 0, 0, NULL);
      g_assert (regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  g_autoptr(GMatchInfo) match = NULL;
  if (!g_regex_match (regex, bootlink, 0, &match))
    return glnx_throw (error, "Invalid ostree= argument '%s', expected ostree=/ostree/boot.BOOTVERSION/OSNAME/BOOTCSUM/TREESERIAL", bootlink);

  g_autofree char *bootversion_str = g_match_info_fetch (match, 1);
  g_autofree char *treebootserial_str = g_match_info_fetch (match, 4);
  *out_entry_bootversion = (int)g_ascii_strtoll (bootversion_str, NULL, 10);
  *out_osname = g_match_info_fetch (match, 2);
  *out_bootcsum = g_match_info_fetch (match, 3);
  *out_treebootserial = (int)g_ascii_strtoll (treebootserial_str, NULL, 10);
  return TRUE;
}

static char *
get_unlocked_development_path (OstreeDeployment *deployment)
{
  return g_strdup_printf ("%s%s.%d/%s",
                          _OSTREE_SYSROOT_DEPLOYMENT_RUNSTATE_DIR,
                          ostree_deployment_get_csum (deployment),
                          ostree_deployment_get_deployserial (deployment),
                          _OSTREE_SYSROOT_DEPLOYMENT_RUNSTATE_FLAG_DEVELOPMENT);
}

static gboolean
parse_deployment (OstreeSysroot       *self,
                  const char          *boot_link,
                  OstreeDeployment   **out_deployment,
                  GCancellable        *cancellable,
                  GError             **error)
{
  if (!ensure_sysroot_fd (self, error))
    return FALSE;

  int entry_boot_version;
  g_autofree char *osname = NULL;
  g_autofree char *bootcsum = NULL;
  int treebootserial = -1;
  if (!parse_bootlink (boot_link, &entry_boot_version,
                       &osname, &bootcsum, &treebootserial,
                       error))
    return FALSE;

  const char *relative_boot_link = boot_link;
  if (*relative_boot_link == '/')
    relative_boot_link++;

  g_autofree char *treebootserial_target =
    glnx_readlinkat_malloc (self->sysroot_fd, relative_boot_link,
                            cancellable, error);
  if (!treebootserial_target)
    return FALSE;

  const char *deploy_basename = glnx_basename (treebootserial_target);
  g_autofree char *treecsum = NULL;
  int deployserial = -1;
  if (!_ostree_sysroot_parse_deploy_path_name (deploy_basename,
                                               &treecsum, &deployserial, error))
    return FALSE;

  glnx_fd_close int deployment_dfd = -1;
  if (!glnx_opendirat (self->sysroot_fd, relative_boot_link, TRUE,
                       &deployment_dfd, error))
    return FALSE;

  g_autoptr(GKeyFile) origin = NULL;
  if (!parse_origin (self, deployment_dfd, deploy_basename, &origin,
                     cancellable, error))
    return FALSE;

  glnx_unref_object OstreeDeployment *ret_deployment
    = ostree_deployment_new (-1, osname, treecsum, deployserial,
                             bootcsum, treebootserial);
  if (origin)
    ostree_deployment_set_origin (ret_deployment, origin);

  ret_deployment->unlocked = OSTREE_DEPLOYMENT_UNLOCKED_NONE;
  g_autofree char *unlocked_development_path = get_unlocked_development_path (ret_deployment);
  struct stat stbuf;
  if (lstat (unlocked_development_path, &stbuf) == 0)
    ret_deployment->unlocked = OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT;
  else
    {
      g_autofree char *existing_unlocked_state =
        g_key_file_get_string (origin, "origin", "unlocked", NULL);

      if (g_strcmp0 (existing_unlocked_state, "hotfix") == 0)
        {
          ret_deployment->unlocked = OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX;
        }
      /* TODO: warn on unknown unlock types? */
    }

  g_debug ("Deployment %s.%d unlocked=%d", treecsum, deployserial, ret_deployment->unlocked);

  if (out_deployment)
    *out_deployment = g_steal_pointer (&ret_deployment);
  return TRUE;
}

static char *
get_ostree_kernel_arg_from_config (OstreeBootconfigParser  *config)
{
  const char *options;
  char *ret = NULL;
  char **opts, **iter;

  options = ostree_bootconfig_parser_get (config, "options");
  if (!options)
    return NULL;

  opts = g_strsplit (options, " ", -1);
  for (iter = opts; *iter; iter++)
    {
      const char *opt = *iter;
      if (g_str_has_prefix (opt, "ostree="))
        {
          ret = g_strdup (opt + strlen ("ostree="));
          break;
        }
    }
  g_strfreev (opts);

  return ret;
}

static gboolean
list_deployments_process_one_boot_entry (OstreeSysroot               *self,
                                         OstreeBootconfigParser      *config,
                                         GPtrArray                   *inout_deployments,
                                         GCancellable                *cancellable,
                                         GError                     **error)
{
  g_autofree char *ostree_arg = get_ostree_kernel_arg_from_config (config);
  if (ostree_arg == NULL)
    return glnx_throw (error, "No ostree= kernel argument found");

  glnx_unref_object OstreeDeployment *deployment = NULL;
  if (!parse_deployment (self, ostree_arg, &deployment,
                         cancellable, error))
    return FALSE;

  ostree_deployment_set_bootconfig (deployment, config);

  g_ptr_array_add (inout_deployments, g_object_ref (deployment));
  return TRUE;
}

static gint
compare_deployments_by_boot_loader_version_reversed (gconstpointer     a_pp,
                                                     gconstpointer     b_pp)
{
  OstreeDeployment *a = *((OstreeDeployment**)a_pp);
  OstreeDeployment *b = *((OstreeDeployment**)b_pp);
  OstreeBootconfigParser *a_bootconfig = ostree_deployment_get_bootconfig (a);
  OstreeBootconfigParser *b_bootconfig = ostree_deployment_get_bootconfig (b);

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
ostree_sysroot_load (OstreeSysroot  *self,
                     GCancellable   *cancellable,
                     GError        **error)
{
  return ostree_sysroot_load_if_changed (self, NULL, cancellable, error);
}

static gboolean
ensure_repo_opened (OstreeSysroot  *self,
                    GError        **error)
{
  if (self->repo_opened)
    return TRUE;
  if (!ostree_repo_open (self->repo, NULL, error))
    return FALSE;
  self->repo_opened = TRUE;
  return TRUE;
}

gboolean
ostree_sysroot_load_if_changed (OstreeSysroot  *self,
                                gboolean       *out_changed,
                                GCancellable   *cancellable,
                                GError        **error)
{
  if (!ensure_sysroot_fd (self, error))
    return FALSE;

  /* Here we also lazily initialize the repository.  We didn't do this
   * previous to v2017.6, but we do now to support the error-free
   * ostree_sysroot_repo() API.
   */
  if (!ensure_repo_opened (self, error))
    return FALSE;

  int bootversion = 0;
  if (!read_current_bootversion (self, &bootversion, cancellable, error))
    return FALSE;

  int subbootversion = 0;
  if (!_ostree_sysroot_read_current_subbootversion (self, bootversion, &subbootversion,
                                                    cancellable, error))
    return FALSE;

  struct stat stbuf;
  if (fstatat (self->sysroot_fd, "ostree/deploy", &stbuf, 0) < 0)
    return glnx_throw_errno_prefix (error, "fstatat");

  if (out_changed)
    {
      if (self->loaded_ts.tv_sec == stbuf.st_mtim.tv_sec &&
          self->loaded_ts.tv_nsec == stbuf.st_mtim.tv_nsec)
        {
          *out_changed = FALSE;
          /* Note early return */
          return TRUE;
        }
    }

  g_clear_pointer (&self->deployments, g_ptr_array_unref);
  g_clear_object (&self->booted_deployment);
  self->bootversion = -1;
  self->subbootversion = -1;

  g_autoptr(GPtrArray) boot_loader_configs = NULL;
  if (!_ostree_sysroot_read_boot_loader_configs (self, bootversion, &boot_loader_configs,
                                                 cancellable, error))
    return FALSE;

  g_autoptr(GPtrArray) deployments = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  for (guint i = 0; i < boot_loader_configs->len; i++)
    {
      OstreeBootconfigParser *config = boot_loader_configs->pdata[i];

      if (!list_deployments_process_one_boot_entry (self, config, deployments,
                                                    cancellable, error))
        return FALSE;
    }

  g_ptr_array_sort (deployments, compare_deployments_by_boot_loader_version_reversed);
  for (guint i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      ostree_deployment_set_index (deployment, i);
    }

  if (!find_booted_deployment (self, deployments, &self->booted_deployment,
                               cancellable, error))
    return FALSE;

  /* Determine whether we're "physical" or not, the first time we initialize */
  if (!self->loaded)
    {
      /* If we have a booted deployment, the sysroot is / and we're definitely
       * not physical.
       */
      if (self->booted_deployment)
        self->is_physical = FALSE;  /* (the default, but explicit for clarity) */
      /* Otherwise - check for /sysroot which should only exist in a deployment,
       * not in ${sysroot} (a metavariable for the real physical root).
       */
      else if (fstatat (self->sysroot_fd, "sysroot", &stbuf, 0) < 0)
        {
          if (errno != ENOENT)
            return glnx_throw_errno_prefix (error, "fstatat");
          self->is_physical = TRUE;
        }
      /* Otherwise, the default is FALSE */
    }

  self->bootversion = bootversion;
  self->subbootversion = subbootversion;
  self->deployments = deployments;
  deployments = NULL; /* Transfer ownership */
  self->loaded = TRUE;
  self->loaded_ts = stbuf.st_mtim;

  if (out_changed)
    *out_changed = TRUE;
  return TRUE;
}

int
ostree_sysroot_get_bootversion (OstreeSysroot   *self)
{
  return self->bootversion;
}

int
ostree_sysroot_get_subbootversion (OstreeSysroot   *self)
{
  return self->subbootversion;
}

/**
 * ostree_sysroot_get_booted_deployment:
 * @self: Sysroot
 * 
 * Returns: (transfer none): The currently booted deployment, or %NULL if none
 */
OstreeDeployment *
ostree_sysroot_get_booted_deployment (OstreeSysroot       *self)
{
  g_return_val_if_fail (self->loaded, NULL);

  return self->booted_deployment;
}

/**
 * ostree_sysroot_get_deployments:
 * @self: Sysroot
 *
 * Returns: (element-type OstreeDeployment) (transfer container): Ordered list of deployments
 */
GPtrArray *
ostree_sysroot_get_deployments (OstreeSysroot  *self)
{
  GPtrArray *copy;
  guint i;

  g_return_val_if_fail (self->loaded, NULL);

  copy = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
  for (i = 0; i < self->deployments->len; i++)
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
 * Returns: (transfer full): Path to deployment root directory, relative to sysroot
 */
char *
ostree_sysroot_get_deployment_dirpath (OstreeSysroot    *self,
                                       OstreeDeployment *deployment)
{
  return g_strdup_printf ("ostree/deploy/%s/deploy/%s.%d",
                          ostree_deployment_get_osname (deployment),
                          ostree_deployment_get_csum (deployment),
                          ostree_deployment_get_deployserial (deployment));
}

/**
 * ostree_sysroot_get_deployment_directory:
 * @self: Sysroot
 * @deployment: A deployment
 *
 * Returns: (transfer full): Path to deployment root directory
 */
GFile *
ostree_sysroot_get_deployment_directory (OstreeSysroot    *self,
                                         OstreeDeployment *deployment)
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
ostree_sysroot_get_deployment_origin_path (GFile   *deployment_path)
{
  g_autoptr(GFile) deployment_parent = g_file_get_parent (deployment_path);
  return ot_gfile_resolve_path_printf (deployment_parent,
                                       "%s.origin",
                                       gs_file_get_path_cached (deployment_path));
}

/**
 * ostree_sysroot_get_repo:
 * @self: Sysroot
 * @out_repo: (out): Repository in sysroot @self
 * @cancellable: Cancellable
 * @error: Error
 *
 * Retrieve the OSTree repository in sysroot @self.
 */
gboolean
ostree_sysroot_get_repo (OstreeSysroot         *self,
                         OstreeRepo   **out_repo,
                         GCancellable  *cancellable,
                         GError       **error)
{
  if (!ensure_repo_opened (self, error))
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
 * returns a cached repository. Can only be called after ostree_sysroot_load()
 * has been invoked successfully.
 *
 * Returns: (transfer none): The OSTree repository in sysroot @self.
 */
OstreeRepo *
ostree_sysroot_repo (OstreeSysroot *self)
{
  g_return_val_if_fail (self->loaded, NULL);
  g_assert (self->repo);
  return self->repo;
}

/**
 * ostree_sysroot_query_bootloader:
 * @sysroot: Sysroot
 * @out_bootloader: (out) (transfer full) (allow-none): Return location for bootloader, may be %NULL
 * @cancellable: Cancellable
 * @error: Error
 */
gboolean
_ostree_sysroot_query_bootloader (OstreeSysroot     *sysroot,
                                  OstreeBootloader **out_bootloader,
                                  GCancellable      *cancellable,
                                  GError           **error)
{
  gboolean is_active;
  glnx_unref_object OstreeBootloader *ret_loader =
    (OstreeBootloader*)_ostree_bootloader_syslinux_new (sysroot);
  if (!_ostree_bootloader_query (ret_loader, &is_active,
                                 cancellable, error))
    return FALSE;

  if (!is_active)
    {
      g_object_unref (ret_loader);
      ret_loader = (OstreeBootloader*)_ostree_bootloader_grub2_new (sysroot);
      if (!_ostree_bootloader_query (ret_loader, &is_active,
                                     cancellable, error))
        return FALSE;
    }
  if (!is_active)
    {
      g_object_unref (ret_loader);
      ret_loader = (OstreeBootloader*)_ostree_bootloader_uboot_new (sysroot);
      if (!_ostree_bootloader_query (ret_loader, &is_active, cancellable, error))
        return FALSE;
    }
  if (!is_active)
    g_clear_object (&ret_loader);

  ot_transfer_out_value(out_bootloader, &ret_loader);
  return TRUE;
}

char *
_ostree_sysroot_join_lines (GPtrArray  *lines)
{
  GString *buf = g_string_new ("");
  guint i;
  gboolean prev_was_empty = FALSE;

  for (i = 0; i < lines->len; i++)
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

static gboolean
parse_kernel_commandline (OstreeKernelArgs  **out_args,
                          GCancellable       *cancellable,
                          GError            **error)
{
  g_autoptr(GFile) proc_cmdline = g_file_new_for_path ("/proc/cmdline");
  g_autofree char *contents = NULL;
  gsize len;

  if (!g_file_load_contents (proc_cmdline, cancellable, &contents, &len, NULL,
                             error))
    return FALSE;

  g_strchomp (contents);
  *out_args = _ostree_kernel_args_from_string (contents);
  return TRUE;
}

static gboolean
find_booted_deployment (OstreeSysroot       *self,
                        GPtrArray           *deployments,
                        OstreeDeployment   **out_deployment,
                        GCancellable        *cancellable,
                        GError             **error)
{
  struct stat root_stbuf;
  struct stat self_stbuf;
  glnx_unref_object OstreeDeployment *ret_deployment = NULL;

  if (stat ("/", &root_stbuf) != 0)
    return glnx_throw_errno_prefix (error, "stat /");

  if (!ensure_sysroot_fd (self, error))
    return FALSE;

  if (fstat (self->sysroot_fd, &self_stbuf) != 0)
    return glnx_throw_errno_prefix (error, "fstat");

  if (root_stbuf.st_dev == self_stbuf.st_dev &&
      root_stbuf.st_ino == self_stbuf.st_ino)
    {
      __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *kernel_args = NULL;
      if (!parse_kernel_commandline (&kernel_args, cancellable, error))
        return FALSE;

      const char *bootlink_arg = _ostree_kernel_args_get_last_value (kernel_args, "ostree");
      if (bootlink_arg)
        {
          for (guint i = 0; i < deployments->len; i++)
            {
              OstreeDeployment *deployment = deployments->pdata[i];
              g_autofree char *deployment_path = ostree_sysroot_get_deployment_dirpath (self, deployment);
              struct stat stbuf;

              if (fstatat (self->sysroot_fd, deployment_path, &stbuf, 0) != 0)
                return glnx_throw_errno_prefix (error, "fstatat");

              if (stbuf.st_dev == root_stbuf.st_dev &&
                  stbuf.st_ino == root_stbuf.st_ino)
                {
                  ret_deployment = g_object_ref (deployment);
                  break;
                }
            }

          if (ret_deployment == NULL)
            return glnx_throw (error, "Unexpected state: ostree= kernel argument found, but / is not a deployment root");
        }
      else
        {
          /* Not an ostree system */
        }
    }

  ot_transfer_out_value (out_deployment, &ret_deployment);
  return TRUE;
}

/**
 * ostree_sysroot_query_deployments_for:
 * @self: Sysroot
 * @osname: (allow-none): "stateroot" name
 * @out_pending: (out) (allow-none) (transfer full): The pending deployment
 * @out_rollback: (out) (allow-none) (transfer full): The rollback deployment
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
ostree_sysroot_query_deployments_for (OstreeSysroot     *self,
                                      const char        *osname,
                                      OstreeDeployment  **out_pending,
                                      OstreeDeployment  **out_rollback)
{
  g_return_if_fail (osname != NULL || self->booted_deployment != NULL);
  g_autoptr(OstreeDeployment) ret_pending = NULL;
  g_autoptr(OstreeDeployment) ret_rollback = NULL;

  if (osname == NULL)
    osname = ostree_deployment_get_osname (self->booted_deployment);

  gboolean found_booted = FALSE;
  for (guint i = 0; i < self->deployments->len; i++)
    {
      OstreeDeployment *deployment = self->deployments->pdata[i];

      /* Is this deployment booted?  If so, note we're past the booted */
      if (self->booted_deployment != NULL &&
          ostree_deployment_equal (deployment, self->booted_deployment))
        {
          found_booted = TRUE;
          continue;
        }

      /* Ignore deployments not for this osname */
      if (strcmp (ostree_deployment_get_osname (deployment), osname) != 0)
          continue;

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
 * Returns: (transfer full): Configuration merge deployment
 */
OstreeDeployment *
ostree_sysroot_get_merge_deployment (OstreeSysroot     *self,
                                     const char        *osname)
{
  g_return_val_if_fail (osname != NULL || self->booted_deployment != NULL, NULL);

  if (osname == NULL)
    osname = ostree_deployment_get_osname (self->booted_deployment);

  /* If we're booted into the OS into which we're deploying, then
   * merge the currently *booted* configuration, rather than the most
   * recently deployed.
   */
  if (self->booted_deployment &&
      g_strcmp0 (ostree_deployment_get_osname (self->booted_deployment), osname) == 0)
      return g_object_ref (self->booted_deployment);
  else
    {
      g_autoptr(OstreeDeployment) pending = NULL;
      ostree_sysroot_query_deployments_for (self, osname, &pending, NULL);
      return g_steal_pointer (&pending);
    }
}

/**
 * ostree_sysroot_origin_new_from_refspec:
 * @self: Sysroot
 * @refspec: A refspec
 *
 * Returns: (transfer full): A new config file which sets @refspec as an origin
 */
GKeyFile *
ostree_sysroot_origin_new_from_refspec (OstreeSysroot  *self,
                                        const char     *refspec)
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
ostree_sysroot_lock (OstreeSysroot     *self,
                     GError           **error)
{
  if (!ensure_sysroot_fd (self, error))
    return FALSE;
  return glnx_make_lock_file (self->sysroot_fd, OSTREE_SYSROOT_LOCKFILE,
                              LOCK_EX, &self->lock, error);
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
ostree_sysroot_try_lock (OstreeSysroot         *self,
                         gboolean              *out_acquired,
                         GError               **error)
{
  g_autoptr(GError) local_error = NULL;

  if (!ensure_sysroot_fd (self, error))
    return FALSE;

  /* Note use of LOCK_NB */
  if (!glnx_make_lock_file (self->sysroot_fd, OSTREE_SYSROOT_LOCKFILE,
                            LOCK_EX | LOCK_NB, &self->lock, &local_error))
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
ostree_sysroot_unlock (OstreeSysroot  *self)
{
  glnx_release_lock_file (&self->lock);
}

static void
lock_in_thread (GTask            *task,
                gpointer          source,
                gpointer          task_data,
                GCancellable     *cancellable)
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
ostree_sysroot_lock_async (OstreeSysroot         *self,
                           GCancellable          *cancellable,
                           GAsyncReadyCallback    callback,
                           gpointer               user_data)
{
  g_autoptr(GTask) task = g_task_new (self, cancellable, callback, user_data);
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
ostree_sysroot_lock_finish (OstreeSysroot         *self,
                            GAsyncResult          *result,
                            GError               **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  return g_task_propagate_boolean ((GTask*)result, error);
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
 */
gboolean
ostree_sysroot_init_osname (OstreeSysroot       *self,
                            const char          *osname,
                            GCancellable        *cancellable,
                            GError             **error)
{
  if (!ensure_sysroot_fd (self, error))
    return FALSE;

  const char *deploydir = glnx_strjoina ("ostree/deploy/", osname);
  if (mkdirat (self->sysroot_fd, deploydir, 0777) < 0)
    return glnx_throw_errno_prefix (error, "Creating %s", deploydir);

  glnx_fd_close int dfd = -1;
  if (!glnx_opendirat (self->sysroot_fd, deploydir, TRUE, &dfd, error))
    return FALSE;

  if (mkdirat (dfd, "var", 0777) < 0)
    return glnx_throw_errno_prefix (error, "Creating %s", "var");

  /* This is a bit of a legacy hack...but we have to keep it around
   * now.  We're ensuring core subdirectories of /var exist.
   */
  if (mkdirat (dfd, "var/tmp", 0777) < 0)
    return glnx_throw_errno_prefix (error, "Creating %s", "var/tmp");

  if (fchmodat (dfd, "var/tmp", 01777, 0) < 0)
    return glnx_throw_errno_prefix (error, "fchmod %s", "var/tmp");

  if (mkdirat (dfd, "var/lib", 0777) < 0)
    return glnx_throw_errno_prefix (error, "Creating %s", "var/tmp");

  /* This needs to be available and properly labeled early during the boot
   * process (before tmpfiles.d kicks in), so that journald can flush logs from
   * the first boot there. https://bugzilla.redhat.com/show_bug.cgi?id=1265295
   * */
  if (mkdirat (dfd, "var/log", 0755) < 0)
    return glnx_throw_errno_prefix (error, "Creating %s", "var/log");

  if (symlinkat ("../run", dfd, "var/run") < 0)
    return glnx_throw_errno_prefix (error, "Symlinking %s", "var/run");

  if (symlinkat ("../run/lock", dfd, "var/lock") < 0)
    return glnx_throw_errno_prefix (error, "Symlinking %s", "var/lock");

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
ostree_sysroot_simple_write_deployment (OstreeSysroot      *sysroot,
                                        const char         *osname,
                                        OstreeDeployment   *new_deployment,
                                        OstreeDeployment   *merge_deployment,
                                        OstreeSysrootSimpleWriteDeploymentFlags flags,
                                        GCancellable       *cancellable,
                                        GError            **error)
{
  gboolean ret = FALSE;
  guint i;
  OstreeDeployment *booted_deployment = NULL;
  g_autoptr(GPtrArray) deployments = NULL;
  g_autoptr(GPtrArray) new_deployments = g_ptr_array_new_with_free_func (g_object_unref);
  const gboolean postclean = (flags & OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NO_CLEAN) == 0;
  OstreeSysrootWriteDeploymentsOpts write_opts = { .do_postclean = postclean };
  gboolean retain = (flags & OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN) > 0;
  const gboolean make_default = !((flags & OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NOT_DEFAULT) > 0);
  gboolean added_new = FALSE;

  deployments = ostree_sysroot_get_deployments (sysroot);
  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  if (osname == NULL && booted_deployment)
    osname = ostree_deployment_get_osname (booted_deployment);

  if (make_default)
    {
      g_ptr_array_add (new_deployments, g_object_ref (new_deployment));
      added_new = TRUE;
    }

  for (i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      const gboolean is_merge_or_booted = 
        ostree_deployment_equal (deployment, booted_deployment) ||
        ostree_deployment_equal (deployment, merge_deployment);
      
      /* Keep deployments with different osnames, as well as the
       * booted and merge deployments
       */
      if (retain ||
          (osname != NULL && strcmp (ostree_deployment_get_osname (deployment), osname) != 0) ||
          is_merge_or_booted)
        {
          g_ptr_array_add (new_deployments, g_object_ref (deployment));
        }

      if (!added_new)
        {
          g_ptr_array_add (new_deployments, g_object_ref (new_deployment));
          added_new = TRUE;
        }
    }

  /* In this non-default case , an improvement in the future would be
   * to put the new deployment right after the current default in the
   * order.
   */
  if (!added_new)
    {
      g_ptr_array_add (new_deployments, g_object_ref (new_deployment));
      added_new = TRUE;
    }

  if (!ostree_sysroot_write_deployments_with_options (sysroot, new_deployments, &write_opts,
                                                      cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
clone_deployment (OstreeSysroot  *sysroot,
                  OstreeDeployment *target_deployment,
                  OstreeDeployment *merge_deployment,
                  GCancellable *cancellable,
                  GError **error)
{
  gboolean ret = FALSE;
  __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *kargs = NULL;
  glnx_unref_object OstreeDeployment *new_deployment = NULL;

  /* Ensure we have a clean slate */
  if (!ostree_sysroot_prepare_cleanup (sysroot, cancellable, error))
    {
      g_prefix_error (error, "Performing initial cleanup: ");
      goto out;
    }

  kargs = _ostree_kernel_args_new ();

  { OstreeBootconfigParser *bootconfig = ostree_deployment_get_bootconfig (merge_deployment);
    g_auto(GStrv) previous_args = g_strsplit (ostree_bootconfig_parser_get (bootconfig, "options"), " ", -1);
    
    _ostree_kernel_args_append_argv (kargs, previous_args);
  }

  {
    g_auto(GStrv) kargs_strv = _ostree_kernel_args_to_strv (kargs);

    if (!ostree_sysroot_deploy_tree (sysroot,
                                     ostree_deployment_get_osname (target_deployment),
                                     ostree_deployment_get_csum (target_deployment),
                                     ostree_deployment_get_origin (target_deployment),
                                     merge_deployment,
                                     kargs_strv,
                                     &new_deployment,
                                     cancellable, error))
      goto out;
  }

  /* Hotfixes push the deployment as rollback target, so it shouldn't
   * be the default.
   */
  if (!ostree_sysroot_simple_write_deployment (sysroot, ostree_deployment_get_osname (target_deployment),
                                               new_deployment, merge_deployment,
                                               OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NOT_DEFAULT,
                                               cancellable, error))
    goto out;
  
  ret = TRUE;
 out:
  return ret;
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
 */
gboolean
ostree_sysroot_deployment_unlock (OstreeSysroot     *self,
                                  OstreeDeployment  *deployment,
                                  OstreeDeploymentUnlockedState unlocked_state,
                                  GCancellable      *cancellable,
                                  GError           **error)
{
  gboolean ret = FALSE;
  glnx_unref_object OstreeSePolicy *sepolicy = NULL;
  OstreeDeploymentUnlockedState current_unlocked =
    ostree_deployment_get_unlocked (deployment); 
  glnx_unref_object OstreeDeployment *deployment_clone =
    ostree_deployment_clone (deployment);
  glnx_unref_object OstreeDeployment *merge_deployment = NULL;
  GKeyFile *origin_clone = ostree_deployment_get_origin (deployment_clone);
  const char hotfix_ovl_options[] = "lowerdir=usr,upperdir=.usr-ovl-upper,workdir=.usr-ovl-work";
  const char *ovl_options = NULL;
  g_autofree char *deployment_path = NULL;
  glnx_fd_close int deployment_dfd = -1;
  pid_t mount_child;

  /* This function cannot re-lock */
  g_return_val_if_fail (unlocked_state != OSTREE_DEPLOYMENT_UNLOCKED_NONE, FALSE);

  if (current_unlocked != OSTREE_DEPLOYMENT_UNLOCKED_NONE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Deployment is already in unlocked state: %s",
                   ostree_deployment_unlocked_state_to_string (current_unlocked));
      goto out;
    }

  merge_deployment = ostree_sysroot_get_merge_deployment (self, ostree_deployment_get_osname (deployment));
  if (!merge_deployment)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No previous deployment to duplicate");
      goto out;
    }

  /* For hotfixes, we push a rollback target */
  if (unlocked_state == OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX)
    {
      if (!clone_deployment (self, deployment, merge_deployment, cancellable, error))
        goto out;
    }

  /* Crack it open */
  if (!ostree_sysroot_deployment_set_mutable (self, deployment, TRUE,
                                              cancellable, error))
    goto out;

  deployment_path = ostree_sysroot_get_deployment_dirpath (self, deployment);

  if (!glnx_opendirat (self->sysroot_fd, deployment_path, TRUE, &deployment_dfd, error))
    goto out;

  sepolicy = ostree_sepolicy_new_at (deployment_dfd, cancellable, error);
  if (!sepolicy)
    goto out;

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
        if (!glnx_shutil_mkdir_p_at (deployment_dfd, ".usr-ovl-upper", 0755, cancellable, error))
          goto out;
        if (!glnx_shutil_mkdir_p_at (deployment_dfd, ".usr-ovl-work", 0755, cancellable, error))
          goto out;
        ovl_options = hotfix_ovl_options;
      }
      break;
    case OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT:
      {
        /* We're just doing transient development/hacking?  Okay,
         * stick the overlayfs bits in /var/tmp.
         */
        char *development_ovldir = strdupa ("/var/tmp/ostree-unlock-ovl.XXXXXX");
        const char *development_ovl_upper;
        const char *development_ovl_work;

        /* Ensure that the directory is created with the same label as `/usr` */
        { g_auto(OstreeSepolicyFsCreatecon) con = { 0, };

          if (!_ostree_sepolicy_preparefscreatecon (&con, sepolicy,
                                                    "/usr", 0755, error))
            goto out;

          if (!glnx_mkdtempat (AT_FDCWD, development_ovldir, 0755, error))
            goto out;
        }

        development_ovl_upper = glnx_strjoina (development_ovldir, "/upper");
        if (!glnx_shutil_mkdir_p_at (AT_FDCWD, development_ovl_upper, 0755, cancellable, error))
          goto out;
        development_ovl_work = glnx_strjoina (development_ovldir, "/work");
        if (!glnx_shutil_mkdir_p_at (AT_FDCWD, development_ovl_work, 0755, cancellable, error))
          goto out;
        ovl_options = glnx_strjoina ("lowerdir=usr,upperdir=", development_ovl_upper,
                                     ",workdir=", development_ovl_work);
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
    mount_child = fork ();
    if (mount_child < 0)
      {
        glnx_set_prefix_error_from_errno (error, "%s", "fork");
        goto out;
      }
    else if (mount_child == 0)
      {
        /* Child process.  Do NOT use any GLib API here. */
        if (fchdir (deployment_dfd) < 0)
          exit (EXIT_FAILURE);
        if (mount ("overlay", "/usr", "overlay", 0, ovl_options) < 0)
          exit (EXIT_FAILURE);
        exit (EXIT_SUCCESS);
      }
    else
      {
        /* Parent */
        int estatus;

        if (TEMP_FAILURE_RETRY (waitpid (mount_child, &estatus, 0)) < 0)
          {
            glnx_set_prefix_error_from_errno (error, "%s", "waitpid() on mount helper");
            goto out;
          }
        if (!g_spawn_check_exit_status (estatus, error))
          {
            g_prefix_error (error, "overlayfs mount helper: "); 
            goto out;
          }
      }
  }

  /* Now, write out the flag saying what we did */
  switch (unlocked_state)
    {
    case OSTREE_DEPLOYMENT_UNLOCKED_NONE:
      g_assert_not_reached ();
      break;
    case OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX:
      g_key_file_set_string (origin_clone, "origin", "unlocked",
                             ostree_deployment_unlocked_state_to_string (unlocked_state));
      if (!ostree_sysroot_write_origin_file (self, deployment, origin_clone,
                                             cancellable, error))
        goto out;
      break;
    case OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT:
      {
        g_autofree char *devpath = get_unlocked_development_path (deployment);
        g_autofree char *devpath_parent = dirname (g_strdup (devpath));

        if (!glnx_shutil_mkdir_p_at (AT_FDCWD, devpath_parent, 0755, cancellable, error))
          goto out;
        
        if (!g_file_set_contents (devpath, "", 0, error))
          goto out;
      }
    }

  /* For hotfixes we already pushed a rollback which will bump the
   * mtime, but we need to bump it again so that clients get the state
   * change for this deployment.  For development we need to do this
   * regardless.
   */
  if (!_ostree_sysroot_bump_mtime (self, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
