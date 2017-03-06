/*
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

static const char syslinux_config_path[] = "boot/syslinux/syslinux.cfg";

struct _OstreeBootloaderSyslinux
{
  GObject       parent_instance;

  OstreeSysroot  *sysroot;
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
  struct stat stbuf;

  if (!glnx_fstatat_allow_noent (self->sysroot->sysroot_fd, syslinux_config_path, &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return FALSE;
  *out_is_active = (errno == 0);
  return TRUE;
}

static const char *
_ostree_bootloader_syslinux_get_name (OstreeBootloader *bootloader)
{
  return "syslinux";
}

static gboolean
append_config_from_loader_entries (OstreeBootloaderSyslinux  *self,
                                   gboolean               regenerate_default,
                                   int                    bootversion,
                                   GPtrArray             *new_lines,
                                   GCancellable          *cancellable,
                                   GError               **error)
{
  g_autoptr(GPtrArray) loader_configs = NULL;
  if (!_ostree_sysroot_read_boot_loader_configs (self->sysroot, bootversion, &loader_configs,
                                                 cancellable, error))
    return FALSE;

  for (guint i = 0; i < loader_configs->len; i++)
    {
      OstreeBootconfigParser *config = loader_configs->pdata[i];
      const char *val = ostree_bootconfig_parser_get (config, "title");
      if (!val)
        val = "(Untitled)";

      if (regenerate_default && i == 0)
        g_ptr_array_add (new_lines, g_strdup_printf ("DEFAULT %s", val));

      g_ptr_array_add (new_lines, g_strdup_printf ("LABEL %s", val));

      val = ostree_bootconfig_parser_get (config, "linux");
      if (!val)
        return glnx_throw (error, "No \"linux\" key in bootloader config");
      g_ptr_array_add (new_lines, g_strdup_printf ("\tKERNEL %s", val));

      val = ostree_bootconfig_parser_get (config, "initrd");
      if (val)
        g_ptr_array_add (new_lines, g_strdup_printf ("\tINITRD %s", val));

      val = ostree_bootconfig_parser_get (config, "devicetree");
      if (val)
        g_ptr_array_add (new_lines, g_strdup_printf ("\tDEVICETREE %s", val));

      val = ostree_bootconfig_parser_get (config, "options");
      if (val)
        g_ptr_array_add (new_lines, g_strdup_printf ("\tAPPEND %s", val));
    }

  return TRUE;
}

static gboolean
_ostree_bootloader_syslinux_write_config (OstreeBootloader          *bootloader,
                                          int                    bootversion,
                                          GCancellable          *cancellable,
                                          GError               **error)
{
  OstreeBootloaderSyslinux *self = OSTREE_BOOTLOADER_SYSLINUX (bootloader);

  g_autofree char *new_config_path =
    g_strdup_printf ("boot/loader.%d/syslinux.cfg", bootversion);

  /* This should follow the symbolic link to the current bootversion. */
  g_autofree char *config_contents =
    glnx_file_get_contents_utf8_at (self->sysroot->sysroot_fd, syslinux_config_path, NULL,
                                    cancellable, error);
  if (!config_contents)
    return FALSE;

  g_auto(GStrv) lines = g_strsplit (config_contents, "\n", -1);
  g_autoptr(GPtrArray) new_lines = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) tmp_lines = g_ptr_array_new_with_free_func (g_free);

  g_autofree char *kernel_arg = NULL;
  gboolean saw_default = FALSE;
  gboolean regenerate_default = FALSE;
  gboolean parsing_label = FALSE;
  /* Note special iteration condition here; we want to also loop one
   * more time at the end where line = NULL to ensure we finish off
   * processing the last LABEL.
   */
  for (char **iter = lines; iter; iter++)
    {
      const char *line = *iter;
      gboolean skip = FALSE;

      if (parsing_label &&
          (line == NULL || !g_str_has_prefix (line, "\t")))
        {
          parsing_label = FALSE;
          if (kernel_arg == NULL)
            return glnx_throw (error, "No KERNEL argument found after LABEL");

          /* If this is a non-ostree kernel, just emit the lines
           * we saw.
           */
          if (!g_str_has_prefix (kernel_arg, "/ostree/"))
            {
              for (guint i = 0; i < tmp_lines->len; i++)
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
            regenerate_default = TRUE;
          skip = TRUE;
        }

      if (!skip)
        {
          if (parsing_label)
            g_ptr_array_add (tmp_lines, g_strdup (line));
          else
            g_ptr_array_add (new_lines, g_strdup (line));
        }
    }

  if (!saw_default)
    regenerate_default = TRUE;

  if (!append_config_from_loader_entries (self, regenerate_default,
                                          bootversion, new_lines,
                                          cancellable, error))
    return FALSE;

  g_autofree char *new_config_contents = _ostree_sysroot_join_lines (new_lines);
  if (!glnx_file_replace_contents_at (self->sysroot->sysroot_fd, new_config_path,
                                      (guint8*)new_config_contents, strlen (new_config_contents),
                                      GLNX_FILE_REPLACE_DATASYNC_NEW,
                                      cancellable, error))
    return FALSE;

  return TRUE;
}

static void
_ostree_bootloader_syslinux_finalize (GObject *object)
{
  OstreeBootloaderSyslinux *self = OSTREE_BOOTLOADER_SYSLINUX (object);

  g_clear_object (&self->sysroot);

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
  return self;
}
