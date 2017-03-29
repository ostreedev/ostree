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

#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#include <selinux/label.h>
#endif

#include "otutil.h"

#include "ostree-sepolicy.h"
#include "ostree-sepolicy-private.h"
#include "ostree-bootloader-uboot.h"
#include "ostree-bootloader-syslinux.h"

/**
 * SECTION:ostree-sepolicy
 * @title: SELinux policy management
 * @short_description: Read SELinux policy and manage filesystem labels
 *
 * A #OstreeSePolicy object can load the SELinux policy from a given
 * root and perform labeling.
 */
struct OstreeSePolicy {
  GObject parent;

  int rootfs_dfd;
  int rootfs_dfd_owned;
  GFile *path;

  gboolean runtime_enabled;

#ifdef HAVE_SELINUX
  GFile *selinux_policy_root;
  struct selabel_handle *selinux_hnd;
  char *selinux_policy_name;
  char *selinux_policy_csum;
#endif
};

typedef struct {
  GObjectClass parent_class;
} OstreeSePolicyClass;

static void initable_iface_init       (GInitableIface      *initable_iface);

enum {
  PROP_0,

  PROP_PATH,
  PROP_ROOTFS_DFD
};

G_DEFINE_TYPE_WITH_CODE (OstreeSePolicy, ostree_sepolicy, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init))

static void
ostree_sepolicy_finalize (GObject *object)
{
  OstreeSePolicy *self = OSTREE_SEPOLICY (object);

  g_clear_object (&self->path);
  if (self->rootfs_dfd_owned != -1)
    (void) close (self->rootfs_dfd_owned);
#ifdef HAVE_SELINUX
  g_clear_object (&self->selinux_policy_root);
  g_clear_pointer (&self->selinux_policy_name, g_free);
  g_clear_pointer (&self->selinux_policy_csum, g_free);
  if (self->selinux_hnd)
    {
      selabel_close (self->selinux_hnd);
      self->selinux_hnd = NULL;
    }
#endif

  G_OBJECT_CLASS (ostree_sepolicy_parent_class)->finalize (object);
}

