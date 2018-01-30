/*
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
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

#include "otutil.h"

#include "ostree.h"
#include "ostree-sysroot-upgrader.h"
#include "ostree-core-private.h"

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
  g_autofree char *origin_refspec = NULL;
  g_autofree char *unconfigured_state = NULL;
  g_autofree char *csum = NULL;

  if ((self->flags & OSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED) == 0)
    {
      /* If explicit action by the OS creator is requried to upgrade, print their text as an error.
       * NOTE: If changing this, see the matching implementation in ostree-repo-pull.c.
       */
      unconfigured_state = g_key_file_get_string (self->origin, "origin", "unconfigured-state", NULL);
      if (unconfigured_state)
        return glnx_throw (error, "origin unconfigured-state: %s", unconfigured_state);
    }

  origin_refspec = g_key_file_get_string (self->origin, "origin", "refspec", NULL);
  if (!origin_refspec)
    return glnx_throw (error, "No origin/refspec in current deployment origin; cannot upgrade via ostree");

  g_clear_pointer (&self->origin_remote, g_free);
  g_clear_pointer (&self->origin_ref, g_free);
  if (!ostree_parse_refspec (origin_refspec,
                             &self->origin_remote,
                             &self->origin_ref,
                             error))
    return FALSE;

  csum = g_key_file_get_string (self->origin, "origin", "override-commit", NULL);
  if (csum != NULL && !ostree_validate_checksum_string (csum, error))
    return FALSE;
  g_clear_pointer (&self->override_csum, g_free);
  self->override_csum = g_steal_pointer (&csum);

  return TRUE;
}

