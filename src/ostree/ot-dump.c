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

#include "ot-dump.h"
#include "otutil.h"

void
ot_dump_variant (GVariant *variant)
{
  gs_free char *formatted_variant = NULL;
  gs_unref_variant GVariant *byteswapped = NULL;

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
format_timestamp (guint64 timestamp)
{
  GDateTime *dt;
  gchar *str;

  dt = g_date_time_new_from_unix_utc (timestamp);
  if (dt == NULL)
    return NULL;

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
  gs_free gchar *str = NULL;

  /* See OSTREE_COMMIT_GVARIANT_FORMAT */
  g_variant_get (variant, "(a{sv}aya(say)&s&stayay)", NULL, NULL, NULL,
                 &subject, &body, &timestamp, NULL, NULL);

  timestamp = GUINT64_FROM_BE (timestamp);
  str = format_timestamp (timestamp);
  if (str)
    g_print ("Date:  %s\n", str);

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
                OstreeRepo        *repo,
                OstreeDumpFlags    flags)
{
  gs_free char *name = NULL;
  gs_free char *csum = g_strdup (checksum);
  gs_unref_object GFile *path_to_customs = NULL;
  g_print ("%s %s\n", ostree_object_type_to_string (objtype), checksum);

  if (flags & OSTREE_DUMP_RAW)
    {
      ot_dump_variant (variant);
      return;
    }

  switch (objtype)
  {
    case OSTREE_OBJECT_TYPE_COMMIT:
      path_to_customs = ot_gfile_resolve_path_printf (ostree_repo_get_path (repo), "state/custom_names");
      if (ostree_deployment_get_name (csum, path_to_customs, &name, NULL))
        g_print ("name: %s\n", name);
      dump_commit (variant, flags);
      break;
    /* TODO: Others could be implemented here */
    default:
      break;
  }
}