static void
ostree_sepolicy_set_property(GObject         *object,
                            guint            prop_id,
                            const GValue    *value,
                            GParamSpec      *pspec)
{
  OstreeSePolicy *self = OSTREE_SEPOLICY (object);

  switch (prop_id)
    {
    case PROP_PATH:
      {
        GFile *path = g_value_get_object (value);
        if (path)
          {
            /* Canonicalize */
            self->path = g_file_new_for_path (gs_file_get_path_cached (path));
            g_assert_cmpint (self->rootfs_dfd, ==, -1);
          }
      }
      break;
    case PROP_ROOTFS_DFD:
      {
        int fd = g_value_get_int (value);
        if (fd != -1)
          {
            g_assert (self->path == NULL);
            self->rootfs_dfd = fd;
          }
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_sepolicy_get_property(GObject         *object,
                            guint            prop_id,
                            GValue          *value,
                            GParamSpec      *pspec)
{
  OstreeSePolicy *self = OSTREE_SEPOLICY (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_object (value, self->path);
      break;
    case PROP_ROOTFS_DFD:
      g_value_set_int (value, self->rootfs_dfd);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_sepolicy_constructed (GObject *object)
{
  OstreeSePolicy *self = OSTREE_SEPOLICY (object);

  g_assert (self->path != NULL || self->rootfs_dfd != -1);

  G_OBJECT_CLASS (ostree_sepolicy_parent_class)->constructed (object);
}

static void
ostree_sepolicy_class_init (OstreeSePolicyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = ostree_sepolicy_constructed;
  object_class->get_property = ostree_sepolicy_get_property;
  object_class->set_property = ostree_sepolicy_set_property;
  object_class->finalize = ostree_sepolicy_finalize;

  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_object ("path",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class,
                                   PROP_ROOTFS_DFD,
                                   g_param_spec_int ("rootfs-dfd",
                                                     "", "",
                                                     -1, G_MAXINT, -1,
                                                     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

}

#ifdef HAVE_SELINUX

/* Find the latest policy file in our root and return its checksum. */
static gboolean
get_policy_checksum (char        **out_csum,
                     GCancellable *cancellable,
                     GError      **error)
{
  gboolean ret = FALSE;

  const char *binary_policy_path = selinux_binary_policy_path ();
  const char *binfile_prefix = glnx_basename (binary_policy_path);
  g_autofree char *bindir_path = g_path_get_dirname (binary_policy_path);

  glnx_fd_close int bindir_dfd = -1;

  g_autofree char *best_policy = NULL;
  int best_version = 0;

  g_auto(GLnxDirFdIterator) dfd_iter = { 0,};

  if (!glnx_opendirat (AT_FDCWD, bindir_path, TRUE, &bindir_dfd, error))
    goto out;

  if (!glnx_dirfd_iterator_init_at (bindir_dfd, ".", FALSE, &dfd_iter, error))
    goto out;

  while (TRUE)
    {
      struct dirent *dent = NULL;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent,
                                                       cancellable, error))
        goto out;

      if (dent == NULL)
        break;

      if (dent->d_type == DT_REG)
        {
          /* We could probably save a few hundred nanoseconds if we accept that
           * the prefix will always be "policy" and hardcode that in a static
           * compile-once GRegex... But picture how exciting it'd be if it *did*
           * somehow change; there would be cheers & slow-mo high-fives at the
           * sight of our code not breaking. Is that hope not worth a fraction
           * of a millisecond? I believe it is... or maybe I'm just lazy. */
          g_autofree char *regex = g_strdup_printf ("^\\Q%s\\E\\.[0-9]+$",
                                                    binfile_prefix);

          /* we could use match groups to extract the version, but mehhh, we
           * already have the prefix on hand */
          if (g_regex_match_simple (regex, dent->d_name, 0, 0))
            {
              int version = /* do +1 for the period */
                (int)g_ascii_strtoll (dent->d_name + strlen (binfile_prefix)+1,
                                      NULL, 10);
              g_assert (version > 0);

              if (version > best_version)
                {
                  best_version = version;
                  g_free (best_policy);
                  best_policy = g_strdup (dent->d_name);
                }
            }
        }
    }

  if (!best_policy)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not find binary policy file");
      goto out;
    }

  *out_csum = ot_checksum_file_at (bindir_dfd, best_policy, G_CHECKSUM_SHA256,
                                   cancellable, error);
  if (*out_csum == NULL)
    goto out;

  ret = TRUE;
out:
  return ret;
}

#endif

static gboolean
initable_init (GInitable     *initable,
               GCancellable  *cancellable,
               GError       **error)
{
#ifdef HAVE_SELINUX
  gboolean ret = FALSE;
  OstreeSePolicy *self = OSTREE_SEPOLICY (initable);
  g_autoptr(GFile) path = NULL;
  g_autoptr(GFile) etc_selinux_dir = NULL;
  g_autoptr(GFile) policy_config_path = NULL;
  g_autoptr(GFile) policy_root = NULL;
  g_autoptr(GFileInputStream) filein = NULL;
  g_autoptr(GDataInputStream) datain = NULL;
  gboolean enabled = FALSE;
  g_autofree char *policytype = NULL;
  const char *selinux_prefix = "SELINUX=";
  const char *selinuxtype_prefix = "SELINUXTYPE=";

  /* TODO - use this below */
  if (self->rootfs_dfd != -1)
    path = ot_fdrel_to_gfile (self->rootfs_dfd, ".");
  else if (self->path)
    {
      path = g_object_ref (self->path);
#if 0
      /* TODO - use this below */
      if (!glnx_opendirat (AT_FDCWD, gs_file_get_path_cached (self->path), TRUE,
                           &self->rootfs_dfd_owned, error))
        goto out;
      self->rootfs_dfd = self->rootfs_dfd_owned;
#endif
    }
  else
    g_assert_not_reached ();

  etc_selinux_dir = g_file_resolve_relative_path (path, "etc/selinux");
  if (!g_file_query_exists (etc_selinux_dir, NULL))
    {
      g_object_unref (etc_selinux_dir);
      etc_selinux_dir = g_file_resolve_relative_path (path, "usr/etc/selinux");
    }
  policy_config_path = g_file_get_child (etc_selinux_dir, "config");

  if (g_file_query_exists (policy_config_path, NULL))
    {
      filein = g_file_read (policy_config_path, cancellable, error);
      if (!filein)
        goto out;

      datain = g_data_input_stream_new ((GInputStream*)filein);

      while (TRUE)
        {
          gsize len;
          GError *temp_error = NULL;
          g_autofree char *line = g_data_input_stream_read_line_utf8 (datain, &len,
                                                                   cancellable, &temp_error);
      
          if (temp_error)
            {
              g_propagate_error (error, temp_error);
              goto out;
            }

          if (!line)
            break;
      
          if (g_str_has_prefix (line, selinuxtype_prefix))
            {
              policytype = g_strstrip (g_strdup (line + strlen (selinuxtype_prefix))); 
              policy_root = g_file_get_child (etc_selinux_dir, policytype);
            }
          else if (g_str_has_prefix (line, selinux_prefix))
            {
              const char *enabled_str = line + strlen (selinux_prefix);
              if (g_ascii_strncasecmp (enabled_str, "enforcing", strlen ("enforcing")) == 0 ||
                  g_ascii_strncasecmp (enabled_str, "permissive", strlen ("permissive")) == 0)
                enabled = TRUE;
            }
        }
    }

  if (enabled)
    {
      self->runtime_enabled = is_selinux_enabled () == 1;

      g_setenv ("LIBSELINUX_DISABLE_PCRE_PRECOMPILED", "1", FALSE);
      if (selinux_set_policy_root (gs_file_get_path_cached (policy_root)) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "selinux_set_policy_root(%s): %s",
                       gs_file_get_path_cached (etc_selinux_dir),
                       strerror (errno));
          goto out;
        }

      self->selinux_hnd = selabel_open (SELABEL_CTX_FILE, NULL, 0);
      if (!self->selinux_hnd)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "With policy root '%s': selabel_open(SELABEL_CTX_FILE): %s",
                       gs_file_get_path_cached (etc_selinux_dir),
                       strerror (errno));
          goto out;
        }

      {
        char *con = NULL;
        if (selabel_lookup_raw (self->selinux_hnd, &con, "/", 0755) != 0)
          {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "With policy root '%s': Failed to look up context of /: %s",
                         gs_file_get_path_cached (etc_selinux_dir),
                         strerror (errno));
            goto out;
          }
        freecon (con);
      }

      if (!get_policy_checksum (&self->selinux_policy_csum, cancellable, error))
        {
          g_prefix_error (error, "While calculating SELinux checksum: ");
          goto out;
        }

      self->selinux_policy_name = g_steal_pointer (&policytype);
      self->selinux_policy_root = g_object_ref (etc_selinux_dir);
    }

  ret = TRUE;
 out:
  return ret;
#else
  return TRUE;
#endif
}

