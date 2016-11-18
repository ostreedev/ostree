/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include "otutil.h"

#include "ostree-sysroot-upgrader.h"

/**
 * SECTION:ostree-sysroot-upgrader
 * @title: Simple upgrade class
 * @short_description: Upgrade OSTree systems
 *
 * The #OstreeSysrootUpgrader class allows performing simple upgrade
 * operations.
 */
typedef struct {
  GObjectClass parent_class;
} OstreeSysrootUpgraderClass;

struct OstreeSysrootUpgrader {
  GObject parent;

  OstreeSysroot *sysroot;
  char *osname;
  OstreeSysrootUpgraderFlags flags;

  OstreeDeployment *merge_deployment;
  GKeyFile *origin;
  char *origin_remote;
  char *origin_ref;
  char *override_csum;

  char *new_revision;
}; 

enum {
  PROP_0,

  PROP_SYSROOT,
  PROP_OSNAME,
  PROP_FLAGS
};

static void ostree_sysroot_upgrader_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (OstreeSysrootUpgrader, ostree_sysroot_upgrader, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, ostree_sysroot_upgrader_initable_iface_init))

static gboolean
parse_refspec (OstreeSysrootUpgrader  *self,
               GCancellable           *cancellable,
               GError                **error)
{
  gboolean ret = FALSE;
  g_autofree char *origin_refspec = NULL;
  g_autofree char *unconfigured_state = NULL;
  g_autofree char *csum = NULL;

  if ((self->flags & OSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED) == 0)
    {
      /* If explicit action by the OS creator is requried to upgrade, print their text as an error */
      unconfigured_state = g_key_file_get_string (self->origin, "origin", "unconfigured-state", NULL);
      if (unconfigured_state)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "origin unconfigured-state: %s", unconfigured_state);
          goto out;
        }
    }

  origin_refspec = g_key_file_get_string (self->origin, "origin", "refspec", NULL);
  if (!origin_refspec)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No origin/refspec in current deployment origin; cannot upgrade via ostree");
      goto out;
    }
  g_clear_pointer (&self->origin_remote, g_free);
  g_clear_pointer (&self->origin_ref, g_free);
  if (!ostree_parse_refspec (origin_refspec,
                             &self->origin_remote, 
                             &self->origin_ref,
                             error))
    goto out;

  csum = g_key_file_get_string (self->origin, "origin", "override-commit", NULL);
  if (csum != NULL && !ostree_validate_checksum_string (csum, error))
    goto out;
  g_clear_pointer (&self->override_csum, g_free);
  self->override_csum = g_steal_pointer (&csum);

  ret = TRUE;
 out:
  return ret;
}

