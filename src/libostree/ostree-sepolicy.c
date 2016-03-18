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
#include "ostree-bootloader-uboot.h"
#include "ostree-bootloader-syslinux.h"

/**
 * SECTION:libostree-sepolicy
 * @title: SELinux policy management
 * @short_description: Read SELinux policy and manage filesystem labels
 *
 * A #OstreeSePolicy object can load the SELinux policy from a given
 * root and perform labeling.
 */
struct OstreeSePolicy {
  GObject parent;

  GFile *path;

  gboolean runtime_enabled;

#ifdef HAVE_SELINUX
  GFile *selinux_policy_root;
  struct selabel_handle *selinux_hnd;
  char *selinux_policy_name;
#endif
};

typedef struct {
  GObjectClass parent_class;
} OstreeSePolicyClass;

static void initable_iface_init       (GInitableIface      *initable_iface);

enum {
  PROP_0,

  PROP_PATH
};

G_DEFINE_TYPE_WITH_CODE (OstreeSePolicy, ostree_sepolicy, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init))

static void
ostree_sepolicy_finalize (GObject *object)
{
  OstreeSePolicy *self = OSTREE_SEPOLICY (object);

  g_clear_object (&self->path);
#ifdef HAVE_SELINUX
  g_clear_object (&self->selinux_policy_root);
  g_clear_pointer (&self->selinux_policy_name, g_free);
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
      /* Canonicalize */
      self->path = g_file_new_for_path (gs_file_get_path_cached (g_value_get_object (value)));
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
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_sepolicy_constructed (GObject *object)
{
  OstreeSePolicy *self = OSTREE_SEPOLICY (object);

  g_assert (self->path != NULL);

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
}

static gboolean
initable_init (GInitable     *initable,
               GCancellable  *cancellable,
               GError       **error)
{
#ifdef HAVE_SELINUX
  gboolean ret = FALSE;
  OstreeSePolicy *self = OSTREE_SEPOLICY (initable);
  g_autoptr(GFile) etc_selinux_dir = NULL;
  g_autoptr(GFile) policy_config_path = NULL;
  g_autoptr(GFile) policy_root = NULL;
  g_autoptr(GFileInputStream) filein = NULL;
  g_autoptr(GDataInputStream) datain = NULL;
  gboolean enabled = FALSE;
  char *policytype = NULL;
  const char *selinux_prefix = "SELINUX=";
  const char *selinuxtype_prefix = "SELINUXTYPE=";

  etc_selinux_dir = g_file_resolve_relative_path (self->path, "etc/selinux");
  if (!g_file_query_exists (etc_selinux_dir, NULL))
    {
      g_object_unref (etc_selinux_dir);
      etc_selinux_dir = g_file_resolve_relative_path (self->path, "usr/etc/selinux");
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

      self->selinux_policy_name = g_strdup (policytype);
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
}

static void
initable_iface_init (GInitableIface *initable_iface)
{
  initable_iface->init = initable_init;
}

/**
 * ostree_sepolicy_new:
 * @path: Path to a root directory
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
  gboolean ret = FALSE;
  int res;
  char *con = NULL;

  if (self->selinux_hnd)
    {
      res = selabel_lookup_raw (self->selinux_hnd, &con, relpath, unix_mode);
      if (res != 0)
        {
          if (errno != ENOENT)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
      else
        {
          /* Ensure we consistently allocate with g_malloc */
          *out_label = g_strdup (con);
          freecon (con);
        }
    }

  ret = TRUE;
 out:
  return ret;
#else
  return TRUE;
#endif
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