static void
ostree_sepolicy_init (OstreeSePolicy *self)
{
  self->rootfs_dfd = -1;
  self->rootfs_dfd_owned = -1;
}

static void
initable_iface_init (GInitableIface *initable_iface)
{
  initable_iface->init = initable_init;
}

/**
 * ostree_sepolicy_new:
 * @path: Path to a root directory
 * @cancellable: Cancellable
 * @error: Error
 *
 * Returns: (transfer full): An accessor object for SELinux policy in root located at @path
 */
OstreeSePolicy*
ostree_sepolicy_new (GFile         *path,
                     GCancellable  *cancellable,
                     GError       **error)
{
  return g_initable_new (OSTREE_TYPE_SEPOLICY, cancellable, error, "path", path, NULL);
}

/**
 * ostree_sepolicy_new_at:
 * @rootfs_dfd: Directory fd for rootfs (will not be cloned)
 * @cancellable: Cancellable
 * @error: Error
 *
 * Returns: (transfer full): An accessor object for SELinux policy in root located at @rootfs_dfd
 */
OstreeSePolicy*
ostree_sepolicy_new_at (int         rootfs_dfd,
                        GCancellable  *cancellable,
                        GError       **error)
{
  return g_initable_new (OSTREE_TYPE_SEPOLICY, cancellable, error, "rootfs-dfd", rootfs_dfd, NULL);
}

