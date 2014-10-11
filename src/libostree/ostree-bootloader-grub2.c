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
};

typedef GObjectClass OstreeBootloaderGrub2Class;

static void _ostree_bootloader_grub2_bootloader_iface_init (OstreeBootloaderInterface *iface);
G_DEFINE_TYPE_WITH_CODE (OstreeBootloaderGrub2, _ostree_bootloader_grub2, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_BOOTLOADER, _ostree_bootloader_grub2_bootloader_iface_init));

static gboolean
_ostree_bootloader_grub2_query (OstreeBootloader *bootloader)
{
  OstreeBootloaderGrub2 *self = OSTREE_BOOTLOADER_GRUB2 (bootloader);

  return g_file_query_exists (self->config_path_bios, NULL);
}

static const char *
_ostree_bootloader_grub2_get_name (OstreeBootloader *bootloader)
{
  return "grub2";
}

gboolean
_ostree_bootloader_grub2_generate_config (OstreeBootloaderGrub2         *self,
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

  out_stream = g_unix_output_stream_new (target_fd, FALSE);

  if (!_ostree_sysroot_read_boot_loader_configs (self->sysroot, bootversion,
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
  gs_unref_object GFile *new_config_path = NULL;
  gs_unref_object GSSubprocessContext *procctx = NULL;
  gs_unref_object GSSubprocess *proc = NULL;
  gs_strfreev char **child_env = g_get_environ ();
  gs_free char *bootversion_str = g_strdup_printf ("%u", (guint)bootversion);

  new_config_path = ot_gfile_resolve_path_printf (self->sysroot->path, "boot/loader.%d/grub.cfg",
                                                  bootversion);

  procctx = gs_subprocess_context_newv ("grub2-mkconfig", "-o",
                                        gs_file_get_path_cached (new_config_path),
                                        NULL);
  child_env = g_environ_setenv (child_env, "_OSTREE_GRUB2_BOOTVERSION", bootversion_str, TRUE);
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
  
  ret = TRUE;
 out:
  return ret;
}

static gboolean
_ostree_bootloader_grub2_is_atomic (OstreeBootloader      *bootloader) 
{
  return TRUE;
}

static void
_ostree_bootloader_grub2_finalize (GObject *object)
{
  OstreeBootloaderGrub2 *self = OSTREE_BOOTLOADER_GRUB2 (object);

  g_clear_object (&self->sysroot);
  g_clear_object (&self->config_path_bios);

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
