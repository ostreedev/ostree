/*
 * Copyright (C) 2012,2013 Colin Walters <walters@verbum.org>
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "libglnx.h"
#include "ostree.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ul-jsonwrt.h"

#include <glib/gi18n.h>

static gboolean opt_verify;
static gboolean opt_skip_signatures;
static gboolean opt_is_default;
static gboolean opt_json;

static GOptionEntry options[]
    = { { "verify", 'V', 0, G_OPTION_ARG_NONE, &opt_verify, "Print the commit verification status",
          NULL },
        { "json", 'J', 0, G_OPTION_ARG_NONE, &opt_json, "Emit JSON", NULL },
        { "skip-signatures", 'S', 0, G_OPTION_ARG_NONE, &opt_skip_signatures,
          "Skip signatures in output", NULL },
        { "is-default", 'D', 0, G_OPTION_ARG_NONE, &opt_is_default,
          "Output \"default\" if booted into the default deployment, otherwise \"not-default\"",
          NULL },
        { NULL } };

static gboolean
deployment_print_status (OstreeSysroot *sysroot, OstreeRepo *repo, OstreeDeployment *deployment,
                         gboolean is_booted, gboolean is_pending, gboolean is_rollback,
                         GCancellable *cancellable, GError **error)
{
  const char *ref = ostree_deployment_get_csum (deployment);

  /* Load the backing commit; shouldn't normally fail, but if it does,
   * we stumble on.
   */
  g_autoptr (GVariant) commit = NULL;
  (void)ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, ref, &commit, NULL);
  g_autoptr (GVariant) commit_metadata = NULL;
  if (commit)
    commit_metadata = g_variant_get_child_value (commit, 0);
  g_autoptr (GVariant) commit_detached_metadata = NULL;
  if (commit)
    {
      if (!ostree_repo_read_commit_detached_metadata (repo, ref, &commit_detached_metadata,
                                                      cancellable, error))
        return FALSE;
    }

  const char *version = NULL;
  const char *source_title = NULL;
  if (commit_metadata)
    {
      (void)g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_VERSION, "&s", &version);
      (void)g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_SOURCE_TITLE, "&s",
                              &source_title);
    }

  GKeyFile *origin = ostree_deployment_get_origin (deployment);
  g_autofree char *origin_refspec
      = origin ? g_key_file_get_string (origin, "origin", "refspec", NULL) : NULL;

  g_autoptr (GString) deployment_status = g_string_new ("");

  if (ostree_deployment_is_finalization_locked (deployment))
    g_string_append (deployment_status, " (finalization locked)");
  else if (ostree_deployment_is_staged (deployment))
    g_string_append (deployment_status, " (staged)");
  else if (is_pending)
    g_string_append (deployment_status, " (pending)");
  else if (is_rollback)
    g_string_append (deployment_status, " (rollback)");

  if (ostree_deployment_is_soft_reboot_target (deployment))
    g_string_append (deployment_status, " (soft-reboot)");

  char deployment_marker = is_booted ? '*' : ' ';
  g_print ("%c %s %s.%d%s\n", deployment_marker, ostree_deployment_get_osname (deployment),
           ostree_deployment_get_csum (deployment), ostree_deployment_get_deployserial (deployment),
           deployment_status->str);
  if (version)
    g_print ("    Version: %s\n", version);

  OstreeDeploymentUnlockedState unlocked = ostree_deployment_get_unlocked (deployment);
  switch (unlocked)
    {
    case OSTREE_DEPLOYMENT_UNLOCKED_NONE:
      break;
    default:
      g_print ("    %s%sUnlocked: %s%s%s\n", ot_get_red_start (), ot_get_bold_start (),
               ostree_deployment_unlocked_state_to_string (unlocked), ot_get_bold_end (),
               ot_get_red_end ());
    }
  if (ostree_deployment_is_pinned (deployment))
    g_print ("    Pinned: yes\n");
  if (!origin)
    g_print ("    origin: none\n");
  else
    {
      if (!origin_refspec)
        g_print ("    origin: <unknown origin type>\n");
      else
        g_print ("    origin refspec: %s\n", origin_refspec);
      if (source_title)
        g_print ("    `- %s\n", source_title);
    }

#ifndef OSTREE_DISABLE_GPGME
  g_autofree char *remote = NULL;
  if (origin_refspec && !ostree_parse_refspec (origin_refspec, &remote, NULL, NULL))
    return FALSE;

  gboolean gpg_verify = FALSE;
  if (remote)
    (void)ostree_repo_remote_get_gpg_verify (repo, remote, &gpg_verify, NULL);
  if (!opt_skip_signatures && !opt_verify && gpg_verify)
    {
      g_assert (remote);
      g_autoptr (GString) output_buffer = g_string_sized_new (256);
      /* Print any digital signatures on this commit. */

      g_autoptr (GError) local_error = NULL;
      g_autoptr (OstreeGpgVerifyResult) result
          = ostree_repo_verify_commit_for_remote (repo, ref, remote, cancellable, &local_error);

      /* G_IO_ERROR_NOT_FOUND just means the commit is not signed. */
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&local_error);
          return TRUE;
        }
      else if (local_error != NULL)
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return glnx_prefix_error (error, "Deployment %i",
                                    ostree_deployment_get_index (deployment));
        }

      const guint n_signatures = ostree_gpg_verify_result_count_all (result);
      for (guint jj = 0; jj < n_signatures; jj++)
        {
          ostree_gpg_verify_result_describe (result, jj, output_buffer,
                                             "    GPG: ", OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);
        }

      g_print ("%s", output_buffer->str);
    }
#else
  g_autofree char *remote = NULL;
