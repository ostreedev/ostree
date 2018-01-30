/*
 * Copyright © 2016 Kinvolk GmbH
 * Copyright © 2017 Endless Mobile, Inc.
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
 * Authors:
 *  - Krzesimir Nowak <krzesimir@kinvolk.io>
 *  - Philip Withnall <withnall@endlessm.com>
 */

#include "config.h"

#include <avahi-common/strlst.h>
#include <glib.h>
#include <glib-object.h>
#include <string.h>

#include "ostree-autocleanups.h"
#include "ostree-repo-finder-avahi.h"
#include "ostree-repo-finder-avahi-private.h"

/* Reference: RFC 6763, §6. */
static gboolean
parse_txt_record (const guint8  *txt,
                  gsize          txt_len,
                  const gchar  **key,
                  gsize         *key_len,
                  const guint8 **value,
                  gsize         *value_len)
{
  gsize i;

  g_return_val_if_fail (key != NULL, FALSE);
  g_return_val_if_fail (key_len != NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);
  g_return_val_if_fail (value_len != NULL, FALSE);

  /* RFC 6763, §6.1. */
  if (txt_len > 8900)
    return FALSE;

  *key = (const gchar *) txt;
  *key_len = 0;
  *value = NULL;
  *value_len = 0;

  for (i = 0; i < txt_len; i++)
    {
      if (txt[i] >= 0x20 && txt[i] <= 0x7e && txt[i] != '=')
        {
          /* Key character. */
          *key_len = *key_len + 1;
          continue;
        }
      else if (*key_len > 0 && txt[i] == '=')
        {
          /* Separator. */
          *value = txt + (i + 1);
          *value_len = txt_len - (i + 1);
          return TRUE;
        }
      else
        {
          return FALSE;
        }
    }

  /* The entire TXT record is the key; there is no ‘=’ or value. */
  *value = NULL;
  *value_len = 0;

  return (*key_len > 0);
}

/* TODO: Docs. Return value is only valid as long as @txt is. Reference: RFC 6763, §6. */
GHashTable *
_ostree_txt_records_parse (AvahiStringList *txt)
{
  AvahiStringList *l;
  g_autoptr(GHashTable) out = NULL;

  out = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);

  for (l = txt; l != NULL; l = avahi_string_list_get_next (l))
    {
      const guint8 *txt;
      gsize txt_len;
      const gchar *key;
      const guint8 *value;
      gsize key_len, value_len;
      g_autofree gchar *key_allocated = NULL;
      g_autoptr(GBytes) value_allocated = NULL;

      txt = avahi_string_list_get_text (l);
      txt_len = avahi_string_list_get_size (l);

      if (!parse_txt_record (txt, txt_len, &key, &key_len, &value, &value_len))
        {
          g_debug ("Ignoring invalid TXT record of length %" G_GSIZE_FORMAT,
                   txt_len);
          continue;
        }

      key_allocated = g_ascii_strdown (key, key_len);

      if (g_hash_table_lookup_extended (out, key_allocated, NULL, NULL))
        {
          g_debug ("Ignoring duplicate TXT record ‘%s’", key_allocated);
          continue;
        }

      /* Distinguish between the case where the entire record is the key
       * (value == NULL) and the case where the record is the key + ‘=’ and the
       * value is empty (value != NULL && value_len == 0). */
      if (value != NULL)
        value_allocated = g_bytes_new_static (value, value_len);

      g_hash_table_insert (out, g_steal_pointer (&key_allocated), g_steal_pointer (&value_allocated));
    }

  return g_steal_pointer (&out);
}