/**
 * ostree_sepolicy_get_path:
 * @self:
 *
 * Returns: (transfer none): Path to rootfs
 */
GFile *
ostree_sepolicy_get_path (OstreeSePolicy  *self)
{
  return self->path;
}

/**
 * ostree_sepolicy_get_name:
 * @self:
 *
 * Returns: (transfer none): Type of current policy
 */
const char *
ostree_sepolicy_get_name (OstreeSePolicy *self)
{
#ifdef HAVE_SELINUX
  return self->selinux_policy_name;
#else
  return NULL;
#endif
}

/**
 * ostree_sepolicy_get_csum:
 * @self:
 *
 * Returns: (transfer none): Checksum of current policy
 */
const char *
ostree_sepolicy_get_csum (OstreeSePolicy *self)
{
#ifdef HAVE_SELINUX
  return self->selinux_policy_csum;
#else
  return NULL;
#endif
}

/**
 * ostree_sepolicy_get_label:
 * @self: Self
 * @relpath: Path
 * @unix_mode: Unix mode
 * @out_label: (allow-none) (out) (transfer full): Return location for security context
 * @cancellable: Cancellable
 * @error: Error
 *
 * Store in @out_label the security context for the given @relpath and
 * mode @unix_mode.  If the policy does not specify a label, %NULL
 * will be returned.
 */
gboolean
ostree_sepolicy_get_label (OstreeSePolicy    *self,
                           const char       *relpath,
                           guint32           unix_mode,
                           char            **out_label,
                           GCancellable     *cancellable,
                           GError          **error)
{
#ifdef HAVE_SELINUX
  /* Early return if no policy */
  if (!self->selinux_hnd)
    return TRUE;

  /* http://marc.info/?l=selinux&m=149082134430052&w=2
   * https://github.com/ostreedev/ostree/pull/768
   */
  if (strcmp (relpath, "/proc") == 0)
    relpath = "/mnt";

  char *con = NULL;
  int res = selabel_lookup_raw (self->selinux_hnd, &con, relpath, unix_mode);
  if (res != 0)
    {
      if (errno == ENOENT)
        *out_label = NULL;
      else
        return glnx_throw_errno (error);
    }
  else
    {
      /* Ensure we consistently allocate with g_malloc */
      *out_label = g_strdup (con);
      freecon (con);
    }

#endif
  return TRUE;
}

/**
 * ostree_sepolicy_restorecon:
 * @self: Self
 * @path: Path string to use for policy lookup
 * @info: (allow-none): File attributes
 * @target: Physical path to target file
 * @flags: Flags controlling behavior
 * @out_new_label: (allow-none) (out): New label, or %NULL if unchanged
 * @cancellable: Cancellable
 * @error: Error
 *
 * Reset the security context of @target based on the SELinux policy.
 */
