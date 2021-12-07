/*
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

#include "otutil.h"

#include <string.h>

static gboolean
is_notfound (GError *error)
{
  return g_error_matches (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND)
          || g_error_matches (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND);
}

gboolean
ot_keyfile_get_boolean_with_default (GKeyFile      *keyfile,
                                     const char    *section,
                                     const char    *value,
                                     gboolean       default_value,
                                     gboolean      *out_bool,
                                     GError       **error)
{
  g_return_val_if_fail (keyfile != NULL, FALSE);
  g_return_val_if_fail (section != NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  GError *temp_error = NULL;
  gboolean ret_bool = g_key_file_get_boolean (keyfile, section, value, &temp_error);
  if (temp_error)
    {
      if (is_notfound (temp_error))
        {
          g_clear_error (&temp_error);
          ret_bool = default_value;
        }
      else
        {
          g_propagate_error (error, temp_error);
          return FALSE;
        }
    }

  *out_bool = ret_bool;
  return TRUE;
}

gboolean
ot_keyfile_get_value_with_default (GKeyFile      *keyfile,
                                   const char    *section,
                                   const char    *value,
                                   const char    *default_value,
                                   char         **out_value,
                                   GError       **error)
{
  g_return_val_if_fail (keyfile != NULL, FALSE);
  g_return_val_if_fail (section != NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  GError *temp_error = NULL;
  g_autofree char *ret_value = g_key_file_get_value (keyfile, section, value, &temp_error);
  if (temp_error)
    {
      if (is_notfound (temp_error))
        {
          g_clear_error (&temp_error);
          g_assert (ret_value == NULL);
          ret_value = g_strdup (default_value);
        }
      else
        {
          g_propagate_error (error, temp_error);
          return FALSE;
        }
    }

  ot_transfer_out_value(out_value, &ret_value);
  return TRUE;
}

gboolean
ot_keyfile_get_value_with_default_group_optional (GKeyFile      *keyfile,
                                                  const char    *section,
                                                  const char    *value,
                                                  const char    *default_value,
                                                  char         **out_value,
                                                  GError       **error)
{
  g_return_val_if_fail (keyfile != NULL, FALSE);
  g_return_val_if_fail (section != NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  GError *local_error = NULL;
  g_autofree char *ret_value = NULL;
  if (!ot_keyfile_get_value_with_default (keyfile, section, value, default_value, &ret_value, &local_error))
    {
      if (g_error_matches (local_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND))
        {
          g_clear_error (&local_error);
          ret_value = g_strdup (default_value);
        }
      else
        {
          g_propagate_error (error, local_error);
          return FALSE;
        }
    }

  ot_transfer_out_value(out_value, &ret_value);
  return TRUE;
}

/* Read the value of key as a string.  If the value string contains
 * zero or one of the separators and none of the others, read the
 * string as a NULL-terminated array out_value.  If the value string
 * contains multiple of the separators, give an error.
 * 
 * Returns TRUE on success, FALSE on error. */
gboolean
ot_keyfile_get_string_list_with_separator_choice (GKeyFile      *keyfile,
                                                  const char    *section,
                                                  const char    *key,
                                                  const char    *separators,
                                                  char        ***out_value,
                                                  GError       **error)
{
  g_return_val_if_fail (keyfile != NULL, FALSE);
  g_return_val_if_fail (section != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);
  g_return_val_if_fail (separators != NULL, FALSE);

  g_autofree char  *value_str = NULL;
  if (!ot_keyfile_get_value_with_default (keyfile, section, key, NULL,
                                          &value_str, error))
    return FALSE;

  g_auto(GStrv) value_list = NULL;
  if (value_str)
    {
      gchar sep = '\0';
      guint sep_count = 0;
      for (size_t i = 0; i < strlen (separators) && sep_count <= 1; i++)
        {
          if (strchr (value_str, separators[i]))
            {
              sep_count++;
              sep = separators[i];
            }
        }

      if (sep_count == 0)
        {
          value_list = g_new (gchar *, 2);
          value_list[0] = g_steal_pointer (&value_str);
          value_list[1] = NULL;
        }
      else if (sep_count == 1)
        {
          if (!ot_keyfile_get_string_list_with_default (keyfile, section, key,
                                                        sep, NULL, &value_list, error))
            return FALSE;
        }
      else
        {
          return glnx_throw (error, "key value list contains more than one separator");
        }
    }

  ot_transfer_out_value (out_value, &value_list);
  return TRUE;
}

gboolean
ot_keyfile_get_string_list_with_default (GKeyFile      *keyfile,
                                         const char    *section,
                                         const char    *key,
                                         char           separator,
                                         char         **default_value,
                                         char        ***out_value,
                                         GError       **error)
{
  g_autoptr(GError) temp_error = NULL;

  g_return_val_if_fail (keyfile != NULL, FALSE);
  g_return_val_if_fail (section != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);

  g_key_file_set_list_separator (keyfile, separator);

  g_auto(GStrv) ret_value = g_key_file_get_string_list (keyfile, section,
                                                        key, NULL, &temp_error);

  if (temp_error)
    {
      if (is_notfound (temp_error))
        {
          g_clear_error (&temp_error);
          ret_value = g_strdupv (default_value);
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&temp_error));
          return FALSE;
        }
    }

  ot_transfer_out_value (out_value, &ret_value);
  return TRUE;
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
