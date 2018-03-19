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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-main.h"
#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"
#include "libglnx.h"

#include <glib/gi18n.h>

static GOptionEntry options[] = {
  { NULL }
};

static gboolean
deployment_get_gpg_verify (OstreeDeployment *deployment,
                           OstreeRepo *repo)
{
  /* XXX Something like this could be added to the OstreeDeployment
   *     API in libostree if the OstreeRepo parameter is acceptable. */
  GKeyFile *origin = ostree_deployment_get_origin (deployment);

  if (origin == NULL)
    return FALSE;

  g_autofree char *refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);

  if (refspec == NULL)
    return FALSE;

  g_autofree char *remote = NULL;
  if (!ostree_parse_refspec (refspec, &remote, NULL, NULL))
    return FALSE;

  gboolean gpg_verify = FALSE;
  if (remote)
    (void) ostree_repo_remote_get_gpg_verify (repo, remote, &gpg_verify, NULL);

  return gpg_verify;
}


static gboolean
deployment_print_status (OstreeSysroot    *sysroot,
                         OstreeRepo       *repo,
                         OstreeDeployment *deployment,
                         gboolean          is_booted,
                         gboolean          is_pending,
                         gboolean          is_rollback,
                         GCancellable     *cancellable,
                         GError          **error)
{
  const char *ref = ostree_deployment_get_csum (deployment);

  /* Load the backing commit; shouldn't normally fail, but if it does,
   * we stumble on.
   */
  g_autoptr(GVariant) commit = NULL;
  (void)ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, ref,
                                  &commit, NULL);
  g_autoptr(GVariant) commit_metadata = NULL;
  if (commit)
    commit_metadata = g_variant_get_child_value (commit, 0);

  const char *version = NULL;
  const char *source_title = NULL;
  if (commit_metadata)
    {
      (void) g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_VERSION, "&s", &version);
      (void) g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_SOURCE_TITLE, "&s", &source_title);
    }

  GKeyFile *origin = ostree_deployment_get_origin (deployment);

  const char *deployment_status = "";
  if (is_pending)
    deployment_status = " (pending)";
  else if (is_rollback)
    deployment_status = " (rollback)";
  g_print ("%c %s %s.%d%s\n",
           is_booted ? '*' : ' ',
           ostree_deployment_get_osname (deployment),
           ostree_deployment_get_csum (deployment),
           ostree_deployment_get_deployserial (deployment),
           deployment_status);
  if (version)
    g_print ("    Version: %s\n", version);

  OstreeDeploymentUnlockedState unlocked = ostree_deployment_get_unlocked (deployment);
  switch (unlocked)
    {
    case OSTREE_DEPLOYMENT_UNLOCKED_NONE:
      break;
    default:
      g_print ("    %s%sUnlocked: %s%s%s\n", ot_get_red_start (), ot_get_bold_start (),
               ostree_deployment_unlocked_state_to_string (unlocked),
               ot_get_bold_end (), ot_get_red_end ());
    }
  if (ostree_deployment_is_pinned (deployment))
    g_print ("    Pinned: yes\n");
  if (!origin)
    g_print ("    origin: none\n");
  else
    {
      g_autofree char *origin_refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);
      if (!origin_refspec)
        g_print ("    origin: <unknown origin type>\n");
      else
        g_print ("    origin refspec: %s\n", origin_refspec);
      if (source_title)
        g_print ("    `- %s\n", source_title);
    }

  if (deployment_get_gpg_verify (deployment, repo))
    {
      g_autoptr(GString) output_buffer = g_string_sized_new (256);
      /* Print any digital signatures on this commit. */

      g_autoptr(GError) local_error = NULL;
      g_autoptr(OstreeGpgVerifyResult) result =
        ostree_repo_verify_commit_ext (repo, ref, NULL, NULL,
                                       cancellable, &local_error);

      /* G_IO_ERROR_NOT_FOUND just means the commit is not signed. */
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&local_error);
          return TRUE;
        }
      else if (local_error != NULL)
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }

      const guint n_signatures = ostree_gpg_verify_result_count_all (result);
      for (guint jj = 0; jj < n_signatures; jj++)
        {
          ostree_gpg_verify_result_describe (result, jj, output_buffer, "    GPG: ",
                                             OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);
        }

      g_print ("%s", output_buffer->str);
    }

  return TRUE;
}

gboolean
ot_admin_builtin_status (int argc, char **argv, OstreeCommandInvocation *invocation, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = g_option_context_new ("");

  g_autoptr(OstreeSysroot) sysroot = NULL;
  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED,
                                          invocation, &sysroot, cancellable, error))
    return FALSE;

  g_autoptr(OstreeRepo) repo = NULL;
  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    return FALSE;

  g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments (sysroot);
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  g_autoptr(OstreeDeployment) pending_deployment = NULL;
  g_autoptr(OstreeDeployment) rollback_deployment = NULL;
  if (booted_deployment)
    ostree_sysroot_query_deployments_for (sysroot, NULL, &pending_deployment,
                                          &rollback_deployment);

  if (deployments->len == 0)
    {
      g_print ("No deployments.\n");
    }
  else
    {
      for (guint i = 0; i < deployments->len; i++)
        {
          OstreeDeployment *deployment = deployments->pdata[i];
          if (!deployment_print_status (sysroot, repo, deployment,
                                        deployment == booted_deployment,
                                        deployment == pending_deployment,
                                        deployment == rollback_deployment,
                                        cancellable,
                                        error))
            return FALSE;
        }
    }

  return TRUE;
}
