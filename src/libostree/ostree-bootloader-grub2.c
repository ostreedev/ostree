/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "ostree-sysroot-private.h"
#include "ostree-bootloader-grub2.h"
#include "otutil.h"
#include <gio/gfiledescriptorbased.h>
#include <gio/gunixoutputstream.h>
#include <sys/mount.h>

#include <string.h>

/* I only did some cursory research here, but it appears
 * that we only want to use "linux16" for x86 platforms.
 * At least, I got a report that "linux16" is definitely wrong
 * for ppc64.  See
 * http://pkgs.fedoraproject.org/cgit/rpms/grub2.git/tree/0036-Use-linux16-when-appropriate-880840.patch?h=f25
 * https://bugzilla.redhat.com/show_bug.cgi?id=1108296
 * among others.
 */
#if defined(__i386__) || defined(__x86_64__)
#define GRUB2_SUFFIX "16"
#else
#define GRUB2_SUFFIX ""
#endif
/* https://github.com/projectatomic/rpm-ostree-toolbox/issues/102#issuecomment-316483554
 * https://github.com/rhboot/grubby/blob/34b1436ccbd56eab8024314cab48f2fc880eef08/grubby.c#L63
 *
 * This is true at least on Fedora/Red Hat Enterprise Linux for aarch64.
 */
#if defined(__aarch64__)
#define GRUB2_EFI_SUFFIX ""
#else
#define GRUB2_EFI_SUFFIX "efi"
#endif

struct _OstreeBootloaderGrub2
{
  GObject       parent_instance;

  OstreeSysroot  *sysroot;
  GFile          *config_path_bios;
  GFile          *config_path_efi;
  gboolean        is_efi;
};

typedef GObjectClass OstreeBootloaderGrub2Class;

static void _ostree_bootloader_grub2_bootloader_iface_init (OstreeBootloaderInterface *iface);
G_DEFINE_TYPE_WITH_CODE (OstreeBootloaderGrub2, _ostree_bootloader_grub2, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_BOOTLOADER, _ostree_bootloader_grub2_bootloader_iface_init));

static gboolean
_ostree_bootloader_grub2_query (OstreeBootloader *bootloader,
                                gboolean         *out_is_active,
                                GCancellable     *cancellable,
                                GError          **error)
{
  OstreeBootloaderGrub2 *self = OSTREE_BOOTLOADER_GRUB2 (bootloader);

  /* Look for the BIOS path first */
  if (g_file_query_exists (self->config_path_bios, NULL))
    {
      /* If we found it, we're done */
      *out_is_active = TRUE;
      return TRUE;
    }

  g_autoptr(GFile) efi_basedir = g_file_resolve_relative_path (self->sysroot->path, "boot/efi/EFI");

  g_clear_object (&self->config_path_efi);

  if (g_file_query_exists (efi_basedir, NULL))
    {
      g_autoptr(GFileEnumerator) direnum = NULL;

      direnum = g_file_enumerate_children (efi_basedir, OSTREE_GIO_FAST_QUERYINFO,
                                           G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                           cancellable, error);
      if (!direnum)
        return FALSE;
  
      while (TRUE)
        {
          GFileInfo *file_info;
          const char *fname;

          if (!g_file_enumerator_iterate (direnum, &file_info, NULL,
                                          cancellable, error))
            return FALSE;
          if (file_info == NULL)
            break;

          fname = g_file_info_get_name (file_info);
          if (strcmp (fname, "BOOT") == 0)
            continue;

          if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
            continue;

          g_autofree char *subdir_grub_cfg =
            g_build_filename (gs_file_get_path_cached (efi_basedir), fname, "grub.cfg", NULL);

          if (g_file_test (subdir_grub_cfg, G_FILE_TEST_EXISTS))
            {
              self->config_path_efi = g_file_new_for_path (subdir_grub_cfg);
              break;
            }
        }

      /* If we found the EFI path, we're done */
      if (self->config_path_efi)
        {
          self->is_efi = TRUE;
          *out_is_active = TRUE;
          return TRUE;
        }
    }
  else
    *out_is_active = FALSE;

  return TRUE;
}

static const char *
_ostree_bootloader_grub2_get_name (OstreeBootloader *bootloader)
{
  return "grub2";
}

/* This implementation is quite complex; see this issue for
 * a starting point:
 * https://github.com/ostreedev/ostree/issues/717
 */