static gboolean
ostree_sysroot_upgrader_initable_init (GInitable        *initable,
                                       GCancellable     *cancellable,
                                       GError          **error)
{
  gboolean ret = FALSE;
  OstreeSysrootUpgrader *self = (OstreeSysrootUpgrader*)initable;
  OstreeDeployment *booted_deployment =
    ostree_sysroot_get_booted_deployment (self->sysroot);

  if (booted_deployment == NULL && self->osname == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Not currently booted into an OSTree system and no OS specified");
      goto out;
    }

  if (self->osname == NULL)
    {
      g_assert (booted_deployment);
      self->osname = g_strdup (ostree_deployment_get_osname (booted_deployment));
    }
  else if (self->osname[0] == '\0')
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid empty osname");
      goto out;
    }

  self->merge_deployment = ostree_sysroot_get_merge_deployment (self->sysroot, self->osname); 
  if (self->merge_deployment == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No previous deployment for OS '%s'", self->osname);
      goto out;
    }

  self->origin = ostree_deployment_get_origin (self->merge_deployment);
  if (!self->origin)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No origin known for deployment %s.%d",
                   ostree_deployment_get_csum (self->merge_deployment),
                   ostree_deployment_get_deployserial (self->merge_deployment));
      goto out;
    }
  g_key_file_ref (self->origin);

  if (!parse_refspec (self, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static void
ostree_sysroot_upgrader_initable_iface_init (GInitableIface *iface)
{
  iface->init = ostree_sysroot_upgrader_initable_init;
}

static void
ostree_sysroot_upgrader_finalize (GObject *object)
{
  OstreeSysrootUpgrader *self = OSTREE_SYSROOT_UPGRADER (object);

  g_clear_object (&self->sysroot);
  g_free (self->osname);

  g_clear_object (&self->merge_deployment);
  if (self->origin)
    g_key_file_unref (self->origin);
  g_free (self->origin_remote);
  g_free (self->origin_ref);
  g_free (self->override_csum);
  g_free (self->new_revision);

  G_OBJECT_CLASS (ostree_sysroot_upgrader_parent_class)->finalize (object);
}

static void
ostree_sysroot_upgrader_set_property (GObject         *object,
                                      guint            prop_id,
                                      const GValue    *value,
                                      GParamSpec      *pspec)
{
  OstreeSysrootUpgrader *self = OSTREE_SYSROOT_UPGRADER (object);

  switch (prop_id)
    {
    case PROP_SYSROOT:
      self->sysroot = g_value_dup_object (value);
      break;
    case PROP_OSNAME:
      self->osname = g_value_dup_string (value);
      break;
    case PROP_FLAGS:
      self->flags = g_value_get_flags (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_sysroot_upgrader_get_property (GObject         *object,
                                      guint            prop_id,
                                      GValue          *value,
                                      GParamSpec      *pspec)
{
  OstreeSysrootUpgrader *self = OSTREE_SYSROOT_UPGRADER (object);

  switch (prop_id)
    {
    case PROP_SYSROOT:
      g_value_set_object (value, self->sysroot);
      break;
    case PROP_OSNAME:
      g_value_set_string (value, self->osname);
      break;
    case PROP_FLAGS:
      g_value_set_flags (value, self->flags);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_sysroot_upgrader_constructed (GObject *object)
{
  OstreeSysrootUpgrader *self = OSTREE_SYSROOT_UPGRADER (object);

  g_assert (self->sysroot != NULL);

  G_OBJECT_CLASS (ostree_sysroot_upgrader_parent_class)->constructed (object);
}

static void
ostree_sysroot_upgrader_class_init (OstreeSysrootUpgraderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = ostree_sysroot_upgrader_constructed;
  object_class->get_property = ostree_sysroot_upgrader_get_property;
  object_class->set_property = ostree_sysroot_upgrader_set_property;
  object_class->finalize = ostree_sysroot_upgrader_finalize;

  g_object_class_install_property (object_class,
                                   PROP_SYSROOT,
                                   g_param_spec_object ("sysroot", "", "",
                                                        OSTREE_TYPE_SYSROOT,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class,
                                   PROP_OSNAME,
                                   g_param_spec_string ("osname", "", "", NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class,
                                   PROP_FLAGS,
                                   g_param_spec_flags ("flags", "", "",
                                                       ostree_sysroot_upgrader_flags_get_type (),
                                                       0,
                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
ostree_sysroot_upgrader_init (OstreeSysrootUpgrader *self)
{
}

/**
 * ostree_sysroot_upgrader_new:
 * @sysroot: An #OstreeSysroot
 *
 * Returns: (transfer full): An upgrader
 */
OstreeSysrootUpgrader*
ostree_sysroot_upgrader_new (OstreeSysroot *sysroot,
                             GCancellable  *cancellable,
                             GError       **error)
{
  return g_initable_new (OSTREE_TYPE_SYSROOT_UPGRADER, cancellable, error,
                         "sysroot", sysroot, NULL);
}

/**
 * ostree_sysroot_upgrader_new_for_os:
 * @sysroot: An #OstreeSysroot
 * @osname: (allow-none): Operating system name
 *
 * Returns: (transfer full): An upgrader
 */
OstreeSysrootUpgrader*
ostree_sysroot_upgrader_new_for_os (OstreeSysroot *sysroot,
                                    const char    *osname,
                                    GCancellable  *cancellable,
                                    GError       **error)
{
  return g_initable_new (OSTREE_TYPE_SYSROOT_UPGRADER, cancellable, error,
                       "sysroot", sysroot, "osname", osname, NULL);
}

/**
 * ostree_sysroot_upgrader_new_for_os_with_flags:
 * @sysroot: An #OstreeSysroot
 * @osname: (allow-none): Operating system name
 * @flags: Flags
 *
 * Returns: (transfer full): An upgrader
 */
OstreeSysrootUpgrader *
ostree_sysroot_upgrader_new_for_os_with_flags (OstreeSysroot              *sysroot,
                                               const char                 *osname,
                                               OstreeSysrootUpgraderFlags  flags,
                                               GCancellable               *cancellable,
                                               GError                    **error)
{
  return g_initable_new (OSTREE_TYPE_SYSROOT_UPGRADER, cancellable, error,
                         "sysroot", sysroot, "osname", osname, "flags", flags, NULL);
}

/**
 * ostree_sysroot_upgrader_get_origin:
 * @self: Sysroot
 *
 * Returns: (transfer none): The origin file, or %NULL if unknown
 */
GKeyFile *
ostree_sysroot_upgrader_get_origin (OstreeSysrootUpgrader *self)
{
  return self->origin;
}

/**
 * ostree_sysroot_upgrader_dup_origin:
 * @self: Sysroot
 *
 * Returns: (transfer full): A copy of the origin file, or %NULL if unknown
 */
GKeyFile *
ostree_sysroot_upgrader_dup_origin (OstreeSysrootUpgrader *self)
{
  GKeyFile *copy = NULL;

  g_return_val_if_fail (OSTREE_IS_SYSROOT_UPGRADER (self), NULL);

  if (self->origin != NULL)
    {
      g_autofree char *data = NULL;
      gsize length = 0;

      copy = g_key_file_new ();
      data = g_key_file_to_data (self->origin, &length, NULL);
      g_key_file_load_from_data (copy, data, length,
                                 G_KEY_FILE_KEEP_COMMENTS, NULL);
    }

  return copy;
}

/**
 * ostree_sysroot_upgrader_set_origin:
 * @self: Sysroot
 * @origin: (allow-none): The new origin
 * @cancellable: Cancellable
 * @error: Error
 *
 * Replace the origin with @origin.
 */
gboolean
ostree_sysroot_upgrader_set_origin (OstreeSysrootUpgrader *self,
                                    GKeyFile              *origin,
                                    GCancellable          *cancellable,
                                    GError               **error)
{
  gboolean ret = FALSE;

  g_clear_pointer (&self->origin, g_key_file_unref);
  if (origin)
    {
      self->origin = g_key_file_ref (origin);
      if (!parse_refspec (self, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_sysroot_upgrader_get_origin_description:
 * @self: Upgrader
 *
 * Returns: A one-line descriptive summary of the origin, or %NULL if unknown
 */
char *
ostree_sysroot_upgrader_get_origin_description (OstreeSysrootUpgrader *self)
{
  if (!self->origin)
    return NULL;
  return g_key_file_get_string (self->origin, "origin", "refspec", NULL);
}

/**
 * ostree_sysroot_upgrader_check_timestamps:
 * @repo: Repo
 * @from_rev: From revision
 * @to_rev: To revision
 * @error: Error
 *
 * Check that the timestamp on @to_rev is equal to or newer than
 * @from_rev.  This protects systems against man-in-the-middle
 * attackers which provide a client with an older commit.
 */
gboolean
ostree_sysroot_upgrader_check_timestamps (OstreeRepo     *repo,
                                          const char     *from_rev,
                                          const char     *to_rev,
                                          GError        **error)
{
  gboolean ret = FALSE;
  g_autoptr(GVariant) old_commit = NULL;
  g_autoptr(GVariant) new_commit = NULL;

  if (!ostree_repo_load_variant (repo,
                                 OSTREE_OBJECT_TYPE_COMMIT,
                                 from_rev,
                                 &old_commit,
                                 error))
    goto out;
  
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 to_rev, &new_commit,
                                 error))
    goto out;

  if (ostree_commit_get_timestamp (old_commit) > ostree_commit_get_timestamp (new_commit))
    {
      GDateTime *old_ts = g_date_time_new_from_unix_utc (ostree_commit_get_timestamp (old_commit));
      GDateTime *new_ts = g_date_time_new_from_unix_utc (ostree_commit_get_timestamp (new_commit));
      g_autofree char *old_ts_str = NULL;
      g_autofree char *new_ts_str = NULL;

      g_assert (old_ts);
      g_assert (new_ts);
      old_ts_str = g_date_time_format (old_ts, "%c");
      new_ts_str = g_date_time_format (new_ts, "%c");
      g_date_time_unref (old_ts);
      g_date_time_unref (new_ts);

      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Upgrade target revision '%s' with timestamp '%s' is chronologically older than current revision '%s' with timestamp '%s'; use --allow-downgrade to permit",
                   to_rev, new_ts_str, from_rev, old_ts_str);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}


/**
 * ostree_sysroot_upgrader_pull:
 * @self: Upgrader
 * @flags: Flags controlling pull behavior
 * @upgrader_flags: Flags controlling upgrader behavior
 * @progress: (allow-none): Progress
 * @out_changed: (out): Whether or not the origin changed
 * @cancellable: Cancellable
 * @error: Error
 *
 * Perform a pull from the origin.  First check if the ref has
 * changed, if so download the linked objects, and store the updated
 * ref locally.  Then @out_changed will be %TRUE.
 *
 * If the origin remote is unchanged, @out_changed will be set to
 * %FALSE.
 */
gboolean
ostree_sysroot_upgrader_pull (OstreeSysrootUpgrader  *self,
                              OstreeRepoPullFlags     flags,
                              OstreeSysrootUpgraderPullFlags     upgrader_flags,
                              OstreeAsyncProgress    *progress,
                              gboolean               *out_changed,
                              GCancellable           *cancellable,
                              GError                **error)
{
  return ostree_sysroot_upgrader_pull_one_dir (self, NULL, flags, upgrader_flags, progress, out_changed, cancellable, error);
}

/**
 * ostree_sysroot_upgrader_pull_one_dir:
 *
 * Like ostree_sysroot_upgrader_pull(), but allows retrieving just a
 * subpath of the tree.  This can be used to download metadata files
 * from inside the tree such as package databases.
 *
 */
gboolean
ostree_sysroot_upgrader_pull_one_dir (OstreeSysrootUpgrader  *self,
                                      const char             *dir_to_pull,
                                      OstreeRepoPullFlags     flags,
                                      OstreeSysrootUpgraderPullFlags     upgrader_flags,
                                      OstreeAsyncProgress    *progress,
                                      gboolean               *out_changed,
                                      GCancellable           *cancellable,
                                      GError                **error)
{
  gboolean ret = FALSE;
  glnx_unref_object OstreeRepo *repo = NULL;
  char *refs_to_fetch[] = { NULL, NULL };
  const char *from_revision = NULL;
  g_autofree char *origin_refspec = NULL;

  if (self->override_csum != NULL)
    refs_to_fetch[0] = self->override_csum;
  else
    refs_to_fetch[0] = self->origin_ref;

  if (!ostree_sysroot_get_repo (self->sysroot, &repo, cancellable, error))
    goto out;

  if (self->origin_remote)
    origin_refspec = g_strconcat (self->origin_remote, ":", self->origin_ref, NULL);
  else
    origin_refspec = g_strdup (self->origin_ref);

  g_assert (self->merge_deployment);
  from_revision = ostree_deployment_get_csum (self->merge_deployment);

  if (self->origin_remote)
    {
      if (!ostree_repo_pull_one_dir (repo, self->origin_remote, dir_to_pull, refs_to_fetch,
                             flags, progress,
                             cancellable, error))
        goto out;

      if (progress)
        ostree_async_progress_finish (progress);
    }

  if (self->override_csum != NULL)
    {
      if (!ostree_repo_set_ref_immediate (repo,
                                          self->origin_remote,
                                          self->origin_ref,
                                          self->override_csum,
                                          cancellable,
                                          error))
        goto out;

      self->new_revision = g_strdup (self->override_csum);
    }
  else
    {
      if (!ostree_repo_resolve_rev (repo, origin_refspec, FALSE,
                                    &self->new_revision, error))
        goto out;

    }

  if (g_strcmp0 (from_revision, self->new_revision) == 0)
    {
      *out_changed = FALSE;
    }
  else
    {
      gboolean allow_older = (upgrader_flags & OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER) > 0;

      *out_changed = TRUE;

      if (from_revision && !allow_older)
        {
          if (!ostree_sysroot_upgrader_check_timestamps (repo, from_revision,
                                                         self->new_revision,
                                                         error))
            goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * ostree_sysroot_upgrader_deploy:
 * @self: Self
 * @cancellable: Cancellable
 * @error: Error
 *
 * Write the new deployment to disk, perform a configuration merge
 * with /etc, and update the bootloader configuration.
 */
gboolean
ostree_sysroot_upgrader_deploy (OstreeSysrootUpgrader  *self,
                                GCancellable           *cancellable,
                                GError                **error)
{
  gboolean ret = FALSE;
  glnx_unref_object OstreeDeployment *new_deployment = NULL;

  if (!ostree_sysroot_deploy_tree (self->sysroot, self->osname,
                                   self->new_revision,
                                   self->origin,
                                   self->merge_deployment,
                                   NULL,
                                   &new_deployment,
                                   cancellable, error))
    goto out;

  if (!ostree_sysroot_simple_write_deployment (self->sysroot, self->osname,
                                               new_deployment,
                                               self->merge_deployment,
                                               0,
                                               cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

GType
ostree_sysroot_upgrader_flags_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GFlagsValue values[] = {
        { OSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED, "OSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED", "ignore-unconfigured" },
        { 0, NULL, NULL }
      };
      GType g_define_type_id =
        g_flags_register_static (g_intern_static_string ("OstreeSysrootUpgraderFlags"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
