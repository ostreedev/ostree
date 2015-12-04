/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
 * Copyright (C) 2013 Stef Walter <stefw@redhat.com>
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
 * Author: Stef Walter <stefw@redhat.com>
 *         Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include <err.h>

#include "ot-dump.h"
#include "otutil.h"
#include "ot-admin-functions.h"

void
ot_dump_variant (GVariant *variant)
{
  g_autofree char *formatted_variant = NULL;
  g_autoptr(GVariant) byteswapped = NULL;

  if (G_BYTE_ORDER != G_BIG_ENDIAN)
    {
      byteswapped = g_variant_byteswap (variant);
      formatted_variant = g_variant_print (byteswapped, TRUE);
    }
  else
    {
      formatted_variant = g_variant_print (variant, TRUE);
    }
  g_print ("%s\n", formatted_variant);
}

static gchar *
format_timestamp (guint64  timestamp,
                  GError **error)
{
  GDateTime *dt;
  gchar *str;

  dt = g_date_time_new_from_unix_utc (timestamp);
  if (dt == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid timestamp: %" G_GUINT64_FORMAT, timestamp);
      return NULL;
    }

  str = g_date_time_format (dt, "%Y-%m-%d %H:%M:%S +0000");
  g_date_time_unref (dt);

  return str;
}

static void
dump_indented_lines (const gchar *data)
{
  const char* indent = "    ";
  const gchar *pos;

  for (;;)
    {
      pos = strchr (data, '\n');
      if (pos)
        {
          g_print ("%s%.*s", indent, (int)(pos + 1 - data), data);
          data = pos + 1;
        }
      else
        {
          if (data[0] != '\0')
            g_print ("%s%s\n", indent, data);
          break;
        }
    }
}

static void
dump_commit (GVariant            *variant,
             OstreeDumpFlags      flags)
{
  const gchar *subject;
  const gchar *body;
  guint64 timestamp;
  g_autofree char *str = NULL;
  g_autofree char *version = NULL;
  g_autoptr(GError) local_error = NULL;

  /* See OSTREE_COMMIT_GVARIANT_FORMAT */
  g_variant_get (variant, "(a{sv}aya(say)&s&stayay)", NULL, NULL, NULL,
                 &subject, &body, &timestamp, NULL, NULL);

  timestamp = GUINT64_FROM_BE (timestamp);
  str = format_timestamp (timestamp, &local_error);
  if (!str)
    errx (1, "Failed to read commit: %s", local_error->message);
  g_print ("Date:  %s\n", str);

  if ((version = ot_admin_checksum_version (variant)))
    {
      g_print ("Version: %s\n", version);
    }

  g_print ("\n");
  dump_indented_lines (subject);

  if (body[0])
    {
      g_print ("\n");
      dump_indented_lines (body);
    }
  g_print ("\n");
}

void
ot_dump_object (OstreeObjectType   objtype,
                const char        *checksum,
                GVariant          *variant,
                OstreeDumpFlags    flags)
{
  g_print ("%s %s\n", ostree_object_type_to_string (objtype), checksum);

  if (flags & OSTREE_DUMP_RAW)
    {
      ot_dump_variant (variant);
      return;
    }

  switch (objtype)
  {
    case OSTREE_OBJECT_TYPE_COMMIT:
      dump_commit (variant, flags);
      break;
    /* TODO: Others could be implemented here */
    default:
      break;
  }
}