gboolean
_ostree_bootloader_grub2_generate_config (OstreeSysroot                 *sysroot,
                                          int                            bootversion,
                                          int                            target_fd,
                                          GCancellable                  *cancellable,
                                          GError                       **error)
{
  /* So... yeah.  Just going to hardcode these. */
  static const char hardcoded_video[] = "load_video\n"
    "set gfxpayload=keep\n";
  static const char hardcoded_insmods[] = "insmod gzio\n";
  const char *grub2_boot_device_id =
    g_getenv ("GRUB2_BOOT_DEVICE_ID");
  const char *grub2_prepare_root_cache =
    g_getenv ("GRUB2_PREPARE_ROOT_CACHE");

  /* We must have been called via the wrapper script */
  g_assert (grub2_boot_device_id != NULL);
  g_assert (grub2_prepare_root_cache != NULL);

  /* Passed from the parent */
  gboolean is_efi = g_getenv ("_OSTREE_GRUB2_IS_EFI") != NULL;

  g_autoptr(GOutputStream) out_stream = g_unix_output_stream_new (target_fd, FALSE);

  g_autoptr(GPtrArray) loader_configs = NULL;
  if (!_ostree_sysroot_read_boot_loader_configs (sysroot, bootversion,
                                                 &loader_configs,
                                                 cancellable, error))
    return FALSE;

  g_autoptr(GString) output = g_string_new ("");
  for (guint i = 0; i < loader_configs->len; i++)
    {
      OstreeBootconfigParser *config = loader_configs->pdata[i];
      const char *title;
      const char *options;
      const char *kernel;
      const char *initrd;
      char *quoted_title = NULL;
      char *uuid = NULL;
      char *quoted_uuid = NULL;

      title = ostree_bootconfig_parser_get (config, "title");
      if (!title)
        title = "(Untitled)";

      kernel = ostree_bootconfig_parser_get (config, "linux");

      quoted_title = g_shell_quote (title);
      uuid = g_strdup_printf ("ostree-%u-%s", (guint)i, grub2_boot_device_id);
      quoted_uuid = g_shell_quote (uuid);
      g_string_append_printf (output, "menuentry %s --class gnu-linux --class gnu --class os --unrestricted %s {\n", quoted_title, quoted_uuid);
      g_free (uuid);
      g_free (quoted_title);
      g_free (quoted_uuid);

      /* Hardcoded sections */
      g_string_append (output, hardcoded_video);
      g_string_append (output, hardcoded_insmods);
      g_string_append (output, grub2_prepare_root_cache);
      g_string_append_c (output, '\n');

      if (!kernel)
        return glnx_throw (error, "No \"linux\" key in bootloader config");
      g_string_append (output, "linux");
      if (is_efi)
        g_string_append (output, GRUB2_EFI_SUFFIX);
      else
        g_string_append (output, GRUB2_SUFFIX);
      g_string_append_c (output, ' ');
      g_string_append (output, kernel);

      options = ostree_bootconfig_parser_get (config, "options");
      if (options)
        {
          g_string_append_c (output, ' ');
          g_string_append (output, options);
        }
      g_string_append_c (output, '\n');

      initrd = ostree_bootconfig_parser_get (config, "initrd");
      if (initrd)
        {
          g_string_append (output, "initrd");
          if (is_efi)
            g_string_append (output, GRUB2_EFI_SUFFIX);
          else
            g_string_append (output, GRUB2_SUFFIX);
          g_string_append_c (output, ' ');
          g_string_append (output, initrd);
          g_string_append_c (output, '\n');
        }

      g_string_append (output, "}\n");
    }

  gsize bytes_written;
  if (!g_output_stream_write_all (out_stream, output->str, output->len,
                                  &bytes_written, cancellable, error))
    return FALSE;

  return TRUE;
}

typedef struct {
  const char *root;
  const char *bootversion_str;
  gboolean is_efi;
} Grub2ChildSetupData;

