/*
 * Copyright (C) Red Hat, Inc.
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <sys/ioctl.h>

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "otutil.h"
#include "ot-fs-utils.h"
#include <libfsverity.h>

/* libfsverity has a thread-unsafe process global error callback */
G_LOCK_DEFINE_STATIC(ot_fsverity_err_lock);
static char *ot_last_fsverity_error;

static void 
ot_on_libfsverity_error(const char *msg)
{
  G_LOCK (ot_fsverity_err_lock);
  g_free (ot_last_fsverity_error);
  ot_last_fsverity_error = g_strdup (msg);
  G_UNLOCK (ot_fsverity_err_lock);
}

static gboolean
throw_fsverity_error (GError **error, const char *prefix)
{
  g_autofree char *msg = NULL;
  G_LOCK (ot_fsverity_err_lock);
  msg = g_steal_pointer (&ot_last_fsverity_error);
  G_UNLOCK (ot_fsverity_err_lock);
  return glnx_throw (error, "%s: %s", prefix, msg);
}

static char *
maybe_make_repo_relative (OstreeRepo *repo, const char *path)
{
  if (path[0] == '/')
    return g_strdup (path);
  return g_strdup_printf ("/proc/self/fd/%d/%s", repo->repo_dir_fd, glnx_basename (path));
}

gboolean 
_ostree_repo_parse_fsverity_config (OstreeRepo *self, GError **error)
{
  struct stat stbuf;
  g_autofree char *keypath = NULL;
  if (!ot_keyfile_get_value_with_default (self->config, _OSTREE_FSVERITY_CONFIG_KEY, "key",
                                          NULL, &keypath, error))
    return FALSE;
  /* If no key is set, we're done */
  if (keypath == NULL)
    return TRUE;

  self->fsverity_key = maybe_make_repo_relative (self, keypath);
  if (!glnx_fstatat (self->repo_dir_fd, self->fsverity_key, &stbuf, 0, error))
    return glnx_prefix_error (error, "Couldn't access fsverity key");
  /* Enforce not-world-readable for the same reason as ssh */
  if (stbuf.st_mode & S_IROTH)
    return glnx_throw (error, "fsverity key must not be world-readable");

  g_autofree char *certpath = NULL;
  if (!ot_keyfile_get_value_with_default (self->config, _OSTREE_FSVERITY_CONFIG_KEY, "cert", NULL, &certpath, error))
    return FALSE;
  if (!certpath)
    return glnx_throw (error, "fsverity key specified, but no certificate");
  self->fsverity_cert = maybe_make_repo_relative (self, certpath);
  if (!glnx_fstatat (self->repo_dir_fd, self->fsverity_cert, &stbuf, 0, error))
    return glnx_prefix_error (error, "Couldn't access fsverity certificate");

  /* Process global state is bad.  We want to support multiple ostree repos per process.
   * At some point we should try patching
   * libfsverity to have something GError like that gives us a string too.
   */
  static gsize initialized = 0;
  if (g_once_init_enter (&initialized))
    {
      libfsverity_set_error_callback (ot_on_libfsverity_error);
      g_once_init_leave (&initialized, 1);
    }

  return TRUE;
}

static int 
ot_fsverity_read_callback (void *file, void *bufp, size_t count)
{
	errno = 0;
  int fd = GPOINTER_TO_INT (file);
  guint8* buf = bufp;
  while (count > 0)
    {
      ssize_t n = read (fd, buf, MIN (count, INT_MAX));
      if (n < 0)
        return -errno;
      if (n == 0)
        return -EIO;
      buf += n;
      count -= n;
    }
	return 0;
}

/* Enable verity on a file, respecting the "wanted" and "supported" states.
 * The main idea here is to optimize out pointlessly calling the ioctl()
 * over and over in cases where it's not supported for the repo's filesystem,
 * as well as to support "opportunistic" use (requested and if filesystem supports).
 * */
gboolean
_ostree_tmpf_fsverity (OstreeRepo  *self,
                       GLnxTmpfile *tmpf,
                       GError    **error)
{
  GLNX_AUTO_PREFIX_ERROR ("ostree/fsverity", error);

  if (!self->fs_verity_wanted)
    return TRUE;

	struct libfsverity_signature_params sig_params = {
    .keyfile = self->fsverity_key,
    .certfile = self->fsverity_cert,
  };

  /* fs-verity requires a read-only file descriptor */
  if (!glnx_tmpfile_reopen_rdonly (tmpf, error))
    return FALSE;

  struct stat stbuf;
  if (!glnx_fstat (tmpf->fd, &stbuf, error))
    return FALSE;

  struct libfsverity_merkle_tree_params tree_params = {
    .version = 1,
    .hash_algorithm = FS_VERITY_HASH_ALG_SHA256,
    .file_size = stbuf.st_size,
    .block_size = 4096,
    /* TODO: salt? */
  };
  g_autofree struct libfsverity_digest *digest = NULL;
	if (libfsverity_compute_digest (GINT_TO_POINTER (tmpf->fd), ot_fsverity_read_callback, &tree_params, &digest) < 0)
    return throw_fsverity_error (error, "failed to compute digest");

	guint8 *sig = NULL;
	size_t sig_size;
	if (libfsverity_sign_digest (digest, &sig_params, &sig, &sig_size) < 0)
    return throw_fsverity_error (error, "failed to generate signature");

  if (libfsverity_enable_with_sig (tmpf->fd, &tree_params, sig, sig_size) < 0)
    return throw_fsverity_error (error, "failed to enable fsverity for file");

  return TRUE;
}
