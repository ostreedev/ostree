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
#include "libgsystem.h"

#include "ostree-sysroot-private.h"
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
 * SECTION:libostree-sysroot
 * @title: Root partition mount point
 * @short_description: Manage physical root filesystem
 *
 * A #OstreeSysroot object represents a physical root filesystem,
 * which in particular should contain a toplevel /ostree directory.
 * Inside this directory is an #OstreeRepo in /ostree/repo, plus a set
 * of deployments in /ostree/deploy.
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
      /* Canonicalize */
      self->path = g_file_new_for_path (gs_file_get_path_cached (g_value_get_object (value)));
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

  g_assert (self->path != NULL);

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
  self->sysroot_fd = -1;
}

/**
 * ostree_sysroot_new:
 * @path: Path to a system root directory
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
  gs_unref_object GFile *rootfs = g_file_new_for_path ("/");
  return ostree_sysroot_new (rootfs);
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

gboolean
_ostree_sysroot_get_devino (GFile         *path,
                            guint32       *out_device,
                            guint64       *out_inode,
                            GCancellable  *cancellable,
                            GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileInfo *finfo = g_file_query_info (path, "unix::device,unix::inode",
                                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                        cancellable, error);

  if (!finfo)
    goto out;

  ret = TRUE;
  *out_device = g_file_info_get_attribute_uint32 (finfo, "unix::device");
  *out_inode = g_file_info_get_attribute_uint64 (finfo, "unix::inode");
 out:
  return ret;
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
  gboolean ret = FALSE;
  gs_unref_object GFile *dir = NULL;
  gs_unref_object GFile *ostree_dir = NULL;
  gs_unref_object GFile *repo_dir = NULL;

  ostree_dir = g_file_get_child (self->path, "ostree");
  repo_dir = g_file_get_child (ostree_dir, "repo");
  if (!gs_file_ensure_directory (repo_dir, TRUE, cancellable, error))
    goto out;

  g_clear_object (&dir);
  dir = g_file_get_child (ostree_dir, "deploy");
  if (!gs_file_ensure_directory (dir, TRUE, cancellable, error))
    goto out;

  g_clear_object (&dir);
  dir = ot_gfile_get_child_build_path (ostree_dir, "repo", "objects", NULL);
  if (!g_file_query_exists (dir, NULL))
    {
      gs_unref_object OstreeRepo *repo = ostree_repo_new (repo_dir);
      if (!ostree_repo_create (repo, OSTREE_REPO_MODE_BARE,
                               cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static void
match_info_cleanup (void *loc)
{
  GMatchInfo **match = (GMatchInfo**)loc;
  if (*match) g_match_info_unref (*match);
}

gboolean
_ostree_sysroot_parse_deploy_path_name (const char *name,
                                        char      **out_csum,
                                        int        *out_serial,
                                        GError    **error)
{
  gboolean ret = FALSE;
  __attribute__((cleanup(match_info_cleanup))) GMatchInfo *match = NULL;
  gs_free char *serial_str = NULL;

  static gsize regex_initialized;
  static GRegex *regex;

  if (g_once_init_enter (&regex_initialized))
    {
      regex = g_regex_new ("^([0-9a-f]+)\\.([0-9]+)$", 0, 0, NULL);
      g_assert (regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  if (!g_regex_match (regex, name, 0, &match))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid deploy name '%s', expected CHECKSUM.TREESERIAL", name);
      goto out;
    }

  *out_csum = g_match_info_fetch (match, 1);
  serial_str = g_match_info_fetch (match, 2);
  *out_serial = (int)g_ascii_strtoll (serial_str, NULL, 10);

  ret = TRUE;
 out:
  return ret;
}

gboolean
_ostree_sysroot_read_current_subbootversion (OstreeSysroot *self,
                                             int            bootversion,
                                             int           *out_subbootversion,
                                             GCancellable  *cancellable,
                                             GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *ostree_dir = g_file_get_child (self->path, "ostree");
  gs_free char *ostree_bootdir_name = g_strdup_printf ("boot.%d", bootversion);
  gs_unref_object GFile *ostree_bootdir = g_file_resolve_relative_path (ostree_dir, ostree_bootdir_name);
  gs_unref_object GFile *ostree_subbootdir = NULL;

  if (!ot_gfile_query_symlink_target_allow_noent (ostree_bootdir, &ostree_subbootdir,
                                                  cancellable, error))
    goto out;

  if (ostree_subbootdir == NULL)
    {
      *out_subbootversion = 0;
    }
  else
    {
      const char *current_subbootdir_name = gs_file_get_basename_cached (ostree_subbootdir);
      if (g_str_has_suffix (current_subbootdir_name, ".0"))
        *out_subbootversion = 0;
      else if (g_str_has_suffix (current_subbootdir_name, ".1"))
        *out_subbootversion = 1;
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid target '%s' in %s",
                       gs_file_get_path_cached (ostree_subbootdir),
                       gs_file_get_path_cached (ostree_bootdir));
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
_ostree_sysroot_read_boot_loader_configs (OstreeSysroot *self,
                                          int            bootversion,
                                          GPtrArray    **out_loader_configs,
                                          GCancellable  *cancellable,
                                          GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFile *loader_entries_dir = NULL;
  gs_unref_ptrarray GPtrArray *ret_loader_configs = NULL;
  GError *temp_error = NULL;

  loader_entries_dir = ot_gfile_resolve_path_printf (self->path, "boot/loader.%d/entries",
                                                     bootversion);
  ret_loader_configs = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  dir_enum = g_file_enumerate_children (loader_entries_dir, OSTREE_GIO_FAST_QUERYINFO,
                                        0, NULL, &temp_error);
  if (!dir_enum)
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
          goto done;
        } 
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *child;
      const char *name;

      if (!gs_file_enumerator_iterate (dir_enum, &file_info, &child,
                                       cancellable, error))
        goto out;
      if (file_info == NULL)
        break;

      name = g_file_info_get_name (file_info);

      if (g_str_has_prefix (name, "ostree-") &&
          g_str_has_suffix (name, ".conf") &&
          g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
        {
          gs_unref_object OstreeBootconfigParser *config = ostree_bootconfig_parser_new ();
  
          if (!ostree_bootconfig_parser_parse (config, child, cancellable, error))
            {
              g_prefix_error (error, "Parsing %s: ", gs_file_get_path_cached (child));
              goto out;
            }

          g_ptr_array_add (ret_loader_configs, g_object_ref (config));
        }
    }

 done:
  gs_transfer_out_value (out_loader_configs, &ret_loader_configs);
  ret = TRUE;
 out:
  return ret;
}


static gboolean
read_current_bootversion (OstreeSysroot *self,
                          int           *out_bootversion,
                          GCancellable  *cancellable,
                          GError       **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *boot_loader_path = g_file_resolve_relative_path (self->path, "boot/loader");
  gs_unref_object GFileInfo *info = NULL;
  const char *target;
  int ret_bootversion;

  if (!ot_gfile_query_info_allow_noent (boot_loader_path, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        &info,
                                        cancellable, error))
    goto out;

  if (info == NULL)
    ret_bootversion = 0;
  else
    {
      if (g_file_info_get_file_type (info) != G_FILE_TYPE_SYMBOLIC_LINK)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Not a symbolic link: %s", gs_file_get_path_cached (boot_loader_path));
          goto out;
        }

      target = g_file_info_get_symlink_target (info);
      if (g_strcmp0 (target, "loader.0") == 0)
        ret_bootversion = 0;
      else if (g_strcmp0 (target, "loader.1") == 0)
        ret_bootversion = 1;
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid target '%s' in %s", target, gs_file_get_path_cached (boot_loader_path));
          goto out;
        }
    }

  ret = TRUE;
  *out_bootversion = ret_bootversion;
 out:
  return ret;
}

static gboolean
parse_origin (OstreeSysroot   *self,
              GFile           *deployment_path,
              GKeyFile       **out_origin,
              GCancellable    *cancellable,
              GError         **error)
{
  gboolean ret = FALSE;
  GKeyFile *ret_origin = NULL;
  gs_unref_object GFile *origin_path = ostree_sysroot_get_deployment_origin_path (deployment_path);
  gs_free char *origin_contents = NULL;
  
  if (!ot_gfile_load_contents_utf8_allow_noent (origin_path, &origin_contents,
                                                cancellable, error))
    goto out;

  if (origin_contents)
    {
      ret_origin = g_key_file_new ();
      if (!g_key_file_load_from_data (ret_origin, origin_contents, -1, 0, error))
        goto out;
    }

  ret = TRUE;
  gs_transfer_out_value (out_origin, &ret_origin);
 out:
  if (error)
    g_prefix_error (error, "Parsing %s: ", gs_file_get_path_cached (origin_path));
  if (ret_origin)
    g_key_file_unref (ret_origin);
  return ret;
}

static gboolean
parse_bootlink (const char    *bootlink,
                int           *out_entry_bootversion,
                char         **out_osname,
                char         **out_bootcsum,
                int           *out_treebootserial,
                GError       **error)
{
  gboolean ret = FALSE;
  __attribute__((cleanup(match_info_cleanup))) GMatchInfo *match = NULL;
  gs_free char *bootversion_str = NULL;
  gs_free char *treebootserial_str = NULL;

  static gsize regex_initialized;
  static GRegex *regex;

  if (g_once_init_enter (&regex_initialized))
    {
      regex = g_regex_new ("^/ostree/boot.([01])/([^/]+)/([^/]+)/([0-9]+)$", 0, 0, NULL);
      g_assert (regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  if (!g_regex_match (regex, bootlink, 0, &match))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid ostree= argument '%s', expected ostree=/ostree/boot.BOOTVERSION/OSNAME/BOOTCSUM/TREESERIAL", bootlink);
      goto out;
    }
    
  bootversion_str = g_match_info_fetch (match, 1);
  *out_entry_bootversion = (int)g_ascii_strtoll (bootversion_str, NULL, 10);
  *out_osname = g_match_info_fetch (match, 2);
  *out_bootcsum = g_match_info_fetch (match, 3);
  treebootserial_str = g_match_info_fetch (match, 4);
  *out_treebootserial = (int)g_ascii_strtoll (treebootserial_str, NULL, 10);
  
  ret = TRUE;
 out:
  return ret;
}

static gboolean
parse_deployment (OstreeSysroot       *self,
                  const char          *boot_link,
                  OstreeDeployment   **out_deployment,
                  GCancellable        *cancellable,
                  GError             **error)
{
  gboolean ret = FALSE;
  const char *relative_boot_link;
  gs_unref_object OstreeDeployment *ret_deployment = NULL;
  int entry_boot_version;
  int treebootserial = -1;
  int deployserial = -1;
  gs_free char *osname = NULL;
  gs_free char *bootcsum = NULL;
  gs_free char *treecsum = NULL;
  gs_unref_object GFile *treebootserial_link = NULL;
  gs_unref_object GFileInfo *treebootserial_info = NULL;
  gs_unref_object GFile *treebootserial_target = NULL;
  GKeyFile *origin = NULL;
      
  if (!parse_bootlink (boot_link, &entry_boot_version,
                       &osname, &bootcsum, &treebootserial,
                       error))
    goto out;

  relative_boot_link = boot_link;
  if (*relative_boot_link == '/')
    relative_boot_link++;
  treebootserial_link = g_file_resolve_relative_path (self->path, relative_boot_link);
  treebootserial_info = g_file_query_info (treebootserial_link, OSTREE_GIO_FAST_QUERYINFO,
                                           G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                           cancellable, error);
  if (!treebootserial_info)
    goto out;

  if (!ot_gfile_get_symlink_target_from_info (treebootserial_link, treebootserial_info,
                                              &treebootserial_target, cancellable, error))
    goto out;

  if (!_ostree_sysroot_parse_deploy_path_name (gs_file_get_basename_cached (treebootserial_target),
                                               &treecsum, &deployserial, error))
    goto out;

  if (!parse_origin (self, treebootserial_target, &origin,
                     cancellable, error))
    goto out;

  ret_deployment = ostree_deployment_new (-1, osname, treecsum, deployserial,
                                      bootcsum, treebootserial);
  if (origin)
    ostree_deployment_set_origin (ret_deployment, origin);

  ret = TRUE;
  gs_transfer_out_value (out_deployment, &ret_deployment);
 out:
  if (origin)
    g_key_file_unref (origin);
  return ret;
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
  gboolean ret = FALSE;
  gs_free char *ostree_arg = NULL;
  gs_unref_object OstreeDeployment *deployment = NULL;

  ostree_arg = get_ostree_kernel_arg_from_config (config);
  if (ostree_arg == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No ostree= kernel argument found");
      goto out;
    }
  
  if (!parse_deployment (self, ostree_arg, &deployment,
                         cancellable, error))
    goto out;
  
  ostree_deployment_set_bootconfig (deployment, config);

  g_ptr_array_add (inout_deployments, g_object_ref (deployment));
  
  ret = TRUE;
 out:
  return ret;
}

static gint
compare_deployments_by_boot_loader_version_reversed (gconstpointer     a_pp,
                                                     gconstpointer     b_pp)
{
  OstreeDeployment *a = *((OstreeDeployment**)a_pp);
  OstreeDeployment *b = *((OstreeDeployment**)b_pp);
  OstreeBootconfigParser *a_bootconfig = ostree_deployment_get_bootconfig (a);
  OstreeBootconfigParser *b_bootconfig = ostree_deployment_get_bootconfig (b);
  const char *a_version = ostree_bootconfig_parser_get (a_bootconfig, "version");
  const char *b_version = ostree_bootconfig_parser_get (b_bootconfig, "version");
  
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
  gboolean ret = FALSE;
  guint i;
  int bootversion = 0;
  int subbootversion = 0;
  gs_unref_ptrarray GPtrArray *boot_loader_configs = NULL;
  gs_unref_ptrarray GPtrArray *deployments = NULL;

  g_clear_pointer (&self->deployments, g_ptr_array_unref);
  g_clear_pointer (&self->booted_deployment, g_object_unref);
  self->bootversion = -1;
  self->subbootversion = -1;

  if (!read_current_bootversion (self, &bootversion, cancellable, error))
    goto out;

  if (!_ostree_sysroot_read_current_subbootversion (self, bootversion, &subbootversion,
                                                    cancellable, error))
    goto out;

  if (!_ostree_sysroot_read_boot_loader_configs (self, bootversion, &boot_loader_configs,
                                                 cancellable, error))
    goto out;

  deployments = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);

  for (i = 0; i < boot_loader_configs->len; i++)
    {
      OstreeBootconfigParser *config = boot_loader_configs->pdata[i];

      if (!list_deployments_process_one_boot_entry (self, config, deployments,
                                                    cancellable, error))
        goto out;
    }

  g_ptr_array_sort (deployments, compare_deployments_by_boot_loader_version_reversed);
  for (i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      ostree_deployment_set_index (deployment, i);
    }

  if (!find_booted_deployment (self, deployments, &self->booted_deployment,
                               cancellable, error))
    goto out;

  self->bootversion = bootversion;
  self->subbootversion = subbootversion;
  self->deployments = deployments;
  deployments = NULL; /* Transfer ownership */
  self->loaded = TRUE;

  ret = TRUE;
 out:
  return ret;
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
  gs_free char *path = g_strdup_printf ("ostree/deploy/%s/deploy/%s.%d",
                                        ostree_deployment_get_osname (deployment),
                                        ostree_deployment_get_csum (deployment),
                                        ostree_deployment_get_deployserial (deployment));
  return g_file_resolve_relative_path (self->path, path);
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
  gs_unref_object GFile *deployment_parent = g_file_get_parent (deployment_path);
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
  gboolean ret = FALSE;
  gs_unref_object OstreeRepo *ret_repo = NULL;
  gs_unref_object GFile *repo_path = g_file_resolve_relative_path (self->path, "ostree/repo");

  ret_repo = ostree_repo_new (repo_path);
  if (!ostree_repo_open (ret_repo, cancellable, error))
    goto out;
    
  ret = TRUE;
  ot_transfer_out_value (out_repo, &ret_repo);
 out:
  return ret;
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
  gboolean ret = FALSE;
  gboolean is_active;
  gs_unref_object OstreeBootloader *ret_loader = NULL;

  ret_loader = (OstreeBootloader*)_ostree_bootloader_syslinux_new (sysroot);
  if (!_ostree_bootloader_query (ret_loader, &is_active,
                                 cancellable, error))
    goto out;
  if (!is_active)
    {
      g_object_unref (ret_loader);
      ret_loader = (OstreeBootloader*)_ostree_bootloader_grub2_new (sysroot);
      if (!_ostree_bootloader_query (ret_loader, &is_active,
                                     cancellable, error))
        goto out;
    }
  if (!is_active)
    {
      g_object_unref (ret_loader);
      ret_loader = (OstreeBootloader*)_ostree_bootloader_uboot_new (sysroot);
      if (!_ostree_bootloader_query (ret_loader, &is_active, cancellable, error))
        goto out;
    }
  if (!is_active)
    g_clear_object (&ret_loader);

  ret = TRUE;
  gs_transfer_out_value (out_bootloader, &ret_loader);
 out:
  return ret;
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
  gboolean ret = FALSE;
  gs_unref_object GFile *proc_cmdline = g_file_new_for_path ("/proc/cmdline");
  gs_free char *contents = NULL;
  gsize len;

  if (!g_file_load_contents (proc_cmdline, cancellable, &contents, &len, NULL,
                             error))
    goto out;

  g_strchomp (contents);

  ret = TRUE;
  *out_args = _ostree_kernel_args_from_string (contents);
 out:
  return ret;
}