/* Post-fork, pre-exec child setup for grub2-mkconfig */
static void
grub2_child_setup (gpointer user_data)
{
  Grub2ChildSetupData *cdata = user_data;

  setenv ("_OSTREE_GRUB2_BOOTVERSION", cdata->bootversion_str, TRUE);
  /* We have to pass our state (whether or not we're using EFI) to the child */
  if (cdata->is_efi)
    setenv ("_OSTREE_GRUB2_IS_EFI", "1", TRUE);

  /* Everything below this is dealing with the chroot case; if
   * we're not doing that, return early.
   */
  if (!cdata->root)
    return;

  /* TODO: investigate replacing this with bwrap */
  if (chdir (cdata->root) != 0)
    {
      perror ("chdir");
      _exit (1);
    }

  if (unshare (CLONE_NEWNS) != 0)
    {
      perror ("CLONE_NEWNS");
      _exit (1);
    }

  if (mount (NULL, "/", "none", MS_REC|MS_PRIVATE, NULL) < 0)
    {
      perror ("Failed to make / a private mount");
      _exit (1);
    }

  if (mount (".", ".", NULL, MS_BIND | MS_PRIVATE, NULL) < 0)
    {
      perror ("mount (MS_BIND)");
      _exit (1);
    }

  if (mount (cdata->root, "/", NULL, MS_MOVE, NULL) < 0)
    {
      perror ("failed to MS_MOVE to /");
      _exit (1);
    }

  if (chroot (".") != 0)
    {
      perror ("chroot");
      _exit (1);
    }
}

/* Main entrypoint for writing GRUB configuration. */
static gboolean
_ostree_bootloader_grub2_write_config (OstreeBootloader      *bootloader,
                                       int                    bootversion,
                                       GCancellable          *cancellable,
                                       GError               **error)
{
  OstreeBootloaderGrub2 *self = OSTREE_BOOTLOADER_GRUB2 (bootloader);

  /* Autotests can set this envvar to select which code path to test, useful for OS installers as well */
  gboolean use_system_grub2_mkconfig = TRUE;
#ifdef USE_BUILTIN_GRUB2_MKCONFIG
  use_system_grub2_mkconfig = FALSE;
#endif
  const gchar *grub_exec = g_getenv ("OSTREE_GRUB2_EXEC");
  if (grub_exec)
    {
      if (g_str_has_suffix (grub_exec, GRUB2_MKCONFIG_PATH))
        use_system_grub2_mkconfig = TRUE;
      else
        use_system_grub2_mkconfig = FALSE;
    }
  else
    grub_exec = use_system_grub2_mkconfig ? GRUB2_MKCONFIG_PATH : TARGET_PREFIX "/lib/ostree/ostree-grub-generator";

  g_autofree char *grub2_mkconfig_chroot = NULL;
  if (use_system_grub2_mkconfig && ostree_sysroot_get_booted_deployment (self->sysroot) == NULL
      && g_file_has_parent (self->sysroot->path, NULL))
    {
      g_autoptr(GPtrArray) deployments = NULL;
      OstreeDeployment *tool_deployment;
      g_autoptr(GFile) tool_deployment_root = NULL;

      deployments = ostree_sysroot_get_deployments (self->sysroot);

      g_assert_cmpint (deployments->len, >, 0);

      tool_deployment = deployments->pdata[0];

      /* Sadly we have to execute code to generate the bootloader configuration.
       * If we're in a booted deployment, we just don't chroot.
       *
       * In the case of an installer, use the first deployment root (which
       * will most likely be the only one.
       *
       * This all only applies if we're not using the builtin
       * generator, which handles being run outside of the root.
       */
      tool_deployment_root = ostree_sysroot_get_deployment_directory (self->sysroot, tool_deployment);
      grub2_mkconfig_chroot = g_file_get_path (tool_deployment_root);
    }

  g_autoptr(GFile) new_config_path = NULL;
  g_autoptr(GFile) config_path_efi_dir = NULL;
  if (self->is_efi)
    {
      config_path_efi_dir = g_file_get_parent (self->config_path_efi);
      new_config_path = g_file_get_child (config_path_efi_dir, "grub.cfg.new");
      /* We use grub2-mkconfig to write to a temporary file first */
      if (!ot_gfile_ensure_unlinked (new_config_path, cancellable, error))
        return FALSE;
    }
  else
    {
      new_config_path = ot_gfile_resolve_path_printf (self->sysroot->path, "boot/loader.%d/grub.cfg",
                                                      bootversion);
    }

  const char *grub_argv[4] = { NULL, "-o", NULL, NULL};
  Grub2ChildSetupData cdata = { NULL, };
  grub_argv[0] = grub_exec;
  grub_argv[2] = gs_file_get_path_cached (new_config_path);

  GSpawnFlags grub_spawnflags = G_SPAWN_SEARCH_PATH;
  if (!g_getenv ("OSTREE_DEBUG_GRUB2"))
    grub_spawnflags |= G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL;
  cdata.root = grub2_mkconfig_chroot;
  g_autofree char *bootversion_str = g_strdup_printf ("%u", (guint)bootversion);
  cdata.bootversion_str = bootversion_str;
  cdata.is_efi = self->is_efi;
  /* Note in older versions of the grub2 package, this script doesn't even try
     to be atomic; it just does:

     cat ${grub_cfg}.new > ${grub_cfg}
     rm -f ${grub_cfg}.new

     Upstream is fixed though.
  */
  int grub2_estatus;
  if (!g_spawn_sync (NULL, (char**)grub_argv, NULL, grub_spawnflags,
                     grub2_child_setup, &cdata, NULL, NULL,
                     &grub2_estatus, error))
    return FALSE;
  if (!g_spawn_check_exit_status (grub2_estatus, error))
    {
      g_prefix_error (error, "%s: ", grub_argv[0]);
      return FALSE;
    }

  /* Now let's fdatasync() for the new file */
  { glnx_fd_close int new_config_fd = -1;
    if (!glnx_openat_rdonly (AT_FDCWD, gs_file_get_path_cached (new_config_path), TRUE, &new_config_fd, error))
      return FALSE;

    if (fdatasync (new_config_fd) < 0)
      return glnx_throw_errno_prefix (error, "fdatasync");
  }

  if (self->is_efi)
    {
      g_autoptr(GFile) config_path_efi_old = g_file_get_child (config_path_efi_dir, "grub.cfg.old");

      /* copy current to old */
      if (!ot_gfile_ensure_unlinked (config_path_efi_old, cancellable, error))
        return FALSE;
      if (!g_file_copy (self->config_path_efi, config_path_efi_old,
                        G_FILE_COPY_OVERWRITE, cancellable, NULL, NULL, error))
        return FALSE;

      /* NOTE: NON-ATOMIC REPLACEMENT; WE can't do anything else on FAT;
       * see https://bugzilla.gnome.org/show_bug.cgi?id=724246
       */
      if (!ot_gfile_ensure_unlinked (self->config_path_efi, cancellable, error))
        return FALSE;
      if (rename (gs_file_get_path_cached (new_config_path), gs_file_get_path_cached (self->config_path_efi)) < 0)
        return glnx_throw_errno_prefix (error, "rename");
    }

  return TRUE;
}

