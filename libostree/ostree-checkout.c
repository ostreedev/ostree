/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ostree.h"
#include "otutil.h"

enum {
  PROP_0,

  PROP_REPO,
  PROP_PATH
};

G_DEFINE_TYPE (OstreeCheckout, ostree_checkout, G_TYPE_OBJECT)

#define GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), OSTREE_TYPE_CHECKOUT, OstreeCheckoutPrivate))

typedef struct _OstreeCheckoutPrivate OstreeCheckoutPrivate;

struct _OstreeCheckoutPrivate {
  OstreeRepo *repo;
  char *path;
};

static void
ostree_checkout_finalize (GObject *object)
{
  OstreeCheckout *self = OSTREE_CHECKOUT (object);
  OstreeCheckoutPrivate *priv = GET_PRIVATE (self);

  g_free (priv->path);
  g_clear_object (&priv->repo);

  G_OBJECT_CLASS (ostree_checkout_parent_class)->finalize (object);
}

static void
ostree_checkout_set_property(GObject         *object,
			   guint            prop_id,
			   const GValue    *value,
			   GParamSpec      *pspec)
{
  OstreeCheckout *self = OSTREE_CHECKOUT (object);
  OstreeCheckoutPrivate *priv = GET_PRIVATE (self);

  switch (prop_id)
    {
    case PROP_PATH:
      priv->path = g_value_dup_string (value);
      break;
    case PROP_REPO:
      priv->repo = g_value_dup_object (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
ostree_checkout_get_property(GObject         *object,
			   guint            prop_id,
			   GValue          *value,
			   GParamSpec      *pspec)
{
  OstreeCheckout *self = OSTREE_CHECKOUT (object);
  OstreeCheckoutPrivate *priv = GET_PRIVATE (self);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_string (value, priv->path);
      break;
    case PROP_REPO:
      g_value_set_object (value, priv->repo);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static GObject *
ostree_checkout_constructor (GType                  gtype,
                           guint                  n_properties,
                           GObjectConstructParam *properties)
{
  GObject *object;
  GObjectClass *parent_class;
  OstreeCheckoutPrivate *priv;

  parent_class = G_OBJECT_CLASS (ostree_checkout_parent_class);
  object = parent_class->constructor (gtype, n_properties, properties);

  priv = GET_PRIVATE (object);

  g_assert (priv->path != NULL);
  
  return object;
}

static void
ostree_checkout_class_init (OstreeCheckoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (OstreeCheckoutPrivate));

  object_class->constructor = ostree_checkout_constructor;
  object_class->get_property = ostree_checkout_get_property;
  object_class->set_property = ostree_checkout_set_property;
  object_class->finalize = ostree_checkout_finalize;

  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path", "", "",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class,
                                   PROP_REPO,
                                   g_param_spec_object ("repo", "", "",
                                                        OSTREE_TYPE_REPO,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
ostree_checkout_init (OstreeCheckout *self)
{
}

OstreeCheckout*
ostree_checkout_new (OstreeRepo  *repo,
                     const char  *path)
{
  return g_object_new (OSTREE_TYPE_CHECKOUT, "repo", repo, "path", path, NULL);
}

static gboolean
executable_exists_in_checkout (const char *path,
                               const char *executable)
{
  int i;
  const char *subdirs[] = {"bin", "sbin", "usr/bin", "usr/sbin"};

  for (i = 0; i < G_N_ELEMENTS (subdirs); i++)
    {
      char *possible_path = g_build_filename (path, subdirs[i], executable, NULL);
      gboolean exists;
      
      exists = g_file_test (possible_path, G_FILE_TEST_EXISTS);
      g_free (possible_path);

      if (exists)
        return TRUE;
    }

  return FALSE;
}

static gboolean
run_trigger (OstreeCheckout *self,
             GFile          *trigger,
             gboolean        requires_chroot,
             GError        **error)
{
  OstreeCheckoutPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  char *path = NULL;
  char *temp_path = NULL;
  char *rel_temp_path = NULL;
  GFile *temp_copy = NULL;
  char *basename = NULL;
  GPtrArray *args = NULL;
  int estatus;

  path = g_file_get_path (trigger);
  basename = g_path_get_basename (path);

  args = g_ptr_array_new ();
  
  if (requires_chroot)
    {
      temp_path = g_build_filename (priv->path, basename, NULL);
      rel_temp_path = g_strconcat ("./", basename, NULL);
      temp_copy = ot_util_new_file_for_path (temp_path);

      if (!g_file_copy (trigger, temp_copy, 0, NULL, NULL, NULL, error))
        goto out;

      g_ptr_array_add (args, "chroot");
      g_ptr_array_add (args, ".");
      g_ptr_array_add (args, rel_temp_path);
      g_ptr_array_add (args, NULL);
    }
  else
    {
      g_ptr_array_add (args, path);
      g_ptr_array_add (args, NULL);
    }
      
  g_print ("Running trigger: %s\n", path);
  if (!g_spawn_sync (priv->path,
                     (char**)args->pdata,
                     NULL,
                     G_SPAWN_SEARCH_PATH,
                     NULL, NULL, NULL, NULL,
                     &estatus,
                     error))
    {
      g_prefix_error (error, "Failed to run trigger %s: ", basename);
      goto out;
    }

  ret = TRUE;
 out:
  if (requires_chroot && temp_path)
    (void)unlink (temp_path);
    
  g_free (path);
  g_free (basename);
  g_free (temp_path);
  g_free (rel_temp_path);
  g_clear_object (&temp_copy);
  if (args)
    g_ptr_array_free (args, TRUE);
  return ret;
}

static gboolean
check_trigger (OstreeCheckout *self,
               GFile          *trigger,
               GError        **error)
{
  OstreeCheckoutPrivate *priv = GET_PRIVATE (self);
  gboolean ret = FALSE;
  GInputStream *instream = NULL;
  GDataInputStream *datain = NULL;
  GError *temp_error = NULL;
  char *line;
  gsize len;
  gboolean requires_chroot = TRUE;
  gboolean matches = FALSE;

  instream = (GInputStream*)g_file_read (trigger, NULL, error);
  if (!instream)
    goto out;
  datain = g_data_input_stream_new (instream);

  while ((line = g_data_input_stream_read_line (datain, &len, NULL, &temp_error)) != NULL)
    {
      if (g_str_has_prefix (line, "# IfExecutable: "))
        {
          char *executable = g_strdup (line + strlen ("# IfExecutable: "));
          g_strchomp (executable);
          matches = executable_exists_in_checkout (priv->path, executable);
          g_free (executable);
        }

      g_free (line);
    }
  if (line == NULL && temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }
  if (matches)
    {
      if (!run_trigger (self, trigger, requires_chroot, error))
        goto out;
    }
  
  ret = TRUE;
 out:
  g_clear_object (&instream);
  g_clear_object (&datain);
  return ret;
}

gboolean
ostree_checkout_run_triggers (OstreeCheckout *self,
                              GError        **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  char *triggerdir_path = NULL;
  GFile *triggerdir = NULL;
  GFileInfo *file_info = NULL;
  GFileEnumerator *enumerator = NULL;

  triggerdir_path = g_build_filename (LIBEXECDIR, "ostree", "triggers.d", NULL);
  triggerdir = ot_util_new_file_for_path (triggerdir_path);

  enumerator = g_file_enumerate_children (triggerdir, "standard::name,standard::type,unix::*", 
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          NULL, 
                                          error);
  if (!enumerator)
    goto out;

  while ((file_info = g_file_enumerator_next_file (enumerator, NULL, &temp_error)) != NULL)
    {
      const char *name;
      guint32 type;
      char *child_path = NULL;
      GFile *child = NULL;
      gboolean success;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name"); 
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");
      
      if (type == G_FILE_TYPE_REGULAR && g_str_has_suffix (name, ".trigger"))
        {
          child_path = g_build_filename (triggerdir_path, name, NULL);
          child = ot_util_new_file_for_path (child_path);

          success = check_trigger (self, child, error);
        }
      else
        success = TRUE;

      g_object_unref (file_info);
      g_free (child_path);
      g_clear_object (&child);
      if (!success)
        goto out;
    }
  if (file_info == NULL && temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  g_free (triggerdir_path);
  g_clear_object (&triggerdir);
  g_clear_object (&enumerator);
  return ret;
}
