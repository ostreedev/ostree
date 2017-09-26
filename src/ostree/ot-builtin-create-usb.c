/*
 * Copyright © 2017 Endless Mobile, Inc.
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
 *
 * Authors:
 *  - Philip Withnall <withnall@endlessm.com>
 */

#include "config.h"

#include "ot-main.h"
#include "ot-builtins.h"
#include "ostree.h"
#include "otutil.h"

#include "ostree-remote-private.h"

static gboolean opt_disable_fsync = FALSE;
static char *opt_destination_repo = NULL;

static GOptionEntry options[] =
  {
    { "disable-fsync", 0, 0, G_OPTION_ARG_NONE, &opt_disable_fsync, "Do not invoke fsync()", NULL },
    { "destination-repo", 0, 0, G_OPTION_ARG_FILENAME, &opt_destination_repo, "Use custom repository directory within the mount", NULL },
    { NULL }
  };

/* TODO: Add a man page. */
gboolean
ostree_builtin_create_usb (int            argc,
                           char         **argv,
                           GCancellable  *cancellable,
                           GError       **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeAsyncProgress) progress = NULL;
  g_auto(GLnxConsoleRef) console = { 0, };

  context = g_option_context_new ("MOUNT-PATH COLLECTION-ID REF [COLLECTION-ID REF...] - Copy the refs to a USB stick");

  /* Parse options. */
  g_autoptr(OstreeRepo) src_repo = NULL;

  if (!ostree_option_context_parse (context, options, &argc, &argv, OSTREE_BUILTIN_FLAG_NONE, &src_repo, cancellable, error))
    return FALSE;

  if (argc < 2)
    {
      ot_util_usage_error (context, "A MOUNT-PATH must be specified", error);
      return FALSE;
    }

  if (argc < 4)
    {
      ot_util_usage_error (context, "At least one COLLECTION-ID REF pair must be specified", error);
      return FALSE;
    }

  if (argc % 2 == 1)
    {
      ot_util_usage_error (context, "Only complete COLLECTION-ID REF pairs may be specified", error);
      return FALSE;
    }

  /* Open the USB stick, which must exist. Allow automounting and following symlinks. */
  const char *mount_root_path = argv[1];
  struct stat mount_root_stbuf;

  glnx_fd_close int mount_root_dfd = -1;
  if (!glnx_opendirat (AT_FDCWD, mount_root_path, TRUE, &mount_root_dfd, error))
    return FALSE;
  if (!glnx_fstat (mount_root_dfd, &mount_root_stbuf, error))
    return FALSE;

  /* Read in the refs to add to the USB stick. */
  g_autoptr(GPtrArray) refs = g_ptr_array_new_full (argc, (GDestroyNotify) ostree_collection_ref_free);

  for (gsize i = 2; i < argc; i += 2)
    {
      if (!ostree_validate_collection_id (argv[i], error) ||
          !ostree_validate_rev (argv[i + 1], error))
        return FALSE;

      g_ptr_array_add (refs, ostree_collection_ref_new (argv[i], argv[i + 1]));
    }

  /* Open the destination repository on the USB stick or create it if it doesn’t exist.
   * Check it’s below @mount_root_path, and that it’s not the same as the source
   * repository.
   *
   * If the destination file system supports xattrs (for example, ext4), we use
   * a BARE_USER repository; if it doesn’t (for example, FAT), we use ARCHIVE.
   * In either case, we want a lossless repository. */
  const char *dest_repo_path = (opt_destination_repo != NULL) ? opt_destination_repo : ".ostree/repo";

  if (!glnx_shutil_mkdir_p_at (mount_root_dfd, dest_repo_path, 0755, cancellable, error))
    return FALSE;

  OstreeRepoMode mode = OSTREE_REPO_MODE_BARE_USER;

  if (TEMP_FAILURE_RETRY (fgetxattr (mount_root_dfd, "user.test", NULL, 0)) < 0 &&
      errno == ENOTSUP)
    mode = OSTREE_REPO_MODE_ARCHIVE;

  g_debug ("%s: Creating repository in mode %u", G_STRFUNC, mode);

  g_autoptr(OstreeRepo) dest_repo = ostree_repo_create_at (mount_root_dfd, dest_repo_path,
                                                           mode, NULL, cancellable, error);

  if (dest_repo == NULL)
    return FALSE;

  struct stat dest_repo_stbuf;

  if (!glnx_fstat (ostree_repo_get_dfd (dest_repo), &dest_repo_stbuf, error))
    return FALSE;

  if (dest_repo_stbuf.st_dev != mount_root_stbuf.st_dev)
    {
      ot_util_usage_error (context, "--destination-repo must be a descendent of MOUNT-PATH", error);
      return FALSE;
    }

  if (ostree_repo_equal (src_repo, dest_repo))
    {
      ot_util_usage_error (context, "--destination-repo must not be the source repository", error);
      return FALSE;
    }

  if (!ostree_ensure_repo_writable (dest_repo, error))
    return FALSE;

  if (opt_disable_fsync)
    ostree_repo_set_disable_fsync (dest_repo, TRUE);

  /* Copy across all of the collection–refs to the destination repo. */
  GVariantBuilder refs_builder;
  g_variant_builder_init (&refs_builder, G_VARIANT_TYPE ("a(sss)"));

  for (gsize i = 0; i < refs->len; i++)
    {
      const OstreeCollectionRef *ref = g_ptr_array_index (refs, i);

      g_variant_builder_add (&refs_builder, "(sss)",
                             ref->collection_id, ref->ref_name, "");
    }

  {
    GVariantBuilder builder;
    g_autoptr(GVariant) opts = NULL;
    OstreeRepoPullFlags flags = OSTREE_REPO_PULL_FLAGS_MIRROR;

    glnx_console_lock (&console);

    if (console.is_tty)
      progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, &console);

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

    g_variant_builder_add (&builder, "{s@v}", "collection-refs",
                           g_variant_new_variant (g_variant_builder_end (&refs_builder)));
    g_variant_builder_add (&builder, "{s@v}", "flags",
                           g_variant_new_variant (g_variant_new_int32 (flags)));
    g_variant_builder_add (&builder, "{s@v}", "depth",
                           g_variant_new_variant (g_variant_new_int32 (0)));
    opts = g_variant_ref_sink (g_variant_builder_end (&builder));

    g_autofree char *src_repo_uri = g_file_get_uri (ostree_repo_get_path (src_repo));

    if (!ostree_repo_pull_with_options (dest_repo, src_repo_uri,
                                        opts,
                                        progress,
                                        cancellable, error))
      {
        ostree_repo_abort_transaction (dest_repo, cancellable, NULL);
        return FALSE;
      }

    if (progress != NULL)
      ostree_async_progress_finish (progress);
  }

  /* Ensure a summary file is present to make it easier to look up commit checksums. */
  /* FIXME: It should be possible to work without this, but find_remotes_cb() in
   * ostree-repo-pull.c currently assumes a summary file (signed or unsigned) is
   * present. */
  struct stat stbuf;
  if (!glnx_fstatat_allow_noent (ostree_repo_get_dfd (dest_repo), "summary", &stbuf, 0, error))
    return FALSE;
  if (errno == ENOENT &&
      !ostree_repo_regenerate_summary (dest_repo, NULL, cancellable, error))
    return FALSE;

  /* Add the symlinks .ostree/repos.d/@symlink_name → @dest_repo_path, unless
   * the @dest_repo_path is a well-known one like ostree/repo, in which case no
   * symlink is necessary; #OstreeRepoFinderMount always looks there. */
  if (!g_str_equal (dest_repo_path, "ostree/repo") &&
      !g_str_equal (dest_repo_path, ".ostree/repo"))
    {
      if (!glnx_shutil_mkdir_p_at (mount_root_dfd, ".ostree/repos.d", 0755, cancellable, error))
        return FALSE;

      /* Find a unique name for the symlink. If a symlink already targets
       * @dest_repo_path, use that and don’t create a new one. */
      GLnxDirFdIterator repos_iter;
      gboolean need_symlink = TRUE;

      if (!glnx_dirfd_iterator_init_at (mount_root_dfd, ".ostree/repos.d", TRUE, &repos_iter, error))
        return FALSE;

      while (TRUE)
        {
          struct dirent *repo_dent;

          if (!glnx_dirfd_iterator_next_dent (&repos_iter, &repo_dent, cancellable, error))
            return FALSE;

          if (repo_dent == NULL)
            break;

          /* Does the symlink already point to this repository? (Or is the
           * repository itself present in repos.d?) We already guarantee that
           * they’re on the same device. */
          if (repo_dent->d_ino == dest_repo_stbuf.st_ino)
            {
              need_symlink = FALSE;
              break;
            }
        }

      /* If we need a symlink, find a unique name for it and create it. */
      if (need_symlink)
        {
          /* Relative to .ostree/repos.d. */
          g_autofree char *relative_dest_repo_path = g_build_filename ("..", "..", dest_repo_path, NULL);
          guint i;
          const guint max_attempts = 100;

          for (i = 0; i < max_attempts; i++)
            {
              g_autofree char *symlink_path = g_strdup_printf (".ostree/repos.d/%02u-generated", i);

              int ret = TEMP_FAILURE_RETRY (symlinkat (relative_dest_repo_path, mount_root_dfd, symlink_path));
              if (ret < 0 && errno != EEXIST)
                return glnx_throw_errno_prefix (error, "symlinkat(%s → %s)", symlink_path, relative_dest_repo_path);
              else if (ret >= 0)
                break;
            }

          if (i == max_attempts)
            return glnx_throw (error, "Could not find an unused symlink name for the repository");
        }
    }

  /* Report success to the user. */
  g_autofree char *src_repo_path = g_file_get_path (ostree_repo_get_path (src_repo));

  g_print ("Copied %u/%u refs successfully from ‘%s’ to ‘%s’ repository in ‘%s’.\n", refs->len, refs->len,
           src_repo_path, dest_repo_path, mount_root_path);

  return TRUE;
}
