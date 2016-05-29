/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
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
#include "ostree-bootloader-syslinux.h"
#include "otutil.h"

#include <string.h>

struct _OstreeBootloaderSyslinux
{
  GObject       parent_instance;

  OstreeSysroot  *sysroot;
  GFile          *config_path;
};

typedef GObjectClass OstreeBootloaderSyslinuxClass;

static void _ostree_bootloader_syslinux_bootloader_iface_init (OstreeBootloaderInterface *iface);
G_DEFINE_TYPE_WITH_CODE (OstreeBootloaderSyslinux, _ostree_bootloader_syslinux, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_BOOTLOADER, _ostree_bootloader_syslinux_bootloader_iface_init));

static gboolean
_ostree_bootloader_syslinux_query (OstreeBootloader *bootloader,
                                   gboolean         *out_is_active,
                                   GCancellable     *cancellable,
                                   GError          **error)
{
  OstreeBootloaderSyslinux *self = OSTREE_BOOTLOADER_SYSLINUX (bootloader);

  *out_is_active = g_file_query_file_type (self->config_path, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_SYMBOLIC_LINK;
  return TRUE;
}

static const char *
_ostree_bootloader_syslinux_get_name (OstreeBootloader *bootloader)
{
  return "syslinux";
}

static gboolean
append_config_from_boostree_loader_entries (OstreeBootloaderSyslinux  *self,
                                        gboolean               regenerate_default,
                                        int                    bootversion,
                                        GPtrArray             *new_lines,
                                        GCancellable          *cancellable,
                                        GError               **error)
{
  gboolean ret = FALSE;
  g_autoptr(GPtrArray) boostree_loader_configs = NULL;
  guint i;

  if (!_ostree_sysroot_read_boot_loader_configs (self->sysroot, bootversion, &boostree_loader_configs,
                                                 cancellable, error))
    goto out;

  for (i = 0; i < boostree_loader_configs->len; i++)
    {
      OstreeBootconfigParser *config = boostree_loader_configs->pdata[i];
      const char *val;

      val = ostree_bootconfig_parser_get (config, "title");
      if (!val)
        val = "(Untitled)";

      if (regenerate_default && i == 0)
        {
          g_ptr_array_add (new_lines, g_strdup_printf ("DEFAULT %s", val));
        }

      g_ptr_array_add (new_lines, g_strdup_printf ("LABEL %s", val));
      
      val = ostree_bootconfig_parser_get (config, "linux");
      if (!val)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No \"linux\" key in bootloader config");
          goto out;
        }
      g_ptr_array_add (new_lines, g_strdup_printf ("\tKERNEL %s", val));

      val = ostree_bootconfig_parser_get (config, "initrd");
      if (val)
        g_ptr_array_add (new_lines, g_strdup_printf ("\tINITRD %s", val));

      val = ostree_bootconfig_parser_get (config, "options");
      if (val)
        g_ptr_array_add (new_lines, g_strdup_printf ("\tAPPEND %s", val));
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
_ostree_bootloader_syslinux_write_config (OstreeBootloader          *bootloader,
                                     int                    bootversion,
                                     GCancellable          *cancellable,
                                     GError               **error)
{
  gboolean ret = FALSE;
  OstreeBootloaderSyslinux *self = OSTREE_BOOTLOADER_SYSLINUX (bootloader);
  g_autoptr(GFile) new_config_path = NULL;
  g_autofree char *config_contents = NULL;
  g_autofree char *new_config_contents = NULL;
  g_autoptr(GPtrArray) new_lines = NULL;
  g_autoptr(GPtrArray) tmp_lines = NULL;
  g_autofree char *kernel_arg = NULL;
  gboolean saw_default = FALSE;
  gboolean regenerate_default = FALSE;
  gboolean parsing_label = FALSE;
  char **lines = NULL;
  char **iter;
  guint i;

  new_config_path = ot_gfile_resolve_path_printf (self->sysroot->path, "boot/loader.%d/syslinux.cfg",
                                                  bootversion);

  /* This should follow the symbolic link to the current bootversion. */
  config_contents = glnx_file_get_contents_utf8_at (AT_FDCWD, gs_file_get_path_cached (self->config_path), NULL,
                                                    cancellable, error);
  if (!config_contents)
    goto out;

  lines = g_strsplit (config_contents, "\n", -1);
  new_lines = g_ptr_array_new_with_free_func (g_free);
  tmp_lines = g_ptr_array_new_with_free_func (g_free);
  
  /* Note special iteration condition here; we want to also loop one
   * more time at the end where line = NULL to ensure we finish off
   * processing the last LABEL.
   */
  iter = lines;
  while (TRUE)
    {
      char *line = *iter;
      gboolean skip = FALSE;

      if (parsing_label && 
          (line == NULL || !g_str_has_prefix (line, "\t")))
        {
          parsing_label = FALSE;
          if (kernel_arg == NULL)
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "No KERNEL argument found after LABEL");
              goto out;
            }

          /* If this is a non-ostree kernel, just emit the lines
           * we saw.
           */
          if (!g_str_has_prefix (kernel_arg, "/ostree/"))
            {
              for (i = 0; i < tmp_lines->len; i++)
                {
                  g_ptr_array_add (new_lines, tmp_lines->pdata[i]);
                  tmp_lines->pdata[i] = NULL; /* Transfer ownership */
                }
            }
          else
            {
              /* Otherwise, we drop the config on the floor - it
               * will be regenerated.
               */
              g_ptr_array_set_size (tmp_lines, 0);
            }
        }

      if (line == NULL)
        break;

      if (!parsing_label &&
          (g_str_has_prefix (line, "LABEL ")))
        {
          parsing_label = TRUE;
          g_ptr_array_set_size (tmp_lines, 0);
        }
      else if (parsing_label && g_str_has_prefix (line, "\tKERNEL "))
        {
          g_free (kernel_arg);
          kernel_arg = g_strdup (line + strlen ("\tKERNEL "));
        }
      else if (!parsing_label &&
               (g_str_has_prefix (line, "DEFAULT ")))
        {
          saw_default = TRUE;
          /* XXX Searching for patterns in the title is rather brittle,
           *     but this hack is at least noted in the code that builds
           *     the title to hopefully avoid regressions. */
          if (g_str_has_prefix (line, "DEFAULT ostree:") ||  /* old format */
              strstr (line, "(ostree") != NULL)              /* new format */
            {
              regenerate_default = TRUE;
            }
          skip = TRUE;
        }
      
      if (skip)
        {
          g_free (line);
        }
      else
        {
          if (parsing_label)
            {
              g_ptr_array_add (tmp_lines, line);
            }
          else
            {
              g_ptr_array_add (new_lines, line);
            }
        }
      /* Transfer ownership */
      *iter = NULL;
      iter++;
    }

  if (!saw_default)
    regenerate_default = TRUE;

  if (!append_config_from_boostree_loader_entries (self, regenerate_default,
                                               bootversion, new_lines,
                                               cancellable, error))
    goto out;

  new_config_contents = _ostree_sysroot_join_lines (new_lines);
  {
    g_autoptr(GBytes) new_config_contents_bytes =
      g_bytes_new_static (new_config_contents,
                          strlen (new_config_contents));

    if (!ot_gfile_replace_contents_fsync (new_config_path, new_config_contents_bytes,
                                          cancellable, error))
      goto out;
  }
  
  ret = TRUE;
 out:
  g_free (lines); /* Note we freed elements individually */
  return ret;
}