static gboolean
_ostree_bootloader_grub2_is_atomic (OstreeBootloader      *bootloader) 
{
  OstreeBootloaderGrub2 *self = OSTREE_BOOTLOADER_GRUB2 (bootloader);
  return !self->is_efi;
}

static void
_ostree_bootloader_grub2_finalize (GObject *object)
{
  OstreeBootloaderGrub2 *self = OSTREE_BOOTLOADER_GRUB2 (object);

  g_clear_object (&self->sysroot);
  g_clear_object (&self->config_path_bios);
  g_clear_object (&self->config_path_efi);

  G_OBJECT_CLASS (_ostree_bootloader_grub2_parent_class)->finalize (object);
}

void
_ostree_bootloader_grub2_init (OstreeBootloaderGrub2 *self)
{
}

static void
_ostree_bootloader_grub2_bootloader_iface_init (OstreeBootloaderInterface *iface)
{
  iface->query = _ostree_bootloader_grub2_query;
  iface->get_name = _ostree_bootloader_grub2_get_name;
  iface->write_config = _ostree_bootloader_grub2_write_config;
  iface->is_atomic = _ostree_bootloader_grub2_is_atomic;
}

void
_ostree_bootloader_grub2_class_init (OstreeBootloaderGrub2Class *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = _ostree_bootloader_grub2_finalize;
}

OstreeBootloaderGrub2 *
_ostree_bootloader_grub2_new (OstreeSysroot *sysroot)
{
  OstreeBootloaderGrub2 *self = g_object_new (OSTREE_TYPE_BOOTLOADER_GRUB2, NULL);
  self->sysroot = g_object_ref (sysroot);
  self->config_path_bios = g_file_resolve_relative_path (self->sysroot->path, "boot/grub2/grub.cfg");
  return self;
}
