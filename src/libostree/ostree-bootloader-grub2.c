/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
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
#include "libgsystem.h"

#include <string.h>

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
  gboolean ret = FALSE;
  OstreeBootloaderGrub2 *self = OSTREE_BOOTLOADER_GRUB2 (bootloader);
  gs_unref_object GFile* efi_basedir = NULL;
  gs_unref_object GFileInfo *file_info = NULL;

  if (g_file_query_exists (self->config_path_bios, NULL))
    {
      *out_is_active = TRUE;
      ret = TRUE;
      goto out;
    }

  efi_basedir = g_file_resolve_relative_path (self->sysroot->path, "boot/efi/EFI");

  g_clear_object (&self->config_path_efi);

  if (g_file_query_exists (efi_basedir, NULL))
    {
      gs_unref_object GFileEnumerator *direnum = NULL;

      direnum = g_file_enumerate_children (efi_basedir, OSTREE_GIO_FAST_QUERYINFO,
                                           G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                           cancellable, error);
      if (!direnum)
        goto out;
  
      while (TRUE)
        {
          GFileInfo *file_info;
          const char *fname;
          gs_free char *subdir_grub_cfg = NULL;

          if (!gs_file_enumerator_iterate (direnum, &file_info, NULL,
                                           cancellable, error))
            goto out;
          if (file_info == NULL)
            break;

          fname = g_file_info_get_name (file_info);
          if (strcmp (fname, "BOOT") == 0)
            continue;

          if (g_file_info_get_file_type (file_info) != G_FILE_TYPE_DIRECTORY)
            continue;

          subdir_grub_cfg = g_build_filename (gs_file_get_path_cached (efi_basedir), fname, "grub.cfg", NULL); 
          
          if (g_file_test (subdir_grub_cfg, G_FILE_TEST_EXISTS))
            {
              self->config_path_efi = g_file_new_for_path (subdir_grub_cfg);
              break;
            }
        }

      if (self->config_path_efi)
        {
          self->is_efi = TRUE;
          *out_is_active = TRUE;
          ret = TRUE;
          goto out;
        }
    }
  else
    *out_is_active = FALSE;

  ret = TRUE;
 out:
  return ret;
}

static const char *
_ostree_bootloader_grub2_get_name (OstreeBootloader *bootloader)
{
  return "grub2";
}