static gboolean
find_booted_deployment (OstreeSysroot       *self,
                        GPtrArray           *deployments,
                        OstreeDeployment   **out_deployment,
                        GCancellable        *cancellable,
                        GError             **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *active_root = g_file_new_for_path ("/");
  gs_unref_object OstreeDeployment *ret_deployment = NULL;

  if (g_file_equal (active_root, self->path))
    { 
      gs_unref_object OstreeSysroot *active_deployment_root = ostree_sysroot_new_default ();
      guint i;
      const char *bootlink_arg;
      __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *kernel_args = NULL;
      guint32 root_device;
      guint64 root_inode;
      
      if (!_ostree_sysroot_get_devino (active_root, &root_device, &root_inode,
                                       cancellable, error))
        goto out;

      if (!parse_kernel_commandline (&kernel_args, cancellable, error))
        goto out;
      
      bootlink_arg = _ostree_kernel_args_get_last_value (kernel_args, "ostree");
      if (bootlink_arg)
        {
          for (i = 0; i < deployments->len; i++)
            {
              OstreeDeployment *deployment = deployments->pdata[i];
              gs_unref_object GFile *deployment_path = ostree_sysroot_get_deployment_directory (active_deployment_root, deployment);
              guint32 device;
              guint64 inode;

              if (!_ostree_sysroot_get_devino (deployment_path, &device, &inode,
                                               cancellable, error))
                goto out;

              if (device == root_device && inode == root_inode)
                {
                  ret_deployment = g_object_ref (deployment);
                  break;
                }
            }
          if (ret_deployment == NULL)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unexpected state: ostree= kernel argument found, but / is not a deployment root");
              goto out;
            }
        }
      else
        {
          /* Not an ostree system */
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_deployment, &ret_deployment);
 out:
  return ret;
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
    {
      return g_object_ref (self->booted_deployment);
    }
  else
    {
      guint i;
      for (i = 0; i < self->deployments->len; i++)
        {
          OstreeDeployment *deployment = self->deployments->pdata[i];

          if (strcmp (ostree_deployment_get_osname (deployment), osname) != 0)
            continue;
          
          return g_object_ref (deployment);
        }
    }
  return NULL;
}

