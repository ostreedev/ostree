/*
 * Copyright (C) 2019 Colin Walters <walters@verbum.org>
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
#include "ostree-bootloader-zipl.h"
#include "otutil.h"

#include <string.h>

/* This is specific to zipl today, but in the future we could also
 * use it for the grub2-mkconfig case.
 */
static const char zipl_requires_execute_path[] = "boot/ostree-bootloader-update.stamp";

struct _OstreeBootloaderZipl
{
  GObject       parent_instance;

  OstreeSysroot  *sysroot;
};

typedef GObjectClass OstreeBootloaderZiplClass;

static void _ostree_bootloader_zipl_bootloader_iface_init (OstreeBootloaderInterface *iface);
G_DEFINE_TYPE_WITH_CODE (OstreeBootloaderZipl, _ostree_bootloader_zipl, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_BOOTLOADER, _ostree_bootloader_zipl_bootloader_iface_init));

static gboolean
_ostree_bootloader_zipl_query (OstreeBootloader *bootloader,
                                   gboolean         *out_is_active,
                                   GCancellable     *cancellable,
                                   GError          **error)
{
  /* We don't auto-detect this one; should be explicitly chosen right now.
   * see also https://github.com/coreos/coreos-assembler/pull/849
   */
  *out_is_active = FALSE;
  return TRUE;
}

static const char *
_ostree_bootloader_zipl_get_name (OstreeBootloader *bootloader)
{
  return "zipl";
}

static gboolean
_ostree_bootloader_zipl_write_config (OstreeBootloader  *bootloader,
                                          int                bootversion,
                                          GPtrArray         *new_deployments,
                                          GCancellable      *cancellable,
                                          GError           **error)
{
  OstreeBootloaderZipl *self = OSTREE_BOOTLOADER_ZIPL (bootloader);

  /* Write our stamp file */
  if (!glnx_file_replace_contents_at (self->sysroot->sysroot_fd, zipl_requires_execute_path,
                                      (guint8*)"", 0, GLNX_FILE_REPLACE_NODATASYNC,
                                      cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
_ostree_bootloader_zipl_post_bls_sync (OstreeBootloader  *bootloader,
                                       GCancellable  *cancellable,
                                       GError       **error)
{
  OstreeBootloaderZipl *self = OSTREE_BOOTLOADER_ZIPL (bootloader);

  /* Note that unlike the grub2-mkconfig backend, we make no attempt to
   * chroot().
   */
  g_assert (self->sysroot->booted_deployment);

  if (!glnx_fstatat_allow_noent (self->sysroot->sysroot_fd, zipl_requires_execute_path, NULL, 0, error))
    return FALSE;

  /* If there's no stamp file, nothing to do */
  if (errno == ENOENT)
    return TRUE;

  const char *const zipl_argv[] = {"zipl", NULL};
  int estatus;
  if (!g_spawn_sync (NULL, (char**)zipl_argv, NULL, G_SPAWN_SEARCH_PATH,
                     NULL, NULL, NULL, NULL, &estatus, error))
    return FALSE;
  if (!g_spawn_check_exit_status (estatus, error))
    return FALSE;
  if (!glnx_unlinkat (self->sysroot->sysroot_fd, zipl_requires_execute_path, 0, error))
    return FALSE;
  return TRUE;
}

static void
_ostree_bootloader_zipl_finalize (GObject *object)
{
  OstreeBootloaderZipl *self = OSTREE_BOOTLOADER_ZIPL (object);

  g_clear_object (&self->sysroot);

  G_OBJECT_CLASS (_ostree_bootloader_zipl_parent_class)->finalize (object);
}

void
_ostree_bootloader_zipl_init (OstreeBootloaderZipl *self)
{
}

static void
_ostree_bootloader_zipl_bootloader_iface_init (OstreeBootloaderInterface *iface)
{
  iface->query = _ostree_bootloader_zipl_query;
  iface->get_name = _ostree_bootloader_zipl_get_name;
  iface->write_config = _ostree_bootloader_zipl_write_config;
  iface->post_bls_sync = _ostree_bootloader_zipl_post_bls_sync;
}

void
_ostree_bootloader_zipl_class_init (OstreeBootloaderZiplClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = _ostree_bootloader_zipl_finalize;
}

OstreeBootloaderZipl *
_ostree_bootloader_zipl_new (OstreeSysroot *sysroot)
{
  OstreeBootloaderZipl *self = g_object_new (OSTREE_TYPE_BOOTLOADER_ZIPL, NULL);
  self->sysroot = g_object_ref (sysroot);
  return self;
}
