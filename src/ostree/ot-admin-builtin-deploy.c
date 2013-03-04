/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
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
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"

#include <glib/gi18n.h>

typedef struct {
  OstreeRepo  *repo;
  OtAdminBuiltinOpts *admin_opts;
  GFile *ostree_dir;
  char  *osname;
  GFile *osname_dir;
} OtAdminDeploy;

static gboolean opt_no_kernel;
static gboolean opt_force;

static GOptionEntry options[] = {
  { "no-kernel", 0, 0, G_OPTION_ARG_NONE, &opt_no_kernel, "Don't update kernel related config (initramfs, bootloader)", NULL },
  { "force", 0, 0, G_OPTION_ARG_NONE, &opt_force, "Overwrite any existing deployment", NULL },
  { NULL }
};

/**
 * update_current:
 *
 * Atomically swap the /ostree/current symbolic link to point to a new
 * path.  If successful, the old current will be saved as
 * /ostree/previous, and /ostree/current-etc will be a link to the
 * current /etc subdirectory.
 *
 * Unless the new-current equals current, in which case, do nothing.
 */
static gboolean
update_current (OtAdminDeploy      *self,
                GFile              *current_deployment,
                GFile              *deploy_target,
                GCancellable       *cancellable,
                GError            **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *current_path = NULL;
  ot_lobj GFile *current_etc_path = NULL;
  ot_lobj GFile *previous_path = NULL;
  ot_lobj GFile *tmp_current_path = NULL;
  ot_lobj GFile *tmp_current_etc_path = NULL;
  ot_lobj GFile *tmp_previous_path = NULL;
  ot_lobj GFileInfo *previous_info = NULL;
  ot_lfree char *relative_current = NULL;
  ot_lfree char *relative_current_etc = NULL;
  ot_lfree char *relative_previous = NULL;

  current_path = g_file_get_child (self->osname_dir, "current");
  current_etc_path = g_file_get_child (self->osname_dir, "current-etc");
  previous_path = g_file_get_child (self->osname_dir, "previous");

  relative_current = g_file_get_relative_path (self->osname_dir, deploy_target);
  g_assert (relative_current);
  relative_current_etc = g_strconcat (relative_current, "-etc", NULL);

  if (current_deployment)
    {
      ot_lfree char *relative_previous = NULL;

      if (g_file_equal (current_deployment, deploy_target))
        {
          g_print ("ostadmin: %s already points to %s\n", gs_file_get_path_cached (current_path),
                   relative_current);
          return TRUE;
        }

      tmp_previous_path = g_file_get_child (self->osname_dir, "tmp-previous");
      (void) gs_file_unlink (tmp_previous_path, NULL, NULL);

      relative_previous = g_file_get_relative_path (self->osname_dir, current_deployment);
      g_assert (relative_previous);
      if (symlink (relative_previous, gs_file_get_path_cached (tmp_previous_path)) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  tmp_current_path = g_file_get_child (self->osname_dir, "tmp-current");
  (void) gs_file_unlink (tmp_current_path, NULL, NULL);

  if (symlink (relative_current, gs_file_get_path_cached (tmp_current_path)) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  tmp_current_etc_path = g_file_get_child (self->osname_dir, "tmp-current-etc");
  (void) gs_file_unlink (tmp_current_etc_path, NULL, NULL);
  if (symlink (relative_current_etc, gs_file_get_path_cached (tmp_current_etc_path)) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  if (!gs_file_rename (tmp_current_path, current_path,
                       cancellable, error))
    goto out;
  if (!gs_file_rename (tmp_current_etc_path, current_etc_path,
                       cancellable, error))
    goto out;

  if (tmp_previous_path)
    {
      if (!gs_file_rename (tmp_previous_path, previous_path,
                           cancellable, error))
        goto out;
    }

  g_print ("ostadmin: %s set to %s\n", gs_file_get_path_cached (current_path),
           relative_current);

  ret = TRUE;
 out:
  return ret;
}

typedef struct {
  GError **error;
  gboolean caught_error;

  GMainLoop *loop;
} ProcessOneCheckoutData;

static void
on_checkout_complete (GObject         *object,
                      GAsyncResult    *result,
                      gpointer         user_data)
{
  ProcessOneCheckoutData *data = user_data;
  GError *local_error = NULL;

  if (!ostree_repo_checkout_tree_finish ((OstreeRepo*)object, result,
                                         &local_error))
    goto out;

 out:
  if (local_error)
    {
      data->caught_error = TRUE;
      g_propagate_error (data->error, local_error);
    }
  g_main_loop_quit (data->loop);
}


/**
 * ensure_unlinked:
 *
 * Like gs_file_unlink(), but return successfully if the file doesn't
 * exist.
 */
static gboolean
ensure_unlinked (GFile         *path,
                 GCancellable  *cancellable,
                 GError       **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;

  if (!gs_file_unlink (path, cancellable, &temp_error))
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }
  
  ret = TRUE;
 out:
  return ret;
}

/**
 * copy_one_config_file:
 *
 * Copy @file from @modified_etc to @new_etc, overwriting any existing
 * file there.
 */
static gboolean
copy_one_config_file (OtAdminDeploy      *self,
                      GFile              *orig_etc,
                      GFile              *modified_etc,
                      GFile              *new_etc,
                      GFile              *src,
                      GCancellable       *cancellable,
                      GError            **error)
{
  gboolean ret = FALSE;
  ot_lobj GFileInfo *src_info = NULL;
  ot_lobj GFile *dest = NULL;
  ot_lobj GFile *parent = NULL;
  ot_lfree char *relative_path = NULL;
  
  relative_path = g_file_get_relative_path (modified_etc, src);
  g_assert (relative_path);
  dest = g_file_resolve_relative_path (new_etc, relative_path);

  src_info = g_file_query_info (src, OSTREE_GIO_FAST_QUERYINFO, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                cancellable, error);
  if (!src_info)
    goto out;

  if (g_file_info_get_file_type (src_info) == G_FILE_TYPE_DIRECTORY)
    {
      ot_lobj GFileEnumerator *src_enum = NULL;
      ot_lobj GFileInfo *child_info = NULL;
      GError *temp_error = NULL;

      /* FIXME actually we need to copy permissions and xattrs */
      if (!gs_file_ensure_directory (dest, TRUE, cancellable, error))
        goto out;

      src_enum = g_file_enumerate_children (src, OSTREE_GIO_FAST_QUERYINFO,
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            cancellable, error);

      while ((child_info = g_file_enumerator_next_file (src_enum, cancellable, error)) != NULL)
        {
          ot_lobj GFile *child = g_file_get_child (src, g_file_info_get_name (child_info));

          if (!copy_one_config_file (self, orig_etc, modified_etc, new_etc, child,
                                     cancellable, error))
            goto out;
        }
      g_clear_object (&child_info);
      if (temp_error != NULL)
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }
  else
    {
      parent = g_file_get_parent (dest);

      /* FIXME actually we need to copy permissions and xattrs */
      if (!gs_file_ensure_directory (parent, TRUE, cancellable, error))
        goto out;
      
      if (!g_file_copy (src, dest, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA,
                        cancellable, NULL, NULL, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * merge_etc_changes:
 *
 * Compute the difference between @orig_etc and @modified_etc,
 * and apply that to @new_etc.
 *
 * The algorithm for computing the difference is pretty simple; it's
 * approximately equivalent to "diff -unR orig_etc modified_etc",
 * except that rather than attempting a 3-way merge if a file is also
 * changed in @new_etc, the modified version always wins.
 */
static gboolean
merge_etc_changes (OtAdminDeploy  *self,
                   GFile          *orig_etc,
                   GFile          *modified_etc,
                   GFile          *new_etc,
                   GCancellable   *cancellable,
                   GError        **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *ostree_etc = NULL;
  ot_lobj GFile *tmp_etc = NULL;
  ot_lptrarray GPtrArray *modified = NULL;
  ot_lptrarray GPtrArray *removed = NULL;
  ot_lptrarray GPtrArray *added = NULL;
  guint i;

  modified = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_diff_item_unref);
  removed = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  added = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

  if (!ostree_diff_dirs (orig_etc, modified_etc, modified, removed, added,
                         cancellable, error))
    {
      g_prefix_error (error, "While computing configuration diff: ");
      goto out;
    }

  if (modified->len > 0 || removed->len > 0 || added->len > 0)
    g_print ("ostadmin: Processing config: %u modified, %u removed, %u added\n", 
             modified->len,
             removed->len,
             added->len);
  else
    g_print ("ostadmin: No modified configuration\n");

  for (i = 0; i < removed->len; i++)
    {
      GFile *file = removed->pdata[i];
      ot_lobj GFile *target_file = NULL;
      ot_lfree char *path = NULL;

      path = g_file_get_relative_path (orig_etc, file);
      g_assert (path);
      target_file = g_file_resolve_relative_path (new_etc, path);

      if (!ensure_unlinked (target_file, cancellable, error))
        goto out;
    }

  for (i = 0; i < modified->len; i++)
    {
      OstreeDiffItem *diff = modified->pdata[i];

      if (!copy_one_config_file (self, orig_etc, modified_etc, new_etc, diff->target,
                                 cancellable, error))
        goto out;
    }
  for (i = 0; i < added->len; i++)
    {
      GFile *file = added->pdata[i];

      if (!copy_one_config_file (self, orig_etc, modified_etc, new_etc, file,
                                 cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * deploy_tree:
 *
 * Look up @revision in the repository, and check it out in
 * OSTREE_DIR/deploy/OS/DEPLOY_TARGET.
 *
 * Merge configuration changes from the old deployment, if any.
 *
 * Update the OSTREE_DIR/current{,-etc} and OSTREE_DIR/previous symbolic
 * links.
 */
static gboolean
deploy_tree (OtAdminDeploy     *self,
             const char        *deploy_target,
             const char        *revision,
             GFile            **out_deploy_dir,           
             GCancellable      *cancellable,
             GError           **error)
{
  gboolean ret = FALSE;
  gs_free char *current_deployment_ref = NULL;
  gs_free char *previous_deployment_ref = NULL;
  ot_lfree char *deploy_target_fullname = NULL;
  ot_lfree char *deploy_target_fullname_tmp = NULL;
  ot_lobj GFile *deploy_target_path = NULL;
  ot_lobj GFile *deploy_target_path_tmp = NULL;
  ot_lfree char *deploy_target_etc_name = NULL;
  ot_lobj GFile *deploy_target_etc_path = NULL;
  ot_lobj GFile *deploy_target_default_etc_path = NULL;
  ot_lobj GFile *deploy_parent = NULL;
  ot_lobj GFile *previous_deployment = NULL;
  ot_lfree char *previous_deployment_revision = NULL;
  ot_lobj GFile *previous_deployment_etc = NULL;
  ot_lobj GFile *previous_deployment_etc_default = NULL;
  ot_lobj OstreeRepoFile *root = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GFileInfo *existing_checkout_info = NULL;
  ot_lfree char *checkout_target_name = NULL;
  ot_lfree char *checkout_target_tmp_name = NULL;
  ot_lfree char *resolved_commit = NULL;
  gs_free char *resolved_previous_commit = NULL;
  GError *temp_error = NULL;
  gboolean skip_checkout;

  if (!revision)
    revision = deploy_target;

  current_deployment_ref = g_strdup_printf ("deployment/%s/current", self->osname);
  previous_deployment_ref = g_strdup_printf ("deployment/%s/previous", self->osname);

  if (!g_file_query_exists (self->osname_dir, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No OS \"%s\" found in \"%s\"", self->osname,
                   gs_file_get_path_cached (self->osname_dir));
      goto out;
    }

  if (!ostree_repo_resolve_rev (self->repo, revision, FALSE, &resolved_commit, error))
    goto out;
  if (!ostree_repo_resolve_rev (self->repo, revision, TRUE, &resolved_previous_commit, error))
    goto out;

  root = (OstreeRepoFile*)ostree_repo_file_new_root (self->repo, resolved_commit);
  if (!ostree_repo_file_ensure_resolved (root, error))
    goto out;

  file_info = g_file_query_info ((GFile*)root, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!file_info)
    goto out;

  deploy_target_fullname = g_strconcat (deploy_target, "-", resolved_commit, NULL);
  deploy_target_path = g_file_resolve_relative_path (self->osname_dir, deploy_target_fullname);

  deploy_target_fullname_tmp = g_strconcat (deploy_target_fullname, ".tmp", NULL);
  deploy_target_path_tmp = g_file_resolve_relative_path (self->osname_dir, deploy_target_fullname_tmp);

  deploy_parent = g_file_get_parent (deploy_target_path);
  if (!gs_file_ensure_directory (deploy_parent, TRUE, cancellable, error))
    goto out;

  deploy_target_etc_name = g_strconcat (deploy_target, "-", resolved_commit, "-etc", NULL);
  deploy_target_etc_path = g_file_resolve_relative_path (self->osname_dir, deploy_target_etc_name);

  /* Delete any previous temporary data */
  if (!gs_shutil_rm_rf (deploy_target_path_tmp, cancellable, error))
    goto out;

  existing_checkout_info = g_file_query_info (deploy_target_path, OSTREE_GIO_FAST_QUERYINFO,
                                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                              cancellable, &temp_error);
  if (existing_checkout_info)
    {
      if (opt_force)
        {
          if (!gs_shutil_rm_rf (deploy_target_path, cancellable, error))
            goto out;
          if (!gs_shutil_rm_rf (deploy_target_etc_path, cancellable, error))
            goto out;
          
          skip_checkout = FALSE;
        }
      else
        skip_checkout = TRUE;
    }
  else if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_clear_error (&temp_error);
      skip_checkout = FALSE;
    }
  else
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  if (!ot_admin_get_current_deployment (self->ostree_dir, self->osname, &previous_deployment,
                                        cancellable, error))
    goto out;
  if (previous_deployment)
    {
      ot_lfree char *etc_name;
      ot_lobj GFile *parent;

      etc_name = g_strconcat (gs_file_get_basename_cached (previous_deployment), "-etc", NULL);
      parent = g_file_get_parent (previous_deployment);

      previous_deployment_etc = g_file_get_child (parent, etc_name);

      if (!g_file_query_exists (previous_deployment_etc, cancellable)
          || g_file_equal (previous_deployment, deploy_target_path))
        g_clear_object (&previous_deployment_etc);
      else
        previous_deployment_etc_default = g_file_get_child (previous_deployment, "etc");

      if (!ostree_repo_resolve_rev (self->repo, current_deployment_ref, TRUE,
                                    &previous_deployment_revision, error))
        goto out;
    }


  if (!skip_checkout)
    {
      ProcessOneCheckoutData checkout_data;
      ot_lobj GFile *triggers_run_path = NULL;

      g_print ("ostadmin: Creating deployment %s\n",
               gs_file_get_path_cached (deploy_target_path));

      memset (&checkout_data, 0, sizeof (checkout_data));
      checkout_data.loop = g_main_loop_new (NULL, TRUE);
      checkout_data.error = error;

      ostree_repo_checkout_tree_async (self->repo, 0, 0, deploy_target_path_tmp, root,
                                       file_info, cancellable,
                                       on_checkout_complete, &checkout_data);

      g_main_loop_run (checkout_data.loop);

      g_main_loop_unref (checkout_data.loop);

      if (checkout_data.caught_error)
        goto out;

      triggers_run_path = g_file_resolve_relative_path (deploy_target_path_tmp, "usr/share/ostree/triggers-run");

      if (!g_file_query_exists (triggers_run_path, NULL))
        {
          if (!ostree_run_triggers_in_root (deploy_target_path_tmp, cancellable, error))
            goto out;
        }

      deploy_target_default_etc_path = ot_gfile_get_child_strconcat (deploy_target_path_tmp, "etc", NULL);

      if (!gs_shutil_rm_rf (deploy_target_etc_path, cancellable, error))
        goto out;

      if (!gs_shutil_cp_a (deploy_target_default_etc_path, deploy_target_etc_path,
                           cancellable, error))
        goto out;

      g_print ("ostadmin: Created %s\n", gs_file_get_path_cached (deploy_target_etc_path));

      if (previous_deployment_etc)
        {
          if (!merge_etc_changes (self, previous_deployment_etc_default,
                                  previous_deployment_etc, deploy_target_etc_path, 
                                  cancellable, error))
            goto out;
        }
      else
        g_print ("ostadmin: No previous deployment; therefore, no configuration changes to merge\n");

      if (!gs_file_rename (deploy_target_path_tmp, deploy_target_path,
                           cancellable, error))
        goto out;
    }

  /* Write out a ref so that any "ostree prune" on the raw repo
   * doesn't GC the currently deployed tree.
   */
  if (!ostree_repo_write_ref (self->repo, NULL, current_deployment_ref,
                              resolved_commit, error))
    goto out;
  /* Only overwrite previous if it's different from what we're deploying now.
   */
  if (resolved_previous_commit != NULL
      && strcmp (resolved_previous_commit, resolved_commit) != 0)
    {
      if (!ostree_repo_write_ref (self->repo, NULL, previous_deployment_ref,
                                  previous_deployment_revision, error))
        goto out;
    }

  if (!update_current (self, previous_deployment, deploy_target_path,
                       cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_deploy_dir, &deploy_target_path);
 out:
  return ret;
}

/**
 * do_update_kernel:
 *
 * Ensure we have a GRUB entry, initramfs set up, etc.
 */
static gboolean
do_update_kernel (OtAdminDeploy     *self,
                  GFile             *deploy_path,
                  GCancellable      *cancellable,
                  GError           **error)
{
  gboolean ret = FALSE;
  gs_unref_object GSSubprocess *proc = NULL;
  gs_unref_ptrarray GPtrArray *args = NULL;

  args = g_ptr_array_new ();
  ot_ptrarray_add_many (args, "ostree", "admin",
                        "--ostree-dir", gs_file_get_path_cached (self->ostree_dir),
                        "--boot-dir", gs_file_get_path_cached (self->admin_opts->boot_dir),
                        "update-kernel",
                        self->osname,
                        gs_file_get_path_cached (deploy_path), NULL);
  g_ptr_array_add (args, NULL);

  proc = gs_subprocess_new_simple_argv ((char**)args->pdata,
                                        GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                        GS_SUBPROCESS_STREAM_DISPOSITION_INHERIT,
                                        cancellable, error);
  if (!proc)
    goto out;
  if (!gs_subprocess_wait_sync_check (proc, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
ot_admin_builtin_deploy (int argc, char **argv, OtAdminBuiltinOpts *admin_opts, GError **error)
{
  GOptionContext *context;
  OtAdminDeploy self_data;
  OtAdminDeploy *self = &self_data;
  gboolean ret = FALSE;
  ot_lobj GFile *repo_path = NULL;
  ot_lobj GFile *deploy_path = NULL;
  const char *osname = NULL;
  const char *deploy_target = NULL;
  const char *revision = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  memset (self, 0, sizeof (*self));

  context = g_option_context_new ("OSNAME TREENAME [REVISION] - In operating system OS, check out revision TREENAME (or REVISION as TREENAME)");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 3)
    {
      ot_util_usage_error (context, "OSNAME and TREENAME must be specified", error);
      goto out;
    }

  self->admin_opts = admin_opts;
  self->ostree_dir = g_object_ref (admin_opts->ostree_dir);

  if (!ot_admin_ensure_initialized (self->ostree_dir, cancellable, error))
    goto out;

  repo_path = g_file_get_child (self->ostree_dir, "repo");
  self->repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (self->repo, error))
    goto out;

  osname = argv[1];
  deploy_target = argv[2];
  if (argc > 3)
    revision = argv[3];

  self->osname = g_strdup (osname);
  self->osname_dir = ot_gfile_get_child_build_path (self->ostree_dir, "deploy", osname, NULL);
  if (!deploy_tree (self, deploy_target, revision, &deploy_path,
                    cancellable, error))
    goto out;

  if (!opt_no_kernel)
    {
      if (!do_update_kernel (self, deploy_path, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  g_clear_object (&self->repo);
  g_free (self->osname);
  g_clear_object (&self->ostree_dir);
  g_clear_object (&self->osname_dir);
  if (context)
    g_option_context_free (context);
  return ret;
}