static void
_ostree_bootloader_syslinux_finalize (GObject *object)
{
  OstreeBootloaderSyslinux *self = OSTREE_BOOTLOADER_SYSLINUX (object);

  g_clear_object (&self->sysroot);
  g_clear_object (&self->config_path);

  G_OBJECT_CLASS (_ostree_bootloader_syslinux_parent_class)->finalize (object);
}

void
_ostree_bootloader_syslinux_init (OstreeBootloaderSyslinux *self)
{
}

static void
_ostree_bootloader_syslinux_bootloader_iface_init (OstreeBootloaderInterface *iface)
{
  iface->query = _ostree_bootloader_syslinux_query;
  iface->get_name = _ostree_bootloader_syslinux_get_name;
  iface->write_config = _ostree_bootloader_syslinux_write_config;
}

void
_ostree_bootloader_syslinux_class_init (OstreeBootloaderSyslinuxClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = _ostree_bootloader_syslinux_finalize;
}

OstreeBootloaderSyslinux *
_ostree_bootloader_syslinux_new (OstreeSysroot *sysroot)
{
  OstreeBootloaderSyslinux *self = g_object_new (OSTREE_TYPE_BOOTLOADER_SYSLINUX, NULL);
  self->sysroot = g_object_ref (sysroot);
  self->config_path = g_file_resolve_relative_path (self->sysroot->path, "boot/syslinux/syslinux.cfg");
  return self;
}