/**
 * ostree_sysroot_origin_new_from_refspec:
 * @refspec: A refspec
 *
 * Returns: (transfer full): A new config file which sets @refspec as an origin
 */
GKeyFile *
ostree_sysroot_origin_new_from_refspec (OstreeSysroot  *sysroot,
                                        const char     *refspec)
{
  GKeyFile *ret = g_key_file_new ();
  g_key_file_set_string (ret, "origin", "refspec", refspec);
  return ret;
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
  gs_unref_ptrarray GPtrArray *deployments = NULL;
  gs_unref_ptrarray GPtrArray *new_deployments = g_ptr_array_new_with_free_func (g_object_unref);
  gboolean retain = (flags & OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_RETAIN) > 0;

  deployments = ostree_sysroot_get_deployments (sysroot);
  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  if (osname == NULL && booted_deployment)
    osname = ostree_deployment_get_osname (booted_deployment);

  g_ptr_array_add (new_deployments, g_object_ref (new_deployment));

  for (i = 0; i < deployments->len; i++)
    {
      OstreeDeployment *deployment = deployments->pdata[i];
      
      /* Keep deployments with different osnames, as well as the
       * booted and merge deployments
       */
      if (retain ||
          (osname != NULL &&
           strcmp (ostree_deployment_get_osname (deployment), osname) != 0) ||
          ostree_deployment_equal (deployment, booted_deployment) ||
          ostree_deployment_equal (deployment, merge_deployment))
        {
          g_ptr_array_add (new_deployments, g_object_ref (deployment));
        }
    }

  if (!ostree_sysroot_write_deployments (sysroot, new_deployments, cancellable, error))
    goto out;

  if (!ostree_sysroot_cleanup (sysroot, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