#endif /* OSTREE_DISABLE_GPGME */
  if (opt_verify)
    {
      if (!commit)
        return glnx_throw (error, "Cannot verify, failed to load commit");
      if (origin_refspec == NULL)
        return glnx_throw (error, "No origin/refspec, cannot verify");
      if (remote == NULL)
        return glnx_throw (error, "Cannot verify deployment without remote");

      g_autoptr (GBytes) commit_data = g_variant_get_data_as_bytes (commit);
      g_autoptr (GBytes) commit_detached_metadata_bytes
          = commit_detached_metadata ? g_variant_get_data_as_bytes (commit_detached_metadata)
                                     : NULL;
      g_autofree char *verify_text = NULL;
      if (!ostree_repo_signature_verify_commit_data (
              repo, remote, commit_data, commit_detached_metadata_bytes, 0, &verify_text, error))
        return FALSE;
      g_print ("%s\n", verify_text);
    }

  return TRUE;
}

static gboolean
deployment_write_json (OstreeSysroot *sysroot, OstreeRepo *repo, OstreeDeployment *deployment,
                       gboolean is_booted, gboolean is_pending, gboolean is_rollback,
                       struct ul_jsonwrt *jo, GCancellable *cancellable, GError **error)
{
  ul_jsonwrt_object_open (jo, NULL);

  const char *ref = ostree_deployment_get_csum (deployment);
  ul_jsonwrt_value_s (jo, "checksum", ref);
  ul_jsonwrt_value_s (jo, "stateroot", ostree_deployment_get_osname (deployment));
  ul_jsonwrt_value_u64 (jo, "serial", ostree_deployment_get_deployserial (deployment));
  ul_jsonwrt_value_u64 (jo, "index", ostree_deployment_get_index (deployment));
  ul_jsonwrt_value_boolean (jo, "booted", is_booted);
  ul_jsonwrt_value_boolean (jo, "pending", is_pending);
  ul_jsonwrt_value_boolean (jo, "rollback", is_rollback);
  ul_jsonwrt_value_boolean (jo, "finalization-locked",
                            ostree_deployment_is_finalization_locked (deployment));
  ul_jsonwrt_value_boolean (jo, "soft-reboot-target",
                            ostree_deployment_is_soft_reboot_target (deployment));
  ul_jsonwrt_value_boolean (jo, "staged", ostree_deployment_is_staged (deployment));
  ul_jsonwrt_value_boolean (jo, "pinned", ostree_deployment_is_pinned (deployment));
  OstreeDeploymentUnlockedState unlocked = ostree_deployment_get_unlocked (deployment);
  ul_jsonwrt_value_s (jo, "unlocked", ostree_deployment_unlocked_state_to_string (unlocked));

  g_autoptr (GVariant) commit = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, ref, &commit, error))
    return FALSE;
  g_autoptr (GVariant) commit_metadata = g_variant_get_child_value (commit, 0);
  const char *version = NULL;
  const char *source_title = NULL;
  (void)g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_VERSION, "&s", &version);
  (void)g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_SOURCE_TITLE, "&s",
                          &source_title);
  if (version)
    ul_jsonwrt_value_s (jo, "version", version);
  if (source_title)
    ul_jsonwrt_value_s (jo, "source-title", source_title);

  ul_jsonwrt_object_close (jo);
  return TRUE;
}

gboolean
ot_admin_builtin_status (int argc, char **argv, OstreeCommandInvocation *invocation,
                         GCancellable *cancellable, GError **error)
{
  g_autoptr (GOptionContext) context = g_option_context_new ("");

  g_autoptr (OstreeSysroot) sysroot = NULL;
  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED, invocation, &sysroot,
                                          cancellable, error))
    return FALSE;

  g_autoptr (OstreeRepo) repo = NULL;
  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    return FALSE;

  g_autoptr (GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  g_autoptr (OstreeDeployment) pending_deployment = NULL;
  g_autoptr (OstreeDeployment) rollback_deployment = NULL;
  if (booted_deployment)
    ostree_sysroot_query_deployments_for (sysroot, NULL, &pending_deployment, &rollback_deployment);

  if (opt_json)
    {
      struct ul_jsonwrt jo_buf;
      ul_jsonwrt_init (&jo_buf, stdout, 0);
      struct ul_jsonwrt *jo = &jo_buf;
      ul_jsonwrt_root_open (jo);
      ul_jsonwrt_array_open (jo, "deployments");
      for (guint i = 0; i < deployments->len; i++)
        {
          OstreeDeployment *deployment = deployments->pdata[i];
          if (!deployment_write_json (sysroot, repo, deployment, deployment == booted_deployment,
                                      deployment == pending_deployment,
                                      deployment == rollback_deployment, jo, cancellable, error))
            return FALSE;
        }
      ul_jsonwrt_array_close (jo);
      ul_jsonwrt_root_close (jo);
      return TRUE;
    }

  if (opt_is_default)
    {
      if (deployments->len == 0)
        return glnx_throw (error, "Not in a booted OSTree system");

      const gboolean is_default_booted = deployments->pdata[0] == booted_deployment;
      g_print ("%s\n", is_default_booted ? "default" : "not-default");
    }
  else if (deployments->len == 0)
    {
      g_print ("No deployments.\n");
    }
  else
    {
      for (guint i = 0; i < deployments->len; i++)
        {
          OstreeDeployment *deployment = deployments->pdata[i];
          if (!deployment_print_status (sysroot, repo, deployment, deployment == booted_deployment,
                                        deployment == pending_deployment,
                                        deployment == rollback_deployment, cancellable, error))
            return FALSE;
        }
    }

  return TRUE;
}