static gboolean
ostree_sysroot_upgrader_initable_init (GInitable        *initable,
                                       GCancellable     *cancellable,
                                       GError          **error)
{
  OstreeSysrootUpgrader *self = (OstreeSysrootUpgrader*)initable;
  OstreeDeployment *booted_deployment =
    ostree_sysroot_get_booted_deployment (self->sysroot);

  if (booted_deployment == NULL && self->osname == NULL)
    return glnx_throw (error, "Not currently booted into an OSTree system and no OS specified");

  if (self->osname == NULL)
    {
      g_assert (booted_deployment);
      self->osname = g_strdup (ostree_deployment_get_osname (booted_deployment));
    }
  else if (self->osname[0] == '\0')
    return glnx_throw (error, "Invalid empty osname");

  self->merge_deployment = ostree_sysroot_get_merge_deployment (self->sysroot, self->osname);
  if (self->merge_deployment == NULL)
    return glnx_throw (error, "No previous deployment for OS '%s'", self->osname);

  self->origin = ostree_deployment_get_origin (self->merge_deployment);
  if (!self->origin)
    return glnx_throw (error, "No origin known for deployment %s.%d",
                       ostree_deployment_get_csum (self->merge_deployment),
                       ostree_deployment_get_deployserial (self->merge_deployment));
  g_key_file_ref (self->origin);

  if (!parse_refspec (self, cancellable, error))
    return FALSE;

  return TRUE;
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
 * @cancellable: Cancellable
 * @error: Error
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
 * @cancellable: Cancellable
 * @error: Error
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
 * @cancellable: Cancellable
 * @error: Error
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
  g_clear_pointer (&self->origin, g_key_file_unref);
  if (origin)
    {
      self->origin = g_key_file_ref (origin);
      if (!parse_refspec (self, cancellable, error))
        return FALSE;
    }

  return TRUE;
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
  g_autoptr(GVariant) old_commit = NULL;
  if (!ostree_repo_load_variant (repo,
                                 OSTREE_OBJECT_TYPE_COMMIT,
                                 from_rev,
                                 &old_commit,
                                 error))
    return FALSE;

  g_autoptr(GVariant) new_commit = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT,
                                 to_rev, &new_commit,
                                 error))
    return FALSE;

  if (!_ostree_compare_timestamps (from_rev, ostree_commit_get_timestamp (old_commit),
                                   to_rev, ostree_commit_get_timestamp (new_commit),
                                   error))
    return FALSE;

  return TRUE;
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
 * @self: Upgrader
 * @dir_to_pull: Subdirectory path (should include a leading /)
 * @flags: Flags controlling pull behavior
 * @upgrader_flags: Flags controlling upgrader behavior
 * @progress: (allow-none): Progress
 * @out_changed: (out): Whether or not the origin changed
 * @cancellable: Cancellable
 * @error: Error
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
  g_autoptr(OstreeRepo) repo = NULL;
  char *refs_to_fetch[] = { NULL, NULL };
  const char *from_revision = NULL;
  g_autofree char *origin_refspec = NULL;
  g_autofree char *new_revision = NULL;
  g_autoptr(GVariant) new_variant = NULL;
  g_autoptr(GVariant) new_metadata = NULL;
  g_autoptr(GVariant) rebase = NULL;

  if (self->override_csum != NULL)
    refs_to_fetch[0] = self->override_csum;
  else
    refs_to_fetch[0] = self->origin_ref;

  if (!ostree_sysroot_get_repo (self->sysroot, &repo, cancellable, error))
    return FALSE;

  if (self->origin_remote)
    origin_refspec = g_strconcat (self->origin_remote, ":", self->origin_ref, NULL);
  else
    origin_refspec = g_strdup (self->origin_ref);

  g_assert (self->merge_deployment);
  from_revision = ostree_deployment_get_csum (self->merge_deployment);

  if (self->origin_remote &&
      (upgrader_flags & OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_SYNTHETIC) == 0)
    {
      g_autoptr(GVariantBuilder) optbuilder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
      if (dir_to_pull && *dir_to_pull)
        g_variant_builder_add (optbuilder, "{s@v}", "subdir",
                               g_variant_new_variant (g_variant_new_string (dir_to_pull)));
      g_variant_builder_add (optbuilder, "{s@v}", "flags",
                             g_variant_new_variant (g_variant_new_int32 (flags)));
      /* Add the timestamp check, unless disabled */
      if ((upgrader_flags & OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_ALLOW_OLDER) == 0)
        g_variant_builder_add (optbuilder, "{s@v}", "timestamp-check",
                               g_variant_new_variant (g_variant_new_boolean (TRUE)));

      g_variant_builder_add (optbuilder, "{s@v}", "refs",
                             g_variant_new_variant (g_variant_new_strv ((const char *const*) refs_to_fetch, -1)));
      g_autoptr(GVariant) opts = g_variant_ref_sink (g_variant_builder_end (optbuilder));
      if (!ostree_repo_pull_with_options (repo, self->origin_remote,
                                          opts, progress,
                                          cancellable, error))
        return FALSE;

      if (progress)
        ostree_async_progress_finish (progress);
    }

  /* Check to see if the commit marks the ref as EOL, redirecting to
   * another. */
  if (!ostree_repo_resolve_rev (repo, origin_refspec, FALSE,
                                &new_revision, error))
    return FALSE;

  if (!ostree_repo_load_variant (repo,
                                 OSTREE_OBJECT_TYPE_COMMIT,
                                 new_revision,
                                 &new_variant,
                                 error))
    return FALSE;

  g_variant_get_child (new_variant, 0, "@a{sv}", &new_metadata);
  rebase = g_variant_lookup_value (new_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE_REBASE, G_VARIANT_TYPE_STRING);
  if (rebase)
    {
      const char *new_ref = g_variant_get_string (rebase, 0);

      /* Pull the new ref */
      if (self->origin_remote &&
          (upgrader_flags & OSTREE_SYSROOT_UPGRADER_PULL_FLAGS_SYNTHETIC) == 0)
        {
          refs_to_fetch[0] = (char *) new_ref;
          if (!ostree_repo_pull_one_dir (repo, self->origin_remote, dir_to_pull, refs_to_fetch,
                                         flags, progress, cancellable, error))
            return FALSE;
        }

        /* Use the new ref for the rest of the update process */
        g_free (self->origin_ref);
        self->origin_ref = g_strdup(new_ref);
        g_free (origin_refspec);

        if (self->origin_remote)
          origin_refspec = g_strconcat (self->origin_remote, ":", new_ref, NULL);
        else
          origin_refspec = g_strdup (new_ref);

        g_key_file_set_string (self->origin, "origin", "refspec", origin_refspec);
    }

  if (self->override_csum != NULL)
    {
      if (!ostree_repo_set_ref_immediate (repo,
                                          self->origin_remote,
                                          self->origin_ref,
                                          self->override_csum,
                                          cancellable,
                                          error))
        return FALSE;

      self->new_revision = g_strdup (self->override_csum);
    }
  else
    {
      if (!ostree_repo_resolve_rev (repo, origin_refspec, FALSE,
                                    &self->new_revision, error))
        return FALSE;

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
            return FALSE;
        }
    }

  return TRUE;
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
  g_autoptr(OstreeDeployment) new_deployment = NULL;
  if (!ostree_sysroot_deploy_tree (self->sysroot, self->osname,
                                   self->new_revision,
                                   self->origin,
                                   self->merge_deployment,
                                   NULL,
                                   &new_deployment,
                                   cancellable, error))
    return FALSE;

  if (!ostree_sysroot_simple_write_deployment (self->sysroot, self->osname,
                                               new_deployment,
                                               self->merge_deployment,
                                               0,
                                               cancellable, error))
    return FALSE;

  return TRUE;
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
