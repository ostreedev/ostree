/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
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

#include <glib-unix.h>
#include <gio/gunixoutputstream.h>
#include <errno.h>
#include <stdio.h>
#ifdef HAVE_LIBMOUNT
#include <libmount.h>
#endif
#include <stdbool.h>
#include "otutil.h"

#include "ostree.h"
#include "ostree-core-private.h"
#include "ostree-cmdprivate.h"

#ifdef HAVE_LIBMOUNT
typedef FILE OtLibMountFile;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(OtLibMountFile, endmntent);

/* Taken from systemd path-util.c */
static bool
is_path (const char *p)
{
  return !!strchr (p, '/');
}

/* Taken from systemd path-util.c */
static char*
path_kill_slashes (char *path)
{
  char *f, *t;
  bool slash = false;

  /* Removes redundant inner and trailing slashes. Modifies the
   * passed string in-place.
   *
   * For example: ///foo///bar/ becomes /foo/bar
   */

  for (f = path, t = path; *f; f++)
    {
      if (*f == '/')
        {
          slash = true;
          continue;
        }

      if (slash)
        {
          slash = false;
          *(t++) = '/';
        }

      *(t++) = *f;
    }

  /* Special rule, if we are talking of the root directory, a
     trailing slash is good */

  if (t == path && slash)
    *(t++) = '/';

  *t = 0;
  return path;
}

/* Written by ostree-sysroot-deploy.c. We parse out the stateroot here since we
 * need to know it to mount /var. Unfortunately we can't easily use the
 * libostree API to find the booted deployment since /boot might not have been
 * mounted yet.
 */
static char *
stateroot_from_ostree_cmdline (const char *ostree_cmdline,
                               GError **error)
{
  static GRegex *regex;
  static gsize regex_initialized;
  if (g_once_init_enter (&regex_initialized))
    {
      regex = g_regex_new ("^/ostree/boot.[01]/([^/]+)/", 0, 0, NULL);
      g_assert (regex);
      g_once_init_leave (&regex_initialized, 1);
    }

  g_autoptr(GMatchInfo) match = NULL;
  if (!g_regex_match (regex, ostree_cmdline, 0, &match))
    return glnx_null_throw (error, "Failed to parse %s", ostree_cmdline);

  return g_match_info_fetch (match, 1);
}
#endif

/* Implementation of ostree-system-generator */
gboolean
_ostree_impl_system_generator (const char *ostree_cmdline,
                               const char *normal_dir,
                               const char *early_dir,
                               const char *late_dir,
                               GError    **error)
{
#ifdef HAVE_LIBMOUNT
  /* Not currently cancellable, but define a var in case we care later */
  GCancellable *cancellable = NULL;
  /* Some path constants to avoid typos */
  static const char fstab_path[] = "/etc/fstab";
  static const char var_path[] = "/var";

  /* ostree-prepare-root was patched to write the stateroot to this file */
  g_autofree char *stateroot = stateroot_from_ostree_cmdline (ostree_cmdline, error);
  if (!stateroot)
    return FALSE;

  /* Load /etc/fstab if it exists, and look for a /var mount */
  g_autoptr(OtLibMountFile) fstab = setmntent (fstab_path, "re");
  gboolean found_var_mnt = FALSE;
  if (!fstab)
    {
      if (errno != ENOENT)
        return glnx_throw_errno_prefix (error, "Reading %s", fstab_path);
    }
  else
    {
      /* Parse it */
      struct mntent *me;
      while ((me = getmntent (fstab)))
        {
          g_autofree char *where = g_strdup (me->mnt_dir);
          if (is_path (where))
            path_kill_slashes (where);

          /* We're only looking for /var here */
          if (strcmp (where, var_path) != 0)
            continue;

          found_var_mnt = TRUE;
          break;
        }
    }

  /* If we found /var, we're done */
  if (found_var_mnt)
    return TRUE;

  /* Prepare to write to the output unit dir; we use the "normal" dir
   * that overrides /usr, but not /etc.
   */
  glnx_fd_close int normal_dir_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, normal_dir, TRUE, &normal_dir_dfd, error))
    return FALSE;

  /* Generate our bind mount unit */
  const char *stateroot_var_path = glnx_strjoina ("/sysroot/ostree/deploy/", stateroot, "/var");

  g_auto(GLnxTmpfile) tmpf = { 0, };
  if (!glnx_open_tmpfile_linkable_at (normal_dir_dfd, ".", O_WRONLY | O_CLOEXEC,
                                      &tmpf, error))
    return FALSE;
  g_autoptr(GOutputStream) outstream = g_unix_output_stream_new (tmpf.fd, FALSE);
  gsize bytes_written;
  /* This code is inspired by systemd's fstab-generator.c.
   *
   * Note that our unit doesn't run if systemd.volatile is enabled;
   * see https://github.com/ostreedev/ostree/pull/856
   */
  if (!g_output_stream_printf (outstream, &bytes_written, cancellable, error,
                               "##\n# Automatically generated by ostree-system-generator\n##\n\n"
                               "[Unit]\n"
                               "Documentation=man:ostree(1)\n"
                               "ConditionKernelCommandLine=!systemd.volatile\n"
                               /* We need /sysroot mounted writable first */
                               "After=ostree-remount.service\n"
                               "Before=local-fs.target\n"
                               "\n"
                               "[Mount]\n"
                               "Where=%s\n"
                               "What=%s\n"
                               "Options=bind\n",
                               var_path,
                               stateroot_var_path))
    return FALSE;
  if (!g_output_stream_flush (outstream, cancellable, error))
    return FALSE;
  g_clear_object (&outstream);
  /* It should be readable */
  if (!glnx_fchmod (tmpf.fd, 0644, error))
    return FALSE;
  /* Error out if somehow it already exists, that'll help us debug conflicts */
  if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_NOREPLACE,
                             normal_dir_dfd, "var.mount", error))
    return FALSE;

  /* And ensure it's required; newer systemd will auto-inject fs dependencies
   * via RequiresMountsFor and the like, but on older versions (e.g. CentOS) we
   * need this. It's what the fstab generator does.  And my mother always said,
   * listen to the fstab generator.
   */
  if (!glnx_shutil_mkdir_p_at (normal_dir_dfd, "local-fs.target.requires", 0755, cancellable, error))
    return FALSE;
  if (symlinkat ("../var.mount", normal_dir_dfd, "local-fs.target.requires/var.mount") < 0)
    return glnx_throw_errno_prefix (error, "symlinkat");

  return TRUE;
#else
  return glnx_throw (error, "Not implemented");
#endif
}