gboolean
_ostree_bootloader_grub2_generate_config (OstreeSysroot                 *sysroot,
                                          int                            bootversion,
                                          int                            target_fd,
                                          GCancellable                  *cancellable,
                                          GError                       **error)
{
  gboolean ret = FALSE;
  GString *output = g_string_new ("");
  gs_unref_object GOutputStream *out_stream = NULL;
  gs_unref_ptrarray GPtrArray *loader_configs = NULL;
  guint i;
  gsize bytes_written;
  gboolean is_efi;
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
  is_efi = g_getenv ("_OSTREE_GRUB2_IS_EFI") != NULL;

  out_stream = g_unix_output_stream_new (target_fd, FALSE);

  if (!_ostree_sysroot_read_boot_loader_configs (sysroot, bootversion,
                                                 &loader_configs,
                                                 cancellable, error))
    goto out;

  for (i = 0; i < loader_configs->len; i++)
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
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No \"linux\" key in bootloader config");
          goto out;
        }
      if (is_efi)
        g_string_append (output, "linuxefi ");
      else
        g_string_append (output, "linux16 ");
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
          if (is_efi)
            g_string_append (output, "initrdefi ");
          else
            g_string_append (output, "initrd16 ");
          g_string_append (output, initrd);
          g_string_append_c (output, '\n');
        }

      g_string_append (output, "}\n");
    }

  if (!g_output_stream_write_all (out_stream, output->str, output->len,
                                  &bytes_written, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (output)
    g_string_free (output, TRUE);
  return ret;
}

static gboolean
_ostree_bootloader_grub2_write_config (OstreeBootloader      *bootloader,
                                       int                    bootversion,
                                       GCancellable          *cancellable,
                                       GError               **error)
{
  OstreeBootloaderGrub2 *self = OSTREE_BOOTLOADER_GRUB2 (bootloader);
  gboolean ret = FALSE;
  gs_unref_object GFile *efi_new_config_temp = NULL;
  gs_unref_object GFile *efi_orig_config = NULL;
  gs_unref_object GFile *new_config_path = NULL;
  gs_unref_object GSSubprocessContext *procctx = NULL;
  gs_unref_object GSSubprocess *proc = NULL;
  gs_strfreev char **child_env = g_get_environ ();
  gs_free char *bootversion_str = g_strdup_printf ("%u", (guint)bootversion);
  gs_unref_object GFile *config_path_efi_dir = NULL;

  if (self->is_efi)
    {
      config_path_efi_dir = g_file_get_parent (self->config_path_efi);
      new_config_path = g_file_get_child (config_path_efi_dir, "grub.cfg.new");
      /* We use grub2-mkconfig to write to a temporary file first */
      if (!ot_gfile_ensure_unlinked (new_config_path, cancellable, error))
        goto out;
    }
  else
    {
      new_config_path = ot_gfile_resolve_path_printf (self->sysroot->path, "boot/loader.%d/grub.cfg",
                                                      bootversion);
    }

  procctx = gs_subprocess_context_newv ("grub2-mkconfig", "-o",
                                        gs_file_get_path_cached (new_config_path),
                                        NULL);
  child_env = g_environ_setenv (child_env, "_OSTREE_GRUB2_BOOTVERSION", bootversion_str, TRUE);
  /* We have to pass our state to the child */
  if (self->is_efi)
    child_env = g_environ_setenv (child_env, "_OSTREE_GRUB2_IS_EFI", "1", TRUE);
  gs_subprocess_context_set_environment (procctx, child_env);
  gs_subprocess_context_set_stdout_disposition (procctx, GS_SUBPROCESS_STREAM_DISPOSITION_NULL);
  if (g_getenv ("OSTREE_DEBUG_GRUB2"))
    gs_subprocess_context_set_stderr_disposition (procctx, GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT);
  else
    gs_subprocess_context_set_stderr_disposition (procctx, GS_SUBPROCESS_STREAM_DISPOSITION_NULL);

  /* In the current Fedora grub2 package, this script doesn't even try
     to be atomic; it just does:

cat ${grub_cfg}.new > ${grub_cfg}
rm -f ${grub_cfg}.new

     Upstream is fixed though.
  */
  proc = gs_subprocess_new (procctx, cancellable, error);
  if (!proc)
    goto out;

  if (!gs_subprocess_wait_sync_check (proc, cancellable, error))
    goto out;

  /* Now let's fdatasync() for the new file */
  if (!gs_file_sync_data (new_config_path, cancellable, error))
    goto out;

  if (self->is_efi)
    {
      gs_unref_object GFile *config_path_efi_old = g_file_get_child (config_path_efi_dir, "grub.cfg.old");
      
      /* copy current to old */
      if (!ot_gfile_ensure_unlinked (config_path_efi_old, cancellable, error))
        goto out;
      if (!g_file_copy (self->config_path_efi, config_path_efi_old,
                        G_FILE_COPY_OVERWRITE, cancellable, NULL, NULL, error))
        goto out;
      if (!ot_gfile_ensure_unlinked (config_path_efi_old, cancellable, error))
        goto out;

      /* NOTE: NON-ATOMIC REPLACEMENT; WE can't do anything else on FAT;
       * see https://bugzilla.gnome.org/show_bug.cgi?id=724246
       */
      if (!ot_gfile_ensure_unlinked (new_config_path, cancellable, error))
        goto out;
      if (!gs_file_rename (new_config_path, self->config_path_efi,
                           cancellable, error))
        goto out;
    }
  
  ret = TRUE;
 out:
  return ret;
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
