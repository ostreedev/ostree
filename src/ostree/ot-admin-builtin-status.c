/*
 * Copyright (C) 2012,2013 Colin Walters <walters@verbum.org>
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

#include <glib/gi18n.h>

/* ATTENTION:
 * Please remember to update the bash-completion script (bash/ostree) and
 * man page (man/ostree-admin-status.xml) when changing the option list.
 */

static GOptionEntry options[] = {
  { NULL }
};

static char *
version_of_commit (OstreeRepo *repo, const char *checksum)
{
  g_autoptr(GVariant) variant = NULL;
  
  /* Shouldn't fail, but if it does, we ignore it */
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                 &variant, NULL))
    goto out;

  return ot_admin_checksum_version (variant);
 out:
  return NULL;
}

static gboolean
deployment_get_gpg_verify (OstreeDeployment *deployment,
                           OstreeRepo *repo)
{
  /* XXX Something like this could be added to the OstreeDeployment
   *     API in libostree if the OstreeRepo parameter is acceptable. */

  GKeyFile *origin;
  g_autofree char *refspec = NULL;
  g_autofree char *remote = NULL;
  gboolean gpg_verify = FALSE;

  origin = ostree_deployment_get_origin (deployment);

  if (origin == NULL)
    goto out;

  refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);

  if (refspec == NULL)
    goto out;

  if (!ostree_parse_refspec (refspec, &remote, NULL, NULL))
    goto out;

  if (remote)
    (void) ostree_repo_remote_get_gpg_verify (repo, remote, &gpg_verify, NULL);

out:
  return gpg_verify;
}

gboolean
ot_admin_builtin_status (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(OstreeSysroot) sysroot = NULL;
  gboolean ret = FALSE;
  g_autoptr(OstreeRepo) repo = NULL;
  OstreeDeployment *booted_deployment = NULL;
  g_autoptr(OstreeDeployment) pending_deployment = NULL;
  g_autoptr(OstreeDeployment) rollback_deployment = NULL;
  g_autoptr(GPtrArray) deployments = NULL;
  const int is_tty = isatty (1);
  const char *red_bold_prefix = is_tty ? "\x1b[31m\x1b[1m" : "";
  const char *red_bold_suffix = is_tty ? "\x1b[22m\x1b[0m" : "";
  guint i;

  context = g_option_context_new ("List deployments");

  if (!ostree_admin_option_context_parse (context, options, &argc, &argv,
                                          OSTREE_ADMIN_BUILTIN_FLAG_UNLOCKED,
                                          &sysroot, cancellable, error))
    goto out;

  if (!ostree_sysroot_get_repo (sysroot, &repo, cancellable, error))
    goto out;

  deployments = ostree_sysroot_get_deployments (sysroot);
  booted_deployment = ostree_sysroot_get_booted_deployment (sysroot);

  if (booted_deployment)
    ostree_sysroot_query_deployments_for (sysroot, NULL, &pending_deployment,
                                          &rollback_deployment);

  if (deployments->len == 0)
    {
      g_print ("No deployments.\n");
    }
  else
    {
      for (i = 0; i < deployments->len; i++)
        {
          OstreeDeployment *deployment = deployments->pdata[i];
          GKeyFile *origin;
          const char *ref = ostree_deployment_get_csum (deployment);
          OstreeDeploymentUnlockedState unlocked = ostree_deployment_get_unlocked (deployment);
          g_autofree char *version = version_of_commit (repo, ref);
          g_autoptr(OstreeGpgVerifyResult) result = NULL;
          guint jj, n_signatures;
          GError *local_error = NULL;

          origin = ostree_deployment_get_origin (deployment);

          const char *deployment_status = "";
          if (deployment == pending_deployment)
            deployment_status = " (pending)";
          else if (deployment == rollback_deployment)
            deployment_status = " (rollback)";
          g_print ("%c %s %s.%d%s\n",
                   deployment == booted_deployment ? '*' : ' ',
                   ostree_deployment_get_osname (deployment),
                   ostree_deployment_get_csum (deployment),
                   ostree_deployment_get_deployserial (deployment),
                   deployment_status);
          if (version)
            g_print ("    Version: %s\n", version);
          switch (unlocked)
            {
            case OSTREE_DEPLOYMENT_UNLOCKED_NONE:
              break;
            default:
              g_print ("    %sUnlocked: %s%s\n", red_bold_prefix,
                       ostree_deployment_unlocked_state_to_string (unlocked),
                       red_bold_suffix);
            }
          if (!origin)
            g_print ("    origin: none\n");
          else
            {
              g_autofree char *origin_refspec = g_key_file_get_string (origin, "origin", "refspec", NULL);
              if (!origin_refspec)
                g_print ("    origin: <unknown origin type>\n");
              else
                g_print ("    origin refspec: %s\n", origin_refspec);
            }

          if (deployment_get_gpg_verify (deployment, repo))
            {
              g_autoptr(GString) output_buffer = g_string_sized_new (256);
              /* Print any digital signatures on this commit. */

              result = ostree_repo_verify_commit_ext (repo, ref, NULL, NULL,
                                                      cancellable, &local_error);

              /* G_IO_ERROR_NOT_FOUND just means the commit is not signed. */
              if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                {
                  g_clear_error (&local_error);
                  continue;
                }
              else if (local_error != NULL)
                {
                  g_propagate_error (error, local_error);
                  goto out;
                }

              n_signatures = ostree_gpg_verify_result_count_all (result);

              for (jj = 0; jj < n_signatures; jj++)
                {
                  ostree_gpg_verify_result_describe (result, jj, output_buffer, "    GPG: ",
                                                     OSTREE_GPG_SIGNATURE_FORMAT_DEFAULT);
                }

              g_print ("%s", output_buffer->str);
            }
        }
    }

  ret = TRUE;
 out:
  return ret;
}