gboolean
ostree_sepolicy_restorecon (OstreeSePolicy    *self,
                            const char       *path,
                            GFileInfo        *info,
                            GFile            *target,
                            OstreeSePolicyRestoreconFlags flags,
                            char            **out_new_label,
                            GCancellable     *cancellable,
                            GError          **error)
{
#ifdef HAVE_SELINUX
  gboolean ret = FALSE;
  g_autoptr(GFileInfo) src_info = NULL;
  g_autofree char *label = NULL;
  gboolean do_relabel = TRUE;

  if (info != NULL)
    src_info = g_object_ref (info);
  else
    {
      src_info = g_file_query_info (target, "unix::mode",
                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                    cancellable, error);
      if (!src_info)
        goto out;
    }

  if (flags & OSTREE_SEPOLICY_RESTORECON_FLAGS_KEEP_EXISTING)
    {
      char *existing_con = NULL;
      if (lgetfilecon_raw (gs_file_get_path_cached (target), &existing_con) > 0
          && existing_con)
        {
          do_relabel = FALSE;
          freecon (existing_con);
        }
    }

  if (do_relabel)
    {
      if (!ostree_sepolicy_get_label (self, path, 
                                      g_file_info_get_attribute_uint32 (src_info, "unix::mode"),
                                      &label,
                                      cancellable, error))
        goto out;

      if (!label)
        {
          if (!(flags & OSTREE_SEPOLICY_RESTORECON_FLAGS_ALLOW_NOLABEL))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "No label found for '%s'", path);
              goto out;
            }
        }
      else
        {
          int res = lsetfilecon (gs_file_get_path_cached (target), label);
          if (res != 0)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
    }

  ret = TRUE;
  if (out_new_label)
    *out_new_label = g_steal_pointer (&label);
 out:
  return ret;
#else
  return TRUE;
#endif
}

/**
 * ostree_sepolicy_setfscreatecon:
 * @self: Policy
 * @path: Use this path to determine a label
 * @mode: Used along with @path
 * @error: Error
 *
 */
gboolean
ostree_sepolicy_setfscreatecon (OstreeSePolicy   *self,
                                const char       *path,
                                guint32           mode,
                                GError          **error)
{
#ifdef HAVE_SELINUX
  gboolean ret = FALSE;
  g_autofree char *label = NULL;

  /* setfscreatecon() will bomb out if the host has SELinux disabled,
   * but we're enabled for the target system.  This is kind of a
   * broken scenario...for now, we'll silently ignore the label
   * request.  To correctly handle the case of disabled host but
   * enabled target will require nontrivial work.
   */
  if (!self->runtime_enabled)
    return TRUE;

  if (!ostree_sepolicy_get_label (self, path, mode, &label, NULL, error))
    goto out;

  if (setfscreatecon_raw (label) != 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  ret = TRUE;
 out:
  return ret;
#else
  return TRUE;
#endif
}

/**
 * ostree_sepolicy_fscreatecon_cleanup:
 * @unused: Not used, just in case you didn't infer that from the parameter name
 *
 * Cleanup function for ostree_sepolicy_setfscreatecon().
 */
void
ostree_sepolicy_fscreatecon_cleanup (void **unused)
{
#ifdef HAVE_SELINUX
  setfscreatecon (NULL);
#endif
}

/* Currently private copy of the older sepolicy/fscreatecon API with a nicer
 * g_auto() cleanup. May be made public later.
 */
gboolean
_ostree_sepolicy_preparefscreatecon (OstreeSepolicyFsCreatecon *con,
                                     OstreeSePolicy   *self,
                                     const char       *path,
                                     guint32           mode,
                                     GError          **error)
{
  if (!self || ostree_sepolicy_get_name (self) == NULL)
    return TRUE;

  if (!ostree_sepolicy_setfscreatecon (self, path, mode, error))
    return FALSE;

  con->initialized = TRUE;
  return TRUE;
}

void
_ostree_sepolicy_fscreatecon_clear (OstreeSepolicyFsCreatecon *con)
{
  if (!con->initialized)
    return;
  ostree_sepolicy_fscreatecon_cleanup (NULL);
}
