/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

#include "otutil.h"

#include <string.h>

gboolean
ot_keyfile_get_boolean_with_default (GKeyFile      *keyfile,
                                     const char    *section,
                                     const char    *value,
                                     gboolean       default_value,
                                     gboolean      *out_bool,
                                     GError       **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  gboolean ret_bool;

  g_return_val_if_fail (keyfile != NULL, ret);
  g_return_val_if_fail (section != NULL, ret);
  g_return_val_if_fail (value != NULL, ret);

  ret_bool = g_key_file_get_boolean (keyfile, section, value, &temp_error);
  if (temp_error)
    {
      if (g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
        {
          g_clear_error (&temp_error);
          ret_bool = default_value;
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  ret = TRUE;
  *out_bool = ret_bool;
 out:
  return ret;
}

gboolean
ot_keyfile_get_value_with_default (GKeyFile      *keyfile,
                                   const char    *section,
                                   const char    *value,
                                   const char    *default_value,
                                   char         **out_value,
                                   GError       **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;
  g_autofree char *ret_value = NULL;

  g_return_val_if_fail (keyfile != NULL, ret);
  g_return_val_if_fail (section != NULL, ret);
  g_return_val_if_fail (value != NULL, ret);

  ret_value = g_key_file_get_value (keyfile, section, value, &temp_error);
  if (temp_error)
    {
      if (g_error_matches (temp_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
        {
          g_clear_error (&temp_error);
          ret_value = g_strdup (default_value);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }

  ret = TRUE;
  ot_transfer_out_value(out_value, &ret_value);
 out:
  return ret;
}

gboolean
ot_keyfile_copy_group (GKeyFile   *source_keyfile,
                       GKeyFile   *target_keyfile,
                       const char *group_name)
{
  g_auto(GStrv) keys = NULL;
  gsize length, ii;
  gboolean ret = FALSE;

  g_return_val_if_fail (source_keyfile != NULL, ret);
  g_return_val_if_fail (target_keyfile != NULL, ret);
  g_return_val_if_fail (group_name != NULL, ret);

  keys = g_key_file_get_keys (source_keyfile, group_name, &length, NULL);

  if (keys == NULL)
    goto out;

  for (ii = 0; ii < length; ii++)
    {
      g_autofree char *value = NULL;

      value = g_key_file_get_value (source_keyfile, group_name, keys[ii], NULL);
      g_key_file_set_value (target_keyfile, group_name, keys[ii], value);
    }

  ret = TRUE;

 out:
  return ret;
}
